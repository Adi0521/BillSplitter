#pragma once
#include "auth/auth.h"
#include "db/db_pool.h"
#include "models/user.h"
#include <crow.h>
#include <optional>
#include <string>

// Extracts the session token from the request cookie or Authorization header.
inline std::string extract_session_token(const crow::request& req) {
    // Try Cookie: session=<token>
    auto cookie_hdr = req.get_header_value("Cookie");
    if (!cookie_hdr.empty()) {
        const std::string key = "session=";
        auto pos = cookie_hdr.find(key);
        if (pos != std::string::npos) {
            pos += key.size();
            auto end = cookie_hdr.find(';', pos);
            return cookie_hdr.substr(pos, end == std::string::npos
                                            ? std::string::npos
                                            : end - pos);
        }
    }
    // Try Authorization: Bearer <token>
    auto auth_hdr = req.get_header_value("Authorization");
    if (auth_hdr.size() > 7 && auth_hdr.substr(0, 7) == "Bearer ") {
        return auth_hdr.substr(7);
    }
    return "";
}

// Returns the authenticated user, or nullopt when the request carries no valid
// session. This is the preferred entry point: it has no output parameter and no
// side effects, so it cannot be misused.
//
//   auto user = current_user(req, pool);
//   if (!user) return unauthorized();
inline std::optional<User> current_user(const crow::request& req, DbPool& pool) {
    return auth::get_session_user(pool, extract_session_token(req));
}

// The standard 401 body. Handlers return this directly.
inline crow::response unauthorized() {
    crow::response res(401, R"({"error":"Unauthorized"})");
    res.add_header("Content-Type", "application/json");
    return res;
}

// Returns the authenticated user, or fills `res` with a 401 and returns nullopt.
//
//   auto user = require_auth(req, res, pool);
//   if (!user) return res;          // NOTE: must return `res`, not a new object
//
// This deliberately does NOT call res.end(). Doing so sets the response's
// `completed_` flag, which Crow's move-assignment carries into the connection's
// own response object; Crow then treats the request as already finished and
// skips the path that actually writes and flushes it. The client receives
// nothing and the connection is held open until it times out — so a handful of
// unauthenticated requests can occupy every worker thread. Filling in the
// response and letting Crow complete it is both correct and sufficient.
inline std::optional<User> require_auth(const crow::request& req,
                                        crow::response&     res,
                                        DbPool&             pool) {
    auto user = current_user(req, pool);
    if (!user) {
        res.code = 401;
        res.body = R"({"error":"Unauthorized"})";
        res.add_header("Content-Type", "application/json");
    }
    return user;
}
