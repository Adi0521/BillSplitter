#include "routes/auth_routes.h"
#include "auth/auth.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static const std::string SESSION_COOKIE_OPTS =
    "; HttpOnly; Path=/; SameSite=Lax; Max-Age=2592000";  // 30 days

void register_auth_routes(BsApp& app, DbPool& pool) {

    // POST /api/auth/register
    // Body: { "email": "...", "password": "...", "display_name": "..." }
    CROW_ROUTE(app, "/api/auth/register").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req) {
        crow::response res;
        res.add_header("Content-Type", "application/json");
        try {
            auto body         = json::parse(req.body);
            std::string email = body.value("email", "");
            std::string pass  = body.value("password", "");
            std::string name  = body.value("display_name", "");

            if (email.empty() || pass.empty()) {
                res.code = 400;
                res.body = R"({"error":"email and password are required"})";
                return res;
            }
            if (email.find('@') == std::string::npos) {
                res.code = 400;
                res.body = R"({"error":"invalid email"})";
                return res;
            }

            auto user = auth::register_user(pool, email, pass, name);
            if (!user) {
                res.code = 409;
                res.body = R"({"error":"Email already registered"})";
                return res;
            }

            std::string token = auth::create_session(pool, user->id);
            res.code = 201;
            res.body = json({
                {"id",           user->id},
                {"email",        user->email},
                {"display_name", user->display_name}
            }).dump();
            res.add_header("Set-Cookie", "session=" + token + SESSION_COOKIE_OPTS);
        } catch (const std::invalid_argument& e) {
            res.code = 400;
            res.body = json({{"error", e.what()}}).dump();
        } catch (const std::exception& e) {
            res.code = 500;
            res.body = json({{"error", e.what()}}).dump();
        }
        return res;
    });

    // POST /api/auth/login
    // Body: { "email": "...", "password": "..." }
    CROW_ROUTE(app, "/api/auth/login").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req) {
        crow::response res;
        res.add_header("Content-Type", "application/json");
        try {
            auto body         = json::parse(req.body);
            std::string email = body.value("email", "");
            std::string pass  = body.value("password", "");

            if (email.empty() || pass.empty()) {
                res.code = 400;
                res.body = R"({"error":"email and password are required"})";
                return res;
            }

            auto user = auth::verify_credentials(pool, email, pass);
            if (!user) {
                res.code = 401;
                res.body = R"({"error":"Invalid email or password"})";
                return res;
            }

            std::string token = auth::create_session(pool, user->id);
            res.code = 200;
            res.body = json({
                {"id",           user->id},
                {"email",        user->email},
                {"display_name", user->display_name}
            }).dump();
            res.add_header("Set-Cookie", "session=" + token + SESSION_COOKIE_OPTS);
        } catch (const std::exception& e) {
            res.code = 500;
            res.body = json({{"error", e.what()}}).dump();
        }
        return res;
    });

    // POST /api/auth/logout
    CROW_ROUTE(app, "/api/auth/logout").methods(crow::HTTPMethod::POST)
    ([&pool](const crow::request& req) {
        try {
            auto token = extract_session_token(req);
            if (!token.empty()) {
                auth::delete_session(pool, token);
            }
            crow::response res(200, R"({"message":"Logged out"})");
            res.add_header("Content-Type", "application/json");
            res.add_header("Set-Cookie",
                "session=; HttpOnly; Path=/; SameSite=Lax; Max-Age=0");
            return res;
        } catch (const std::exception& e) {
            return crow::response(500,
                json({{"error", e.what()}}).dump());
        }
    });

    // GET /api/auth/me
    CROW_ROUTE(app, "/api/auth/me").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req) {
        crow::response res;
        res.add_header("Content-Type", "application/json");

        auto token = extract_session_token(req);
        auto user  = auth::get_session_user(pool, token);
        if (!user) {
            res.code = 401;
            res.body = R"({"error":"Unauthorized"})";
            return res;
        }
        res.code = 200;
        res.body = json({
            {"id",           user->id},
            {"email",        user->email},
            {"display_name", user->display_name},
            {"created_at",   user->created_at}
        }).dump();
        return res;
    });
}
