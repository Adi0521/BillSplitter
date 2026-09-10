#include "routes/payment_routes.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>

using json = nlohmann::json;

namespace {

constexpr std::size_t MAX_METHOD_LEN = 50;
constexpr std::size_t MAX_NOTES_LEN  = 1000;

// ── Response helpers ─────────────────────────────────────────────────────────

crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// ── Input helpers ────────────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    const char* ws = " \t\n\r\f\v";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

// Path parameters are raw user input. Casting a non-uuid string to uuid makes
// Postgres throw, which would surface as a 500, so the shape is checked here
// and anything malformed is simply "not found" — the same answer a well-formed
// but unknown id gets, which also avoids leaking whether an id was plausible.
bool is_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

// Reads an optional string field. A field present but neither string nor null
// is an error; JSON null reads as "".
bool read_string_field(const json&  body,
                       const char*  key,
                       std::string& out,
                       std::string& err) {
    const auto it = body.find(key);
    if (it == body.end()) return true;
    if (it->is_null()) {
        out.clear();
        return true;
    }
    if (!it->is_string()) {
        err = std::string(key) + " must be a string";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

// Money never becomes a C++ double: the amount travels as text from the client
// straight into the NUMERIC(12,4) column, so nothing but Postgres ever rounds
// it. A client that sent a JSON number has already lost the exact decimal to
// nlohmann's parse; dump() gives the shortest round-trip form back rather than
// a printf conversion that would invent digits.
bool read_money_field(const json&  body,
                      const char*  key,
                      std::string& out,
                      std::string& err) {
    const auto it = body.find(key);
    if (it == body.end()) return true;
    if (it->is_null()) return true;   // stays "", which validate_amount rejects
    if (it->is_string()) {
        out = it->get<std::string>();
        return true;
    }
    if (it->is_number()) {
        out = it->dump();
        return true;
    }
    err = std::string(key) + " must be a numeric string, like \"12.50\"";
    return false;
}

// An unsigned decimal with at most 8 integer digits and at most 4 decimal
// places, so it fits NUMERIC(12,4) exactly and Postgres is never asked to
// silently round someone's money. Strictly greater than zero: the column's
// CHECK (amount > 0) would otherwise throw and surface as a 500, and a zero
// payment records nothing anyway.
bool validate_amount(std::string& value, std::string& err) {
    value = trim(value);
    if (value.empty()) {
        // "" is rejected rather than read as zero. A cleared field or a typo
        // must not quietly become a real number in someone's ledger.
        err = "amount is required and must be a numeric string, like \"12.50\"";
        return false;
    }
    if (value[0] == '-') {
        err = "amount must be greater than 0";
        return false;
    }

    const auto dot = value.find('.');
    const std::string int_part = value.substr(0, dot);
    const std::string frac_part =
        (dot == std::string::npos) ? "" : value.substr(dot + 1);

    const auto digits_only = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        });
    };

    if (!digits_only(int_part) ||
        (dot != std::string::npos && !digits_only(frac_part))) {
        err = "amount must be a decimal number, like \"12.50\"";
        return false;
    }
    if (frac_part.size() > 4) {
        err = "amount must have at most 4 decimal places";
        return false;
    }

    std::string significant = int_part;
    significant.erase(0, std::min(significant.find_first_not_of('0'),
                                  significant.size()));
    if (significant.size() > 8) {
        err = "amount must be at most 99999999.9999";
        return false;
    }
    // Zero is checked on the digits, not by parsing to a number, so "0.0000"
    // and "000" are rejected without a float ever touching the value.
    if (significant.empty() &&
        frac_part.find_first_not_of('0') == std::string::npos) {
        err = "amount must be greater than 0";
        return false;
    }
    return true;
}

bool validate_currency(std::string& currency, std::string& err) {
    currency = to_upper(trim(currency));
    // ISO 4217 codes are three ASCII letters. A length check alone would admit
    // "US1" or "u$d", which then flow into the FX lookups of a later phase.
    if (currency.size() != 3 ||
        !std::all_of(currency.begin(), currency.end(),
                     [](unsigned char c) { return c >= 'A' && c <= 'Z'; })) {
        err = "currency must be a 3-letter code, like USD";
        return false;
    }
    return true;
}

// A required member id. Absent, null, blank or malformed are all 400: the split
// in the URL was found, so the body is what is wrong. The schema permits NULL
// in both columns, but a payment with no sender or no recipient cannot affect a
// balance and is only ever a bug, so the API refuses to create one.
bool read_member_field(const json&  body,
                       const char*  key,
                       std::string& out,
                       std::string& err) {
    const auto it = body.find(key);
    if (it == body.end() || it->is_null() || !it->is_string()) {
        err = std::string(key) + " is required and must be a member uuid";
        return false;
    }
    out = trim(it->get<std::string>());
    if (!is_uuid(out)) {
        err = std::string(key) + " is required and must be a member uuid";
        return false;
    }
    return true;
}

// Optional free text. Blank stores SQL NULL, matching how a member's email
// treats "" — an empty note is "no note", and having two ways to say that
// invites them to disagree.
bool read_optional_text(const json&        body,
                        const char*        key,
                        std::size_t        max_len,
                        std::optional<std::string>& out,
                        std::string&       err) {
    std::string value;
    if (!read_string_field(body, key, value, err)) return false;
    value = trim(value);
    if (value.empty()) return true;
    if (value.size() > max_len) {
        err = std::string(key) + " must be at most " +
              std::to_string(max_len) + " characters";
        return false;
    }
    out = value;
    return true;
}

// ── SQL ──────────────────────────────────────────────────────────────────────

// Shared column list, so the positional indices used by payment_to_json() are
// the same everywhere. from_name/to_name are joined in so the UI never has to
// render a bare uuid, and the joins are LEFT because ON DELETE SET NULL means a
// removed member leaves the payment behind with a null id.
const char* const PAYMENT_COLUMNS =
    "p.id, p.split_id, p.from_member, fm.name, p.to_member, tm.name, "
    "p.amount, p.currency, p.method, p.notes, p.paid_at";

// `text` for a nullable column, JSON null when it is NULL. Never "": the two
// mean different things and a client forced to treat both as absent will one
// day treat only one of them as absent.
json text_or_null(const pqxx::field& f) {
    return f.is_null() ? json(nullptr) : json(f.as<std::string>());
}

// Every NUMERIC is read with as<std::string>() and emitted as a JSON string.
// Reading it as a double here would defeat the entire representation.
json payment_to_json(const pqxx::row& r) {
    json j;
    j["id"]          = r[0].as<std::string>();
    j["split_id"]    = r[1].as<std::string>();
    j["from_member"] = text_or_null(r[2]);
    j["from_name"]   = text_or_null(r[3]);
    j["to_member"]   = text_or_null(r[4]);
    j["to_name"]     = text_or_null(r[5]);
    j["amount"]      = r[6].as<std::string>();
    j["currency"]    = r[7].as<std::string>();
    j["method"]      = text_or_null(r[8]);
    j["notes"]       = text_or_null(r[9]);
    j["paid_at"]     = text_or_null(r[10]);
    return j;
}

}  // namespace

void register_payment_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/splits/<id>/payments ────────────────────────────────────────
    // 200 [Payment], newest first. 404 when the split does not exist or is not
    // the caller's.
    CROW_ROUTE(app, "/api/splits/<string>/payments").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& split_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        if (!is_uuid(split_id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // Driving off splits with a LEFT JOIN makes the ownership check and
            // the list one round trip: zero rows means "no such split for this
            // user" (404), while a single row whose payment id is NULL means
            // "owned, but no payments yet" (200 []). Ownership lives in the
            // WHERE clause, so another user's split can never produce a row.
            auto rows = txn.exec(
                std::string("SELECT ") + PAYMENT_COLUMNS +
                "  FROM splits s"
                "  LEFT JOIN payments p ON p.split_id = s.id"
                "  LEFT JOIN split_members fm ON fm.id = p.from_member"
                "  LEFT JOIN split_members tm ON tm.id = p.to_member"
                " WHERE s.id = $1::uuid AND s.owner_id = $2::uuid"
                " ORDER BY p.paid_at DESC, p.id",
                pqxx::params{split_id, user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, "Split not found");

            json out = json::array();
            for (const auto& r : rows) {
                if (r[0].is_null()) continue;   // owned split, no payments
                out.push_back(payment_to_json(r));
            }

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "payment_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/splits/<id>/payments ───────────────────────────────────────
    // Body: { from_member, to_member, amount, currency?, method?, notes? }
    // → 201 Payment
    CROW_ROUTE(app, "/api/splits/<string>/payments").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req, const std::string& split_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        if (!is_uuid(split_id)) return json_error(404, "Split not found");

        try {
            auto body = json::parse(req.body);
            if (!body.is_object()) {
                return json_error(400, "Request body must be a JSON object");
            }

            std::string err;
            std::string from_member, to_member, amount, currency;
            std::optional<std::string> method, notes;

            if (!read_member_field(body, "from_member", from_member, err) ||
                !read_member_field(body, "to_member", to_member, err) ||
                !read_money_field(body, "amount", amount, err) ||
                !read_string_field(body, "currency", currency, err) ||
                !read_optional_text(body, "method", MAX_METHOD_LEN, method, err) ||
                !read_optional_text(body, "notes", MAX_NOTES_LEN, notes, err)) {
                return json_error(400, err);
            }

            // Paying yourself is always a mistake, and it would silently
            // distort the balances rather than fail loudly, so it is refused
            // here rather than stored and explained away later.
            if (from_member == to_member) {
                return json_error(400,
                    "from_member and to_member must be different members");
            }
            if (!validate_amount(amount, err)) return json_error(400, err);

            // "" means "unset" for currency — that is what an untouched
            // <select> submits — and inherits the split's currency below.
            const bool currency_given = !trim(currency).empty();
            if (currency_given && !validate_currency(currency, err)) {
                return json_error(400, err);
            }

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto split = txn.exec(
                "SELECT currency FROM splits"
                " WHERE id = $1::uuid AND owner_id = $2::uuid",
                pqxx::params{split_id, user->id});
            if (split.empty()) {
                txn.commit();
                return json_error(404, "Split not found");
            }
            // An omitted currency inherits the split's, not a hardcoded USD.
            if (!currency_given) currency = split[0][0].as<std::string>();

            // Both members must belong to *this* split, proven in SQL. The
            // split was found, so an id from another split is a bad body, not a
            // missing resource: 400, not 404. Two EXISTS in one round trip so
            // the message can name which side is wrong.
            auto membership = txn.exec(
                "SELECT"
                "  EXISTS(SELECT 1 FROM split_members"
                "          WHERE id = $1::uuid AND split_id = $3::uuid),"
                "  EXISTS(SELECT 1 FROM split_members"
                "          WHERE id = $2::uuid AND split_id = $3::uuid)",
                pqxx::params{from_member, to_member, split_id});
            if (!membership[0][0].as<bool>()) {
                txn.commit();
                return json_error(400,
                    "from_member must be a member of this split");
            }
            if (!membership[0][1].as<bool>()) {
                txn.commit();
                return json_error(400,
                    "to_member must be a member of this split");
            }

            // Ownership is re-asserted in the INSERT itself rather than trusted
            // from the SELECT above, so the row cannot come into existence for
            // a split the caller does not own.
            auto inserted = txn.exec(
                "INSERT INTO payments"
                "    (split_id, from_member, to_member, amount, currency,"
                "     method, notes)"
                "  SELECT s.id, $2::uuid, $3::uuid, $4::numeric, $5, $6, $7"
                "    FROM splits s"
                "   WHERE s.id = $1::uuid AND s.owner_id = $8::uuid"
                "  RETURNING id",
                pqxx::params{split_id, from_member, to_member, amount, currency,
                             method, notes, user->id});
            if (inserted.empty()) {
                txn.commit();
                return json_error(404, "Split not found");
            }

            auto rows = txn.exec(
                std::string("SELECT ") + PAYMENT_COLUMNS +
                "  FROM payments p"
                "  JOIN splits s ON s.id = p.split_id"
                "  LEFT JOIN split_members fm ON fm.id = p.from_member"
                "  LEFT JOIN split_members tm ON tm.id = p.to_member"
                " WHERE p.id = $1::uuid AND p.split_id = $2::uuid"
                "   AND s.owner_id = $3::uuid",
                pqxx::params{inserted[0][0].as<std::string>(), split_id,
                             user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, "Payment not found");

            res.code = 201;
            res.body = payment_to_json(rows[0]).dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "payment_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "payment_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── DELETE /api/splits/<id>/payments/<pid> ───────────────────────────────
    // 200 {"message":"Deleted"}. 404 when the split is not the caller's OR the
    // payment does not belong to that split.
    CROW_ROUTE(app, "/api/splits/<string>/payments/<string>")
        .methods(crow::HTTPMethod::DELETE)
    ([&pool](const crow::request& req,
             const std::string& split_id,
             const std::string& payment_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        // Either id may be arbitrary user input; a malformed one is a 404
        // rather than a Postgres cast error surfacing as a 500.
        if (!is_uuid(split_id) || !is_uuid(payment_id)) {
            return json_error(404, "Payment not found");
        }

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // Three conditions, all in the statement: the payment is this id,
            // its split is this split, and that split is the caller's. A
            // payment id from someone else's split therefore matches nothing
            // and is indistinguishable from one that never existed.
            auto rows = txn.exec(
                "DELETE FROM payments p"
                "  USING splits s"
                " WHERE p.id = $1::uuid"
                "   AND p.split_id = s.id"
                "   AND s.id = $2::uuid"
                "   AND s.owner_id = $3::uuid"
                " RETURNING p.id",
                pqxx::params{payment_id, split_id, user->id});

            if (rows.empty()) {
                txn.abort();
                return json_error(404, "Payment not found");
            }
            txn.commit();

            res.code = 200;
            res.body = R"({"message":"Deleted"})";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "payment_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
