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

// Returns the authenticated user or responds 401 and returns nullopt.
// Usage:
//   auto user_opt = require_auth(req, res, pool);
//   if (!user_opt) return;
inline std::optional<User> require_auth(const crow::request& req,
                                        crow::response&     res,
                                        DbPool&             pool) {
    auto token = extract_session_token(req);
    auto user  = auth::get_session_user(pool, token);
    if (!user) {
        res.code = 401;
        res.body = R"({"error":"Unauthorized"})";
        res.add_header("Content-Type", "application/json");
        res.end();
    }
    return user;
}
