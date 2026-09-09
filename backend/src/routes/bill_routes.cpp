#include "routes/bill_routes.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

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

// Path parameters are raw user input. Postgres raises an error (which would
// otherwise surface as a 500) when a non-UUID string is cast to uuid, so the
// shape is checked here and anything malformed is treated as "not found".
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

// Reads an optional string field. Returns false (and fills `err`) if the field
// is present but is neither a string nor null. JSON null is read as "".
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
// to the NUMERIC(12,4) column, so nothing but Postgres ever rounds it.
//
// A client that sends a JSON number instead of a string has already lost the
// exact decimal to nlohmann's parse, so the value is re-serialized with dump()
// (shortest round-trip form, "3.83" for 3.83) rather than formatted through a
// printf-style conversion that would invent digits.
bool read_money_field(const json&  body,
                      const char*  key,
                      std::string& out,
                      std::string& err) {
    const auto it = body.find(key);
    if (it == body.end()) return true;
    if (it->is_null()) {
        out = "0";
        return true;
    }
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

// Accepts an unsigned decimal with at most 8 integer digits and at most 4
// decimal places, so the value always fits NUMERIC(12,4) exactly and Postgres
// is never asked to silently round a user's money.
bool validate_money(std::string& value, const char* field, std::string& err) {
    value = trim(value);
    if (value.empty()) {
        err = std::string(field) + " must not be empty";
        return false;
    }
    if (value[0] == '-') {
        err = std::string(field) + " must not be negative";
        return false;
    }

    const auto dot = value.find('.');
    const std::string int_part  = value.substr(0, dot);
    const std::string frac_part =
        (dot == std::string::npos) ? "" : value.substr(dot + 1);

    const auto digits_only = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        });
    };

    if (!digits_only(int_part) ||
        (dot != std::string::npos && !digits_only(frac_part))) {
        err = std::string(field) + " must be a decimal number, like \"12.50\"";
        return false;
    }
    if (frac_part.size() > 4) {
        err = std::string(field) + " must have at most 4 decimal places";
        return false;
    }

    std::string significant = int_part;
    significant.erase(0, std::min(significant.find_first_not_of('0'),
                                  significant.size()));
    if (significant.size() > 8) {
        err = std::string(field) + " must be at most 99999999.9999";
        return false;
    }
    return true;
}

bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// A well-formed but impossible date ("2026-02-31") would make Postgres throw on
// the ::date cast, which would surface as a 500, so the calendar is checked here.
bool validate_date(const std::string& date, std::string& err) {
    err = "date must be a calendar date in YYYY-MM-DD form";
    if (date.size() != 10) return false;
    for (std::size_t i = 0; i < date.size(); ++i) {
        const char c = date[i];
        if (i == 4 || i == 7) {
            if (c != '-') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    const int year  = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    const int day   = std::stoi(date.substr(8, 2));

    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    static const int days_in[12] = {31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
    int limit = days_in[month - 1];
    if (month == 2 && is_leap(year)) limit = 29;
    if (day > limit) return false;

    err.clear();
    return true;
}

bool validate_store_name(std::string& name, std::string& err) {
    name = trim(name);
    if (name.empty() || name.size() > 200) {
        err = "store_name must be 1-200 characters";
        return false;
    }
    return true;
}

bool validate_currency(std::string& currency, std::string& err) {
    currency = to_upper(trim(currency));
    // ISO 4217 codes are three ASCII letters. Checking only the length would
    // admit "US1" or "u$d", which then flow into FX lookups in a later phase.
    if (currency.size() != 3 ||
        !std::all_of(currency.begin(), currency.end(),
                     [](unsigned char c) { return c >= 'A' && c <= 'Z'; })) {
        err = "currency must be a 3-letter code, like USD";
        return false;
    }
    return true;
}

// subtotal and total are computed from the items on every read; a client that
// sends them is working from a stale or invented number and must be told so
// rather than have the value quietly dropped.
bool rejects_derived_fields(const json& body, std::string& err) {
    for (const char* key : {"subtotal", "total"}) {
        if (body.contains(key)) {
            err = std::string(key) +
                  " is derived from the bill's items and cannot be set";
            return true;
        }
    }
    return false;
}

// Reads an optional uuid field that may also be explicitly null.
// `present` distinguishes "omitted" from "sent as null" for PUT.
bool read_payer_field(const json&  body,
                      bool&        present,
                      std::string& out,   // "" means SQL NULL
                      std::string& err) {
    const auto it = body.find("payer_member_id");
    if (it == body.end()) return true;
    present = true;
    if (it->is_null()) {
        out.clear();
        return true;
    }
    if (!it->is_string()) {
        err = "payer_member_id must be a uuid string or null";
        return false;
    }
    out = trim(it->get<std::string>());
    if (out.empty()) return true;   // "" is treated as "no payer"
    if (!is_uuid(out)) {
        // A malformed id is bad input, not a missing resource: the split in the
        // URL was found, so this is a 400 for the same reason a member id from
        // another split is.
        err = "payer_member_id must be a uuid string or null";
        return false;
    }
    return true;
}

// ── SQL fragments ────────────────────────────────────────────────────────────

// Column list shared by every query that returns a Bill, so the positional
// indices used by bill_to_json() are identical everywhere.
//
// subtotal is SUM(price * quantity) over the bill's items and total is
// subtotal + tax + tip + fees, both computed by Postgres in NUMERIC and read
// out as text. COALESCE's fallback is written 0.0000 rather than 0 so that a
// bill with no items still renders "0.0000" and not "0".
const char* const BILL_COLUMNS =
    "b.id, b.split_id, b.store_name, to_char(b.date, 'YYYY-MM-DD'), "
    "b.currency, agg.subtotal, b.tax, b.tip, b.fees, "
    "(agg.subtotal + b.tax + b.tip + b.fees), b.payer_member_id, "
    "agg.item_count, b.created_at";

// The join to splits is what enforces ownership, and the LATERAL aggregate is
// what keeps item_count and subtotal out of an N+1 loop.
const char* const BILL_FROM =
    " FROM bills b "
    "   JOIN splits s ON s.id = b.split_id "
    "   CROSS JOIN LATERAL ("
    "       SELECT COALESCE(SUM(i.price * i.quantity), 0.0000) AS subtotal, "
    "              COUNT(i.id) AS item_count "
    "         FROM bill_items i WHERE i.bill_id = b.id) agg ";

// $1 = bill id, $2 = split id, $3 = owner id.
std::string select_one_bill_sql() {
    return std::string("SELECT ") + BILL_COLUMNS + BILL_FROM +
           " WHERE b.id = $1::uuid AND b.split_id = $2::uuid "
           "   AND s.owner_id = $3::uuid";
}

// ── Serialization ────────────────────────────────────────────────────────────

// Every NUMERIC column is read with as<std::string>() and emitted as a JSON
// string: "42.5000", never 42.5. No monetary value is ever a double here.
json bill_to_json(const pqxx::row& r) {
    json j;
    j["id"]              = r[0].as<std::string>();
    j["split_id"]        = r[1].as<std::string>();
    j["store_name"]      = r[2].as<std::string>();
    j["date"]            = r[3].as<std::string>();
    j["currency"]        = r[4].as<std::string>();
    j["subtotal"]        = r[5].as<std::string>();   // derived, read-only
    j["tax"]             = r[6].as<std::string>();
    j["tip"]             = r[7].as<std::string>();
    j["fees"]            = r[8].as<std::string>();
    j["total"]           = r[9].as<std::string>();   // derived, read-only
    j["payer_member_id"] = r[10].is_null() ? json(nullptr)
                                           : json(r[10].as<std::string>());
    j["item_count"]      = r[11].as<long long>();
    j["created_at"]      = r[12].as<std::string>();
    return j;
}

json item_to_json(const pqxx::row& r) {
    json j;
    j["id"]         = r[0].as<std::string>();
    j["bill_id"]    = r[1].as<std::string>();
    j["name"]       = r[2].as<std::string>();
    j["price"]      = r[3].as<std::string>();
    j["quantity"]   = r[4].as<int>();
    j["currency"]   = r[5].as<std::string>();
    j["line_total"] = r[6].as<std::string>();   // derived: price * quantity
    return j;
}

}  // namespace

void register_bill_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/splits/<id>/bills ───────────────────────────────────────────
    // Bills of an owned split, newest first by date then insertion order.
    CROW_ROUTE(app, "/api/splits/<string>/bills").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // Ownership is scoped into the query: another user's split yields
            // no rows, so it is indistinguishable from one that does not exist.
            auto owned = txn.exec(
                "SELECT 1 FROM splits WHERE id = $1::uuid AND owner_id = $2::uuid",
                pqxx::params{id, user->id});
            if (owned.empty()) {
                txn.commit();
                return json_error(404, "Split not found");
            }

            auto rows = txn.exec(
                std::string("SELECT ") + BILL_COLUMNS + BILL_FROM +
                " WHERE b.split_id = $1::uuid AND s.owner_id = $2::uuid "
                " ORDER BY b.date DESC, b.created_at DESC",
                pqxx::params{id, user->id});
            txn.commit();

            json out = json::array();
            for (const auto& row : rows) out.push_back(bill_to_json(row));

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "bill_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/splits/<id>/bills ──────────────────────────────────────────
    // Body: { store_name, date, currency?, tax?, tip?, fees?, payer_member_id? }
    CROW_ROUTE(app, "/api/splits/<string>/bills").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req, const std::string& id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id)) return json_error(404, "Split not found");

        try {
            auto body = json::parse(req.body);
            if (!body.is_object()) return json_error(400, "body must be a JSON object");

            std::string err;
            if (rejects_derived_fields(body, err)) return json_error(400, err);

            std::string store_name, date, currency;
            std::string tax = "0", tip = "0", fees = "0";
            std::string payer;
            bool payer_present = false;

            if (!read_string_field(body, "store_name", store_name, err) ||
                !read_string_field(body, "date", date, err) ||
                !read_string_field(body, "currency", currency, err) ||
                !read_money_field(body, "tax", tax, err) ||
                !read_money_field(body, "tip", tip, err) ||
                !read_money_field(body, "fees", fees, err) ||
                !read_payer_field(body, payer_present, payer, err)) {
                return json_error(400, err);
            }

            if (!validate_store_name(store_name, err)) return json_error(400, err);
            if (!validate_date(date, err))             return json_error(400, err);
            if (!validate_money(tax,  "tax",  err))    return json_error(400, err);
            if (!validate_money(tip,  "tip",  err))    return json_error(400, err);
            if (!validate_money(fees, "fees", err))    return json_error(400, err);

            const bool currency_given = !trim(currency).empty();
            if (currency_given && !validate_currency(currency, err)) {
                return json_error(400, err);
            }

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto split = txn.exec(
                "SELECT currency FROM splits "
                "  WHERE id = $1::uuid AND owner_id = $2::uuid",
                pqxx::params{id, user->id});
            if (split.empty()) {
                txn.commit();
                return json_error(404, "Split not found");
            }
            // Omitted currency inherits the split's currency, not a hardcoded USD.
            if (!currency_given) currency = split[0][0].as<std::string>();

            // The payer must be a member of *this* split. The split was found,
            // so a member id from elsewhere is a bad body, not a missing
            // resource: 400, not 404.
            if (payer_present && !payer.empty()) {
                auto member = txn.exec(
                    "SELECT 1 FROM split_members "
                    "  WHERE id = $1::uuid AND split_id = $2::uuid",
                    pqxx::params{payer, id});
                if (member.empty()) {
                    txn.commit();
                    return json_error(400,
                        "payer_member_id must be a member of this split");
                }
            }

            // Ownership is re-asserted in the INSERT itself rather than trusted
            // from the SELECT above. NULLIF turns an absent payer into SQL NULL.
            auto inserted = txn.exec(
                "INSERT INTO bills "
                "    (split_id, store_name, date, currency, tax, tip, fees, "
                "     payer_member_id) "
                "  SELECT s.id, $2, $3::date, $4, $5::numeric, $6::numeric, "
                "         $7::numeric, NULLIF($8, '')::uuid "
                "    FROM splits s "
                "   WHERE s.id = $1::uuid AND s.owner_id = $9::uuid "
                "  RETURNING id",
                pqxx::params{id, store_name, date, currency, tax, tip, fees,
                             payer, user->id});
            if (inserted.empty()) {
                txn.commit();
                return json_error(404, "Split not found");
            }

            auto rows = txn.exec(
                select_one_bill_sql(),
                pqxx::params{inserted[0][0].as<std::string>(), id, user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, "Bill not found");

            res.code = 201;
            res.body = bill_to_json(rows[0]).dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "bill_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "bill_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── GET /api/splits/<id>/bills/<bid> ─────────────────────────────────────
    // Returns a BillDetail: the Bill plus its "items" array.
    CROW_ROUTE(app, "/api/splits/<string>/bills/<string>")
        .methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& id,
             const std::string& bid) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id) || !is_uuid(bid)) return json_error(404, "Bill not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto rows = txn.exec(select_one_bill_sql(),
                                 pqxx::params{bid, id, user->id});
            if (rows.empty()) {
                txn.commit();
                return json_error(404, "Bill not found");
            }

            // Safe without a second ownership check: the bill above is already
            // proven to belong to the caller's split.
            auto items = txn.exec(
                "SELECT i.id, i.bill_id, i.name, i.price, i.quantity, "
                "       i.currency, (i.price * i.quantity) "
                "  FROM bill_items i WHERE i.bill_id = $1::uuid "
                " ORDER BY i.created_at, i.id",
                pqxx::params{bid});
            txn.commit();

            json out = bill_to_json(rows[0]);
            json item_array = json::array();
            for (const auto& item : items) item_array.push_back(item_to_json(item));
            out["items"] = item_array;

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "bill_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── PUT /api/splits/<id>/bills/<bid> ─────────────────────────────────────
    // Body: any subset of the POST fields. Omitted fields keep their stored
    // value; an empty body is a 400.
    CROW_ROUTE(app, "/api/splits/<string>/bills/<string>")
        .methods(crow::HTTPMethod::PUT)
    ([&pool](const crow::request& req, const std::string& id,
             const std::string& bid) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id) || !is_uuid(bid)) return json_error(404, "Bill not found");

        try {
            auto body = json::parse(req.body);
            if (!body.is_object()) return json_error(400, "body must be a JSON object");

            std::string err;
            if (rejects_derived_fields(body, err)) return json_error(400, err);

            std::vector<std::string> assignments;
            pqxx::params params;
            int n = 1;

            if (body.contains("store_name")) {
                std::string store_name;
                if (!read_string_field(body, "store_name", store_name, err))
                    return json_error(400, err);
                if (!validate_store_name(store_name, err)) return json_error(400, err);
                assignments.push_back("store_name = $" + std::to_string(n++));
                params.append(store_name);
            }
            if (body.contains("date")) {
                std::string date;
                if (!read_string_field(body, "date", date, err))
                    return json_error(400, err);
                if (!validate_date(date, err)) return json_error(400, err);
                assignments.push_back("date = $" + std::to_string(n++) + "::date");
                params.append(date);
            }
            if (body.contains("currency")) {
                std::string currency;
                if (!read_string_field(body, "currency", currency, err))
                    return json_error(400, err);
                if (trim(currency).empty()) {
                    // "" means "unset" for currency, the same as it does on
                    // create and the same as it does for payer_member_id here:
                    // fall back to the split's currency rather than rejecting.
                    // An edit form whose currency select is cleared submits ""
                    // and must not be a 400.
                    assignments.push_back(
                        "currency = (SELECT currency FROM splits WHERE id = b.split_id)");
                } else {
                    if (!validate_currency(currency, err)) return json_error(400, err);
                    assignments.push_back("currency = $" + std::to_string(n++));
                    params.append(currency);
                }
            }
            for (const char* field : {"tax", "tip", "fees"}) {
                if (!body.contains(field)) continue;
                std::string amount = "0";
                if (!read_money_field(body, field, amount, err))
                    return json_error(400, err);
                if (!validate_money(amount, field, err)) return json_error(400, err);
                assignments.push_back(std::string(field) + " = $" +
                                      std::to_string(n++) + "::numeric");
                params.append(amount);
            }

            std::string payer;
            bool payer_present = false;
            if (!read_payer_field(body, payer_present, payer, err))
                return json_error(400, err);
            if (payer_present) {
                assignments.push_back("payer_member_id = NULLIF($" +
                                      std::to_string(n++) + ", '')::uuid");
                params.append(payer);
            }

            if (assignments.empty()) {
                return json_error(400,
                    "No updatable fields provided; expected at least one of: "
                    "store_name, date, currency, tax, tip, fees, payer_member_id");
            }

            // Only the fields actually present are written, so omitted fields
            // keep their stored value rather than being reset to a default.
            // Placeholder *names* are concatenated; values only ever travel as
            // bound parameters.
            std::string set_clause;
            for (std::size_t i = 0; i < assignments.size(); ++i) {
                if (i > 0) set_clause += ", ";
                set_clause += assignments[i];
            }

            const std::string bid_param   = "$" + std::to_string(n++);
            const std::string id_param    = "$" + std::to_string(n++);
            const std::string owner_param = "$" + std::to_string(n++);
            params.append(bid);
            params.append(id);
            params.append(user->id);

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // The bill is resolved first so that "bill not found" (404) is
            // decided before "payer is not a member of this split" (400).
            auto existing = txn.exec(
                "SELECT b.id FROM bills b JOIN splits s ON s.id = b.split_id "
                " WHERE b.id = $1::uuid AND b.split_id = $2::uuid "
                "   AND s.owner_id = $3::uuid",
                pqxx::params{bid, id, user->id});
            if (existing.empty()) {
                txn.commit();
                return json_error(404, "Bill not found");
            }

            if (payer_present && !payer.empty()) {
                auto member = txn.exec(
                    "SELECT 1 FROM split_members "
                    "  WHERE id = $1::uuid AND split_id = $2::uuid",
                    pqxx::params{payer, id});
                if (member.empty()) {
                    txn.commit();
                    return json_error(400,
                        "payer_member_id must be a member of this split");
                }
            }

            const std::string sql =
                "UPDATE bills AS b SET " + set_clause +
                "  FROM splits s "
                " WHERE s.id = b.split_id "
                "   AND b.id = " + bid_param + "::uuid" +
                "   AND b.split_id = " + id_param + "::uuid" +
                "   AND s.owner_id = " + owner_param + "::uuid" +
                " RETURNING b.id";

            auto updated = txn.exec(sql, params);
            if (updated.empty()) {
                txn.commit();
                return json_error(404, "Bill not found");
            }

            // Re-read through the shared projection so the response carries the
            // recomputed subtotal, total and item_count.
            auto rows = txn.exec(select_one_bill_sql(),
                                 pqxx::params{bid, id, user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, "Bill not found");

            res.code = 200;
            res.body = bill_to_json(rows[0]).dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "bill_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "bill_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── DELETE /api/splits/<id>/bills/<bid> ──────────────────────────────────
    // A real delete, unlike splits which archive. Items cascade via the
    // bill_items.bill_id foreign key.
    CROW_ROUTE(app, "/api/splits/<string>/bills/<string>")
        .methods(crow::HTTPMethod::DELETE)
    ([&pool](const crow::request& req, const std::string& id,
             const std::string& bid) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id) || !is_uuid(bid)) return json_error(404, "Bill not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            auto rows = txn.exec(
                "DELETE FROM bills b USING splits s "
                " WHERE s.id = b.split_id AND b.id = $1::uuid "
                "   AND b.split_id = $2::uuid AND s.owner_id = $3::uuid "
                " RETURNING b.id",
                pqxx::params{bid, id, user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, "Bill not found");

            res.code = 200;
            res.body = R"({"message":"Deleted"})";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "bill_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
