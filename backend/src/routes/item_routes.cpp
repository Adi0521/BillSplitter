#include "routes/item_routes.h"
#include "auth/auth.h"
#include "auth/middleware.h"
#include "models/user.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr size_t MAX_NAME_LEN     = 200;
constexpr size_t MAX_MONEY_INT_DIGITS = 8;  // 99999999.9999 is the ceiling
constexpr size_t MAX_MONEY_DECIMALS   = 4;  // NUMERIC(12,4)
constexpr std::int64_t MIN_QUANTITY = 1;
constexpr std::int64_t MAX_QUANTITY = 100000;

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    const char* ws = " \t\n\r\f\v";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// Path parameters are user input and are interpolated into `$n::uuid` casts.
// Postgres raises invalid_text_representation for a malformed uuid, which would
// surface as a 500, so ids are validated here first and a bad one is simply
// "not found" — the same answer an unknown id gets.
bool is_uuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

// Resolves the session user, or nullopt when the request is unauthenticated.
//
crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// ── Money ─────────────────────────────────────────────────────────────────────
//
// Money never passes through a double in this module. The wire form is a
// string ("12.50"); a JSON number is accepted for convenience but is handled as
// text, and the text is what is handed to Postgres as `$n::numeric`. The only
// arithmetic on an amount happens in the database, in NUMERIC.

// Recovers the literal text of a JSON money value.
//
// Integers are printed from their exact integer value. A JSON float has already
// been through a double by the time nlohmann hands it over — there is no raw
// token to recover at that point — so dump() (shortest round-tripping form) is
// used, and any value whose text does not land inside the 4-decimal domain is
// rejected below rather than being silently rounded. Clients that care about
// exactness send a string, which is passed through untouched.
bool money_text(const json& v, std::string& out, std::string& err) {
    if (v.is_string()) {
        out = trim(v.get<std::string>());
        return true;
    }
    if (v.is_number_unsigned()) {
        out = std::to_string(v.get<std::uint64_t>());
        return true;
    }
    if (v.is_number_integer()) {
        out = std::to_string(v.get<std::int64_t>());
        return true;
    }
    if (v.is_number_float()) {
        out = v.dump();
        return true;
    }
    err = "price must be a numeric string, like \"12.50\"";
    return false;
}

// Validates a plain decimal amount and rewrites it in the canonical form
// Postgres will store (leading zeros dropped, fraction padded to 4 places), so
// what is sent to the database is exactly what comes back out.
bool normalize_money(std::string& text, const std::string& field, std::string& err) {
    const std::string in = text;
    size_t i = 0;

    if (i < in.size() && (in[i] == '-' || in[i] == '+')) {
        if (in[i] == '-') {
            err = field + " must not be negative";
            return false;
        }
        ++i;
    }

    std::string int_part;
    while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i]))) {
        int_part += in[i++];
    }

    std::string frac_part;
    if (i < in.size() && in[i] == '.') {
        ++i;
        while (i < in.size() && std::isdigit(static_cast<unsigned char>(in[i]))) {
            frac_part += in[i++];
        }
    }

    // Anything left over is not a plain decimal: exponent notation, currency
    // symbols, thousands separators, "12.5.5", or an empty string.
    if (i != in.size() || (int_part.empty() && frac_part.empty())) {
        err = field + " must be a decimal number, like \"12.50\"";
        return false;
    }
    if (frac_part.size() > MAX_MONEY_DECIMALS) {
        err = field + " must have at most 4 decimal places";
        return false;
    }

    auto first_significant = int_part.find_first_not_of('0');
    int_part = (first_significant == std::string::npos)
                   ? "0"
                   : int_part.substr(first_significant);
    if (int_part.size() > MAX_MONEY_INT_DIGITS) {
        err = field + " must be at most 99999999.9999";
        return false;
    }

    frac_part.append(MAX_MONEY_DECIMALS - frac_part.size(), '0');
    text = int_part + "." + frac_part;
    return true;
}

bool read_price(const json& body, std::string& price, std::string& err) {
    if (!money_text(body.at("price"), price, err)) return false;
    return normalize_money(price, "price", err);
}

bool read_name(const json& body, std::string& name, std::string& err) {
    const auto& v = body.at("name");
    if (!v.is_string()) {
        err = "name must be a string";
        return false;
    }
    name = trim(v.get<std::string>());
    if (name.empty()) {
        err = "name is required";
        return false;
    }
    if (name.size() > MAX_NAME_LEN) {
        err = "name must be 1-200 characters";
        return false;
    }
    return true;
}

bool read_quantity(const json& body, int& quantity, std::string& err) {
    const auto& v = body.at("quantity");
    // Rejects floats and bools: 2.0 and true are not quantities.
    if (!v.is_number_integer()) {
        err = "quantity must be a whole number";
        return false;
    }
    std::int64_t q;
    if (v.is_number_unsigned()) {
        std::uint64_t u = v.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(MAX_QUANTITY)) {
            err = "quantity must be between 1 and 100000";
            return false;
        }
        q = static_cast<std::int64_t>(u);
    } else {
        q = v.get<std::int64_t>();
    }
    if (q < MIN_QUANTITY || q > MAX_QUANTITY) {
        err = "quantity must be between 1 and 100000";
        return false;
    }
    quantity = static_cast<int>(q);
    return true;
}

bool read_currency(const json& body, std::string& currency, std::string& err) {
    const auto& v = body.at("currency");
    if (!v.is_string()) {
        err = "currency must be a string";
        return false;
    }
    currency = trim(v.get<std::string>());
    std::transform(currency.begin(), currency.end(), currency.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (currency.size() != 3 ||
        !std::all_of(currency.begin(), currency.end(),
                     [](unsigned char c) { return std::isalpha(c) != 0; })) {
        err = "currency must be a 3-letter code, like USD";
        return false;
    }
    return true;
}

// subtotal / total / line_total are derived from the items; a client that sends
// one is working from a stale model of who owns the number, and silently
// ignoring it would hide that.
bool rejects_derived_field(const json& body, std::string& err) {
    for (const char* field : {"line_total", "subtotal", "total"}) {
        if (body.contains(field)) {
            err = std::string(field) + " is derived by the server and cannot be set";
            return true;
        }
    }
    return false;
}

// The item columns, qualified with `prefix` ("", "i." or "bill_items.").
// UPDATE ... FROM bills, splits makes an unqualified `name` ambiguous with
// splits.name, so the prefix is not optional there. Only identifiers are
// concatenated here — never a value.
std::string item_columns(const std::string& p) {
    return p + "id, " + p + "bill_id, " + p + "name, " +
           p + "price::text AS price, " + p + "quantity, " + p + "currency, (" +
           p + "price * " + p + "quantity)::text AS line_total";
}

// NUMERIC is read as text and emitted as a JSON string: "12.5000", never 12.5.
json item_to_json(const pqxx::row& r) {
    json j;
    j["id"]         = r["id"].as<std::string>();
    j["bill_id"]    = r["bill_id"].as<std::string>();
    j["name"]       = r["name"].as<std::string>();
    j["price"]      = r["price"].as<std::string>();
    j["quantity"]   = r["quantity"].as<int>();
    j["currency"]   = r["currency"].as<std::string>();
    j["line_total"] = r["line_total"].as<std::string>();
    return j;
}

}  // namespace

void register_item_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/bills/<bid>/items ────────────────────────────────────────────
    // 200 [BillItem] in insertion order. 404 if the bill does not exist or its
    // split is not the caller's.
    CROW_ROUTE(app, "/api/bills/<string>/items").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& bill_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        if (!is_uuid(bill_id)) return json_error(404, "Bill not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // The join to splits is the authorization: the bill is only visible
            // through a split this user owns. LEFT JOIN to the items so one
            // round trip distinguishes the two "empty" answers — zero rows means
            // no such bill for this user (404), while one row with a NULL item
            // id means an owned bill with no items yet (200 []).
            auto rows = txn.exec(
                "SELECT " + item_columns("i.") +
                "  FROM bills b"
                "  JOIN splits s ON s.id = b.split_id"
                "  LEFT JOIN bill_items i ON i.bill_id = b.id"
                " WHERE b.id = $1::uuid AND s.owner_id = $2::uuid"
                " ORDER BY i.created_at, i.id",
                pqxx::params{bill_id, user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, "Bill not found");

            json out = json::array();
            for (const auto& r : rows) {
                if (r["id"].is_null()) continue;  // owned bill, no items
                out.push_back(item_to_json(r));
            }

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "item_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/bills/<bid>/items ───────────────────────────────────────────
    // Body: { name, price, quantity?, currency? } → 201 BillItem
    CROW_ROUTE(app, "/api/bills/<string>/items").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req, const std::string& bill_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        if (!is_uuid(bill_id)) return json_error(404, "Bill not found");

        try {
            auto body = json::parse(req.body);
            if (!body.is_object()) {
                return json_error(400, "Request body must be a JSON object");
            }

            // Validate everything before opening a transaction.
            std::string err;
            if (rejects_derived_field(body, err)) return json_error(400, err);

            if (!body.contains("name")) return json_error(400, "name is required");
            std::string name;
            if (!read_name(body, name, err)) return json_error(400, err);

            if (!body.contains("price")) return json_error(400, "price is required");
            std::string price;
            if (!read_price(body, price, err)) return json_error(400, err);

            int quantity = 1;
            if (body.contains("quantity") && !body["quantity"].is_null()) {
                if (!read_quantity(body, quantity, err)) return json_error(400, err);
            }

            // Omitted or null currency inherits the bill's, resolved in SQL so
            // the parent bill is read in the same statement that writes.
            std::optional<std::string> currency;
            if (body.contains("currency") && !body["currency"].is_null()) {
                std::string c;
                if (!read_currency(body, c, err)) return json_error(400, err);
                currency = c;
            }

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // INSERT ... SELECT: the row only comes into existence if the CTE
            // finds this bill on a split owned by this user, so ownership is
            // enforced by the statement itself. No rows inserted means the bill
            // is not the caller's (or does not exist) — 404, never 403.
            auto rows = txn.exec(
                "WITH target AS ("
                "    SELECT b.id, b.currency"
                "      FROM bills b"
                "      JOIN splits s ON s.id = b.split_id"
                "     WHERE b.id = $1::uuid AND s.owner_id = $2::uuid)"
                " INSERT INTO bill_items (bill_id, name, price, quantity, currency)"
                " SELECT target.id, $3::text, $4::numeric, $5::int,"
                "        COALESCE($6::text, target.currency)"
                "   FROM target"
                " RETURNING " + item_columns(""),
                pqxx::params{bill_id, user->id, name, price, quantity, currency});

            if (rows.empty()) {
                txn.abort();
                return json_error(404, "Bill not found");
            }

            json created = item_to_json(rows[0]);
            txn.commit();

            res.code = 201;
            res.body = created.dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "item_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const pqxx::data_exception& e) {
            // A value Postgres cannot represent in NUMERIC(12,4) — the caller's
            // number is the problem, not the server's, so 400 rather than 500.
            CROW_LOG_WARNING << "item_routes: numeric out of range: " << e.what();
            return json_error(400, "Amount out of range");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "item_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── PUT /api/bills/<bid>/items/<iid> ──────────────────────────────────────
    // Body: any subset of { name, price, quantity, currency }. Omitted fields
    // are left unchanged; an empty body is a 400.
    CROW_ROUTE(app, "/api/bills/<string>/items/<string>").methods(crow::HTTPMethod::PUT)
    ([&pool](const crow::request& req,
             const std::string& bill_id,
             const std::string& item_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        if (!is_uuid(bill_id) || !is_uuid(item_id)) {
            return json_error(404, "Item not found");
        }

        try {
            auto body = json::parse(req.body);
            if (!body.is_object()) {
                return json_error(400, "Request body must be a JSON object");
            }

            std::string err;
            if (rejects_derived_field(body, err)) return json_error(400, err);

            // Only the fields actually present are written, so omitted fields
            // keep their stored value. Placeholder names are concatenated into
            // the SQL; values only ever travel as parameters.
            std::vector<std::string> assignments;
            pqxx::params params;
            int n = 1;

            if (body.contains("name")) {
                std::string name;
                if (!read_name(body, name, err)) return json_error(400, err);
                assignments.push_back("name = $" + std::to_string(n++) + "::text");
                params.append(name);
            }
            if (body.contains("price")) {
                std::string price;
                if (!read_price(body, price, err)) return json_error(400, err);
                assignments.push_back("price = $" + std::to_string(n++) + "::numeric");
                params.append(price);
            }
            if (body.contains("quantity")) {
                int quantity = 1;
                if (!read_quantity(body, quantity, err)) return json_error(400, err);
                assignments.push_back("quantity = $" + std::to_string(n++) + "::int");
                params.append(quantity);
            }
            if (body.contains("currency")) {
                if (body["currency"].is_null()) {
                    // Explicit null means "back to the bill's currency", the
                    // same default POST applies when the field is omitted.
                    assignments.push_back("currency = b.currency");
                } else {
                    std::string currency;
                    if (!read_currency(body, currency, err)) return json_error(400, err);
                    assignments.push_back("currency = $" + std::to_string(n++) + "::text");
                    params.append(currency);
                }
            }

            if (assignments.empty()) {
                return json_error(400,
                    "No updatable fields provided; expected at least one of: "
                    "name, price, quantity, currency");
            }

            std::string set_clause;
            for (std::size_t i = 0; i < assignments.size(); ++i) {
                if (i > 0) set_clause += ", ";
                set_clause += assignments[i];
            }

            const std::string item_param  = "$" + std::to_string(n++);
            const std::string bill_param  = "$" + std::to_string(n++);
            const std::string owner_param = "$" + std::to_string(n++);
            params.append(item_id);
            params.append(bill_id);
            params.append(user->id);

            // Three conditions, all in the statement: the item is this id, it
            // belongs to this bill, and that bill's split is the caller's. An
            // item id from another bill — even another bill this same user owns
            // — therefore matches nothing and reads as 404.
            const std::string sql =
                "UPDATE bill_items SET " + set_clause +
                "  FROM bills b, splits s"
                " WHERE bill_items.id = " + item_param + "::uuid"
                "   AND bill_items.bill_id = " + bill_param + "::uuid"
                "   AND b.id = bill_items.bill_id"
                "   AND s.id = b.split_id"
                "   AND s.owner_id = " + owner_param + "::uuid"
                " RETURNING " + item_columns("bill_items.");

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto rows = txn.exec(sql, params);
            if (rows.empty()) {
                txn.abort();
                return json_error(404, "Item not found");
            }

            json updated = item_to_json(rows[0]);
            txn.commit();

            res.code = 200;
            res.body = updated.dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "item_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const pqxx::data_exception& e) {
            CROW_LOG_WARNING << "item_routes: numeric out of range: " << e.what();
            return json_error(400, "Amount out of range for this bill");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "item_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── DELETE /api/bills/<bid>/items/<iid> ───────────────────────────────────
    // 200 {"message":"Deleted"}. 404 if the item does not belong to this bill or
    // the bill's split is not the caller's.
    CROW_ROUTE(app, "/api/bills/<string>/items/<string>")
        .methods(crow::HTTPMethod::DELETE)
    ([&pool](const crow::request& req,
             const std::string& bill_id,
             const std::string& item_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        if (!is_uuid(bill_id) || !is_uuid(item_id)) {
            return json_error(404, "Item not found");
        }

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto rows = txn.exec(
                "DELETE FROM bill_items"
                "  USING bills b, splits s"
                " WHERE bill_items.id = $1::uuid"
                "   AND bill_items.bill_id = $2::uuid"
                "   AND b.id = bill_items.bill_id"
                "   AND s.id = b.split_id"
                "   AND s.owner_id = $3::uuid"
                " RETURNING bill_items.id",
                pqxx::params{item_id, bill_id, user->id});

            if (rows.empty()) {
                txn.abort();
                return json_error(404, "Item not found");
            }

            txn.commit();

            res.code = 200;
            res.body = R"({"message":"Deleted"})";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "item_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
