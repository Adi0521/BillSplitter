#include "routes/auth_routes.h"
#include "auth/auth.h"
#include "auth/login_throttle.h"
#include "auth/middleware.h"
#include <crow.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Tunable so tests can exercise the limit without a hundred requests, and so a
// deployment can loosen it without a rebuild.
static std::size_t env_size(const char* name, std::size_t fallback) {
    if (const char* v = std::getenv(name)) {
        try {
            auto n = std::stoul(v);
            if (n > 0) return static_cast<std::size_t>(n);
        } catch (const std::exception&) { /* fall through to the default */ }
    }
    return fallback;
}

static const std::string SESSION_COOKIE_OPTS =
    "; HttpOnly; Path=/; SameSite=Lax; Max-Age=2592000";  // 30 days

void register_auth_routes(BsApp& app, DbPool& pool) {

    // Function-local static: one throttle shared by every request handled by
    // this app, constructed on first use.
    static LoginThrottle throttle(
        env_size("LOGIN_MAX_FAILURES", 10),
        std::chrono::seconds(env_size("LOGIN_THROTTLE_WINDOW_SECONDS", 900)));

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
            // created_at is part of the documented User shape. Omitting it here
            // while GET /api/auth/me includes it gave the same resource two
            // shapes depending on which endpoint produced it.
            res.body = json({
                {"id",           user->id},
                {"email",        user->email},
                {"display_name", user->display_name},
                {"created_at",   user->created_at}
            }).dump();
            res.add_header("Set-Cookie", "session=" + token + SESSION_COOKIE_OPTS);
        } catch (const std::invalid_argument& e) {
            res.code = 400;
            res.body = json({{"error", e.what()}}).dump();
        } catch (const std::exception& e) {
            // Internal exception text can carry SQL and schema details, so it is
            // logged server-side and never returned to the client.
            CROW_LOG_ERROR << "auth_routes: " << e.what();
            res.code = 500;
            res.body = R"({"error":"Internal server error"})";
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

            // Keyed on email+IP so one person mistyping their own password
            // cannot lock out others behind the same address.
            const std::string throttle_key = email + "|" + req.remote_ip_address;
            if (throttle.is_blocked(throttle_key)) {
                res.code = 429;
                res.body = R"({"error":"Too many failed sign-in attempts. Try again later."})";
                return res;
            }

            auto user = auth::verify_credentials(pool, email, pass);
            if (!user) {
                throttle.record_failure(throttle_key);
                res.code = 401;
                res.body = R"({"error":"Invalid email or password"})";
                return res;
            }
            // Only failures accumulate; a success clears the history so normal
            // use is never throttled.
            throttle.record_success(throttle_key);

            std::string token = auth::create_session(pool, user->id);
            res.code = 200;
            // created_at is part of the documented User shape. Omitting it here
            // while GET /api/auth/me includes it gave the same resource two
            // shapes depending on which endpoint produced it.
            res.body = json({
                {"id",           user->id},
                {"email",        user->email},
                {"display_name", user->display_name},
                {"created_at",   user->created_at}
            }).dump();
            res.add_header("Set-Cookie", "session=" + token + SESSION_COOKIE_OPTS);
        } catch (const std::exception& e) {
            // Internal exception text can carry SQL and schema details, so it is
            // logged server-side and never returned to the client.
            CROW_LOG_ERROR << "auth_routes: " << e.what();
            res.code = 500;
            res.body = R"({"error":"Internal server error"})";
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
            CROW_LOG_ERROR << "auth_routes: " << e.what();
            crow::response res(500, R"({"error":"Internal server error"})");
            res.add_header("Content-Type", "application/json");
            return res;
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
