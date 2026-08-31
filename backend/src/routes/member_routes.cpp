#include "routes/member_routes.h"
#include "auth/auth.h"
#include "auth/middleware.h"
#include "models/user.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <cctype>
#include <optional>
#include <string>

using json = nlohmann::json;

namespace {

constexpr size_t MAX_NAME_LEN  = 100;
constexpr size_t MAX_EMAIL_LEN = 320;

// ── Helpers ───────────────────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    const char* ws = " \t\n\r\f\v";
    auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    auto end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// Path parameters are user input and are interpolated into `$n::uuid` casts.
// Postgres raises invalid_text_representation (a 500 if it escaped) for a
// malformed uuid, so ids are validated in C++ first and a bad one is simply
// "not found" — the same answer a well-formed but unknown id gets, which also
// avoids leaking whether an id was ever plausible.
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

// user_id and email are nullable columns and must serialize as JSON null,
// not as "". name is NOT NULL and is always a string.
json member_to_json(const pqxx::row& r) {
    json j;
    j["id"]        = r["id"].as<std::string>();
    j["split_id"]  = r["split_id"].as<std::string>();
    j["user_id"]   = r["user_id"].is_null() ? json(nullptr)
                                            : json(r["user_id"].as<std::string>());
    j["name"]      = r["name"].as<std::string>();
    j["email"]     = r["email"].is_null() ? json(nullptr)
                                          : json(r["email"].as<std::string>());
    j["joined_at"] = r["joined_at"].as<std::string>();
    return j;
}

}  // namespace

void register_member_routes(BsApp& app, DbPool& pool) {

    // ── GET /api/splits/<id>/members ──────────────────────────────────────────
    // 200 [Member] ordered by joined_at ASC. 404 if the split does not exist or
    // is not owned by the caller.
    CROW_ROUTE(app, "/api/splits/<string>/members").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req, const std::string& split_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        if (!is_uuid(split_id)) return json_error(404, "Split not found");

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // LEFT JOIN so the ownership check and the member list are one
            // round trip: zero rows means "no such split for this user" (404),
            // while a single row with a NULL member id means "owned, but the
            // split has no members yet" (200 []).
            auto rows = txn.exec(
                "SELECT m.id, m.split_id, m.user_id, m.name, m.email, m.joined_at"
                "  FROM splits s"
                "  LEFT JOIN split_members m ON m.split_id = s.id"
                " WHERE s.id = $1::uuid AND s.owner_id = $2::uuid"
                " ORDER BY m.joined_at ASC",
                pqxx::params{split_id, user->id});
            txn.commit();

            if (rows.empty()) return json_error(404, "Split not found");

            json out = json::array();
            for (const auto& r : rows) {
                if (r["id"].is_null()) continue;  // owned split, no members
                out.push_back(member_to_json(r));
            }

            res.code = 200;
            res.body = out.dump();
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "member_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── POST /api/splits/<id>/members ─────────────────────────────────────────
    // Body: { "name": "...", "email": "..." }  → 201 Member
    CROW_ROUTE(app, "/api/splits/<string>/members").methods(crow::HTTPMethod::POST)
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

            // Validate before touching the database.
            if (!body.contains("name") || !body["name"].is_string()) {
                return json_error(400, "name is required");
            }
            std::string name = trim(body["name"].get<std::string>());
            if (name.empty()) {
                return json_error(400, "name is required");
            }
            if (name.size() > MAX_NAME_LEN) {
                return json_error(400, "name must be 1-100 characters");
            }

            // email is optional. Absent, JSON null, or blank all store NULL;
            // anything else must look like an address. Duplicate names within a
            // split are explicitly allowed, so there is no uniqueness check.
            std::optional<std::string> email;
            if (body.contains("email") && !body["email"].is_null()) {
                if (!body["email"].is_string()) {
                    return json_error(400, "email must be a string");
                }
                std::string e = trim(body["email"].get<std::string>());
                if (!e.empty()) {
                    if (e.size() > MAX_EMAIL_LEN) {
                        return json_error(400, "email must be at most 320 characters");
                    }
                    if (e.find('@') == std::string::npos) {
                        return json_error(400, "invalid email");
                    }
                    email = e;
                }
            }

            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // INSERT ... SELECT: the row only comes into existence if the
            // SELECT finds a split with this id owned by this user, so
            // ownership is enforced by the statement itself. No rows inserted
            // means the split is not the caller's — 404.
            auto rows = txn.exec(
                "INSERT INTO split_members (split_id, name, email)"
                " SELECT s.id, $3::text, $4::text"
                "   FROM splits s"
                "  WHERE s.id = $1::uuid AND s.owner_id = $2::uuid"
                " RETURNING id, split_id, user_id, name, email, joined_at",
                pqxx::params{split_id, user->id, name, email});

            if (rows.empty()) {
                txn.abort();
                return json_error(404, "Split not found");
            }
            txn.commit();

            res.code = 201;
            res.body = member_to_json(rows[0]).dump();
        } catch (const json::exception& e) {
            CROW_LOG_WARNING << "member_routes: bad JSON body: " << e.what();
            return json_error(400, "Invalid JSON body");
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "member_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });

    // ── DELETE /api/splits/<id>/members/<mid> ─────────────────────────────────
    // 200 {"message":"Removed"}. 404 if the split is not the caller's OR the
    // member does not belong to that split.
    CROW_ROUTE(app, "/api/splits/<string>/members/<string>")
        .methods(crow::HTTPMethod::DELETE)
    ([&pool](const crow::request& req,
             const std::string& split_id,
             const std::string& member_id) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto user = current_user(req, pool);
        if (!user) return json_error(401, "Unauthorized");

        // Either id may be arbitrary user input; a malformed one is a 404
        // rather than a Postgres cast error surfacing as a 500.
        if (!is_uuid(split_id) || !is_uuid(member_id)) {
            return json_error(404, "Member not found");
        }

        try {
            auto conn = pool.acquire();
            pqxx::work txn(*conn);

            // Three conditions, all in the statement: the member is this id,
            // the member's split is this split, and that split is the caller's.
            // A member id from someone else's split therefore matches nothing.
            auto rows = txn.exec(
                "DELETE FROM split_members m"
                "  USING splits s"
                " WHERE m.id = $1::uuid"
                "   AND m.split_id = s.id"
                "   AND s.id = $2::uuid"
                "   AND s.owner_id = $3::uuid"
                " RETURNING m.id",
                pqxx::params{member_id, split_id, user->id});

            if (rows.empty()) {
                txn.abort();
                return json_error(404, "Member not found");
            }
            txn.commit();

            res.code = 200;
            res.body = R"({"message":"Removed"})";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "member_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
        return res;
    });
}
