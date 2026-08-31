#include "routes/split_routes.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
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

// The :id path parameter is raw user input. Postgres raises an error (which
// would otherwise surface as a 500) when a non-UUID string is cast to uuid, so
// the shape is checked here and anything malformed is treated as "not found".
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
bool read_string_field(const json&        body,
                       const char*        key,
                       std::string&       out,
                       std::string&       err) {
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

bool validate_name(std::string& name, std::string& err) {
    name = trim(name);
    if (name.empty() || name.size() > 200) {
        err = "name must be 1-200 characters";
        return false;
    }
    return true;
}

bool validate_description(const std::string& description, std::string& err) {
    if (description.size() > 2000) {
        err = "description must be 2000 characters or fewer";
        return false;
    }
    return true;
}

bool validate_type(const std::string& type, std::string& err) {
    if (type != "one_time" && type != "ongoing") {
        err = "type must be one of: one_time, ongoing";
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

// ── Serialization ────────────────────────────────────────────────────────────

// Column list shared by every query that returns a Split, so the positional
// indices used by split_to_json() are identical everywhere. member_count comes
// from a correlated subquery rather than a follow-up query per split.
const char* const SPLIT_COLUMNS =
    "s.id, s.name, COALESCE(s.description, ''), s.type, s.currency, "
    "s.share_token, s.created_at, s.archived_at, "
    "(SELECT COUNT(*) FROM split_members m WHERE m.split_id = s.id)";

json split_to_json(const pqxx::row& r) {
    json j;
    j["id"]           = r[0].as<std::string>();
    j["name"]         = r[1].as<std::string>();
    j["description"]  = r[2].as<std::string>();   // COALESCE'd: "" never null
    j["type"]         = r[3].as<std::string>();
    j["currency"]     = r[4].as<std::string>();
    j["share_token"]  = r[5].as<std::string>();
    j["created_at"]   = r[6].as<std::string>();
    j["archived_at"]  = r[7].is_null() ? json(nullptr)
                                       : json(r[7].as<std::string>());
    j["member_count"] = r[8].as<long long>();
    return j;
}

json member_to_json(const pqxx::row& r) {
    json j;
    j["id"]        = r[0].as<std::string>();
    j["split_id"]  = r[1].as<std::string>();
    j["user_id"]   = r[2].is_null() ? json(nullptr) : json(r[2].as<std::string>());
    j["name"]      = r[3].as<std::string>();
    j["email"]     = r[4].is_null() ? json(nullptr) : json(r[4].as<std::string>());
    j["joined_at"] = r[5].as<std::string>();
    return j;
}

}  // namespace

void register_split_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/splits ──────────────────────────────────────────────────────
    // Owned splits, newest first. Archived splits are excluded unless
    // ?archived=true is passed.
    CROW_ROUTE(app, "/api/splits").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        try {
            const char* archived_param = req.url_params.get("archived");
            const bool include_archived =
                archived_param != nullptr &&
                (std::string(archived_param) == "true" ||
                 std::string(archived_param) == "1");

            const std::string sql =
                std::string("SELECT ") + SPLIT_COLUMNS +
                " FROM splits s WHERE s.owner_id = $1::uuid" +
                (include_archived ? "" : " AND s.archived_at IS NULL") +
                " ORDER BY s.created_at DESC";

            auto conn = pool.acquire();
            pqxx::work txn(*conn);
            auto rows = txn.exec(sql, pqxx::params{user->id});
            txn.commit();

            json out = json::array();
            for (const auto& row : rows) out.push_back(split_to_json(row));

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "split_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/splits ─────────────────────────────────────────────────────
    // Body: { name, description?, type, currency? }
    // The owner is added as the split's first member in the same transaction:
    // a split without its owner-member must never be observable.
    CROW_ROUTE(app, "/api/splits").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        try {
            auto body = json::parse(req.body);
            if (!body.is_object()) return json_error(400, "body must be a JSON object");

            std::string name, description, type, currency = "USD", err;
            if (!read_string_field(body, "name", name, err) ||
                !read_string_field(body, "description", description, err) ||
                !read_string_field(body, "type", type, err) ||
                !read_string_field(body, "currency", currency, err)) {
                return json_error(400, err);
            }
            if (currency.empty()) currency = "USD";  // explicit null / omitted

            if (!validate_name(name, err) ||
                !validate_description(description, err) ||
                !validate_type(type, err) ||
                !validate_currency(currency, err)) {
                return json_error(400, err);
            }

            // Owner's member name: display_name, else the local part of email.
            std::string member_name = trim(user->display_name);
            if (member_name.empty()) {
                const auto at = user->email.find('@');
                member_name = (at == std::string::npos) ? user->email
                                                        : user->email.substr(0, at);
            }
            if (member_name.empty()) member_name = "Owner";

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // NULLIF stores an omitted/empty description as NULL; it reads back
            // as "" through the COALESCE in SPLIT_COLUMNS.
            auto inserted = txn.exec(
                std::string("INSERT INTO splits AS s "
                            "  (owner_id, name, description, type, currency) "
                            "  VALUES ($1::uuid, $2, NULLIF($3, ''), $4, $5) "
                            "  RETURNING ") + SPLIT_COLUMNS,
                pqxx::params{user->id, name, description, type, currency});

            const std::string split_id = inserted[0][0].as<std::string>();

            txn.exec(
                "INSERT INTO split_members (split_id, user_id, name) "
                "  VALUES ($1::uuid, $2::uuid, $3)",
                pqxx::params{split_id, user->id, member_name});

            // Re-read the count now that the owner-member exists; the RETURNING
            // above ran before the member insert.
            auto counted = txn.exec(
                "SELECT COUNT(*) FROM split_members WHERE split_id = $1::uuid",
                pqxx::params{split_id});

            txn.commit();

            json out = split_to_json(inserted[0]);
            out["member_count"] = counted[0][0].as<long long>();

            res.code = 201;
            res.body = out.dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "split_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "split_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── GET /api/splits/<id> ─────────────────────────────────────────────────
    // Returns a SplitDetail: the Split plus its "members" array.
    CROW_ROUTE(app, "/api/splits/<string>").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // Ownership is scoped into the query: a split owned by someone else
            // returns no rows, hence 404 rather than 403.
            auto rows = txn.exec(
                std::string("SELECT ") + SPLIT_COLUMNS +
                " FROM splits s WHERE s.id = $1::uuid AND s.owner_id = $2::uuid",
                pqxx::params{id, user->id});

            if (rows.empty()) {
                txn.commit();
                return json_error(404, "Split not found");
            }

            auto members = txn.exec(
                "SELECT id, split_id, user_id, name, email, joined_at "
                "  FROM split_members WHERE split_id = $1::uuid "
                "  ORDER BY joined_at ASC",
                pqxx::params{id});
            txn.commit();

            json out = split_to_json(rows[0]);
            json member_array = json::array();
            for (const auto& m : members) member_array.push_back(member_to_json(m));
            out["members"] = member_array;

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "split_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── PUT /api/splits/<id> ─────────────────────────────────────────────────
    // Body: any subset of { name, description, type, currency }. Omitted fields
    // are left unchanged; an empty body is a 400.
    CROW_ROUTE(app, "/api/splits/<string>").methods(crow::HTTPMethod::PUT)
    ([&pool](const crow::request& req, const std::string& id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id)) return json_error(404, "Split not found");

        try {
            auto body = json::parse(req.body);
            if (!body.is_object()) return json_error(400, "body must be a JSON object");

            std::vector<std::string> assignments;
            pqxx::params params;
            int n = 1;
            std::string err;

            if (body.contains("name")) {
                std::string name;
                if (!read_string_field(body, "name", name, err)) return json_error(400, err);
                if (!validate_name(name, err)) return json_error(400, err);
                assignments.push_back("name = $" + std::to_string(n++));
                params.append(name);
            }
            if (body.contains("description")) {
                std::string description;
                if (!read_string_field(body, "description", description, err))
                    return json_error(400, err);
                if (!validate_description(description, err)) return json_error(400, err);
                assignments.push_back("description = NULLIF($" + std::to_string(n++) + ", '')");
                params.append(description);
            }
            if (body.contains("type")) {
                std::string type;
                if (!read_string_field(body, "type", type, err)) return json_error(400, err);
                if (!validate_type(type, err)) return json_error(400, err);
                assignments.push_back("type = $" + std::to_string(n++));
                params.append(type);
            }
            if (body.contains("currency")) {
                std::string currency;
                if (!read_string_field(body, "currency", currency, err))
                    return json_error(400, err);
                if (!validate_currency(currency, err)) return json_error(400, err);
                assignments.push_back("currency = $" + std::to_string(n++));
                params.append(currency);
            }

            if (assignments.empty()) {
                return json_error(400,
                    "No updatable fields provided; expected at least one of: "
                    "name, description, type, currency");
            }

            // Only the fields actually present are written, so omitted fields
            // keep their stored value rather than being reset to a default.
            std::string set_clause;
            for (std::size_t i = 0; i < assignments.size(); ++i) {
                if (i > 0) set_clause += ", ";
                set_clause += assignments[i];
            }

            const std::string id_param    = "$" + std::to_string(n++);
            const std::string owner_param = "$" + std::to_string(n++);
            params.append(id);
            params.append(user->id);

            const std::string sql =
                "UPDATE splits AS s SET " + set_clause +
                " WHERE s.id = " + id_param + "::uuid" +
                " AND s.owner_id = " + owner_param + "::uuid" +
                " RETURNING " + SPLIT_COLUMNS;

            auto conn = pool.acquire();
            pqxx::work txn(*conn);
            auto rows = txn.exec(sql, params);
            txn.commit();

            if (rows.empty()) return json_error(404, "Split not found");

            res.code = 200;
            res.body = split_to_json(rows[0]).dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "split_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "split_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── DELETE /api/splits/<id> ──────────────────────────────────────────────
    // Archives the split (sets archived_at); rows are never deleted. Archiving
    // an already-archived split is a no-op that still returns 200.
    CROW_ROUTE(app, "/api/splits/<string>").methods(crow::HTTPMethod::DELETE)
    ([&pool](const crow::request& req, const std::string& id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = require_auth(req, res, pool);
        if (!user) return unauthorized();

        if (!is_uuid(id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // COALESCE keeps the original archival timestamp on a repeat call.
            auto rows = txn.exec(
                "UPDATE splits SET archived_at = COALESCE(archived_at, now()) "
                "  WHERE id = $1::uuid AND owner_id = $2::uuid RETURNING id",
                pqxx::params{id, user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, "Split not found");

            res.code = 200;
            res.body = R"({"message":"Archived"})";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "split_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
