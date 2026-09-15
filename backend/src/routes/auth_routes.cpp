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

// Cookie attributes, derived once at startup.
//
// Two deployment shapes need different answers, and getting it wrong fails
// silently — the browser simply declines to send the cookie and every request
// after login looks unauthenticated:
//
//   same-origin  (frontend proxies /api to the backend)  SameSite=Lax
//   cross-origin (frontend and API on different domains) SameSite=None; Secure
//
// SameSite=Lax is the safer default and the reason DEPLOYMENT.md recommends the
// proxy. "None" is only honoured alongside "Secure", so it is forced on.
struct CookiePolicy {
    std::string same_site = "Lax";
    bool        secure    = false;

    std::string attributes(long max_age_seconds) const {
        std::string out = "; HttpOnly; Path=/; SameSite=" + same_site;
        if (secure) out += "; Secure";
        out += "; Max-Age=" + std::to_string(max_age_seconds);
        return out;
    }
};

static CookiePolicy cookie_policy() {
    CookiePolicy p;

    if (const char* ss = std::getenv("SESSION_COOKIE_SAMESITE")) {
        const std::string v(ss);
        if (v == "None" || v == "none")      p.same_site = "None";
        else if (v == "Strict" || v == "strict") p.same_site = "Strict";
        else                                  p.same_site = "Lax";
    }

    // "auto" (the default) means: secure whenever the app is served over https.
    // That keeps http://localhost working in development without a flag, and
    // makes a real deployment secure without remembering one.
    const char* sec = std::getenv("SESSION_COOKIE_SECURE");
    const std::string mode = sec ? std::string(sec) : "auto";
    if (mode == "true" || mode == "1") {
        p.secure = true;
    } else if (mode == "false" || mode == "0") {
        p.secure = false;
    } else {
        const char* base = std::getenv("APP_BASE_URL");
        p.secure = base && std::string(base).rfind("https://", 0) == 0;
    }

    // A browser ignores SameSite=None without Secure, which would drop the
    // cookie entirely. Correcting it beats honouring an unusable combination.
    if (p.same_site == "None" && !p.secure) {
        CROW_LOG_WARNING << "auth: SESSION_COOKIE_SAMESITE=None requires Secure; "
                            "enabling Secure. Cross-site cookies need HTTPS.";
        p.secure = true;
    }
    return p;
}

static const CookiePolicy  COOKIE          = cookie_policy();
static const std::string   SESSION_COOKIE_OPTS = COOKIE.attributes(2592000);  // 30 days
static const std::string   SESSION_COOKIE_CLEAR = COOKIE.attributes(0);

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
            // Must match the attributes the cookie was set with, or the
            // browser keeps the original and logout does nothing visible.
            res.add_header("Set-Cookie", "session=" + SESSION_COOKIE_CLEAR);
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
