#include "routes/google_routes.h"
#include "auth/auth.h"
#include "auth/cookie_policy.h"
#include "auth/identity_provider.h"
#include <crow.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <cstdlib>
#include <memory>
#include <string>

using json = nlohmann::json;

namespace {

// ── Response helpers ─────────────────────────────────────────────────────────

crow::response json_error(int code, const std::string& message) {
    crow::response res(code, json({{"error", message}}).dump());
    res.add_header("Content-Type", "application/json");
    return res;
}

// One message for every unusable `state`: missing, malformed, mismatched, or
// already spent. A caller has no legitimate reason to tell them apart, and
// distinguishing them would describe the CSRF check to whoever is probing it.
const char* const BAD_STATE =
    "This sign-in link has expired or is invalid. Please try signing in again.";

// ── Configuration ────────────────────────────────────────────────────────────

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

std::string strip_trailing_slashes(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// The `state` cookie is scoped to /api/auth/google, so it is not attached to
// any other request in the app, and expires in ten minutes — long enough for a
// consent screen, short enough that an abandoned attempt leaves nothing behind.
const char* const STATE_COOKIE   = "g_state";
constexpr long    STATE_MAX_AGE  = 600;
const char* const STATE_PATH     = "/api/auth/google";

// Secure/SameSite come from the one policy the session cookie uses; see
// auth/cookie_policy.h for why that rule is shared rather than restated.
const CookiePolicy COOKIE            = cookie_policy();
const std::string  STATE_COOKIE_OPTS = COOKIE.attributes(STATE_MAX_AGE, STATE_PATH);
const std::string  STATE_COOKIE_CLEAR = COOKIE.attributes(0, STATE_PATH);
const std::string  SESSION_COOKIE_OPTS =
    COOKIE.attributes(SESSION_COOKIE_MAX_AGE);

// ── Cookies ──────────────────────────────────────────────────────────────────

std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// Reads one named cookie. Parsed pair by pair rather than with a substring
// search, so a cookie whose name merely *ends* with the one we want (or a value
// that happens to contain "g_state=") cannot be mistaken for it.
std::string cookie_value(const crow::request& req, const std::string& name) {
    const std::string hdr = req.get_header_value("Cookie");
    std::size_t i = 0;
    while (i <= hdr.size()) {
        const std::size_t end = hdr.find(';', i);
        const std::string pair =
            trim(hdr.substr(i, end == std::string::npos ? std::string::npos : end - i));
        const std::size_t eq = pair.find('=');
        if (eq != std::string::npos && trim(pair.substr(0, eq)) == name) {
            return trim(pair.substr(eq + 1));
        }
        if (end == std::string::npos) break;
        i = end + 1;
    }
    return "";
}

// ── state ────────────────────────────────────────────────────────────────────

// `state` carries two jobs: it proves the callback belongs to a sign-in this
// browser started (CSRF), and it remembers where the user was going.
//
// Only the NONCE travels to Google and back. The redirect path stays in the
// HttpOnly cookie, so the value that ends up in a Location: header was never
// outside this server's control — a `state` parameter round-tripping through an
// external service is attacker-reachable input, and an open redirect is exactly
// what you get by trusting one.
struct SignInState {
    std::string nonce;
    std::string redirect_path = "/";
};

std::string encode_state_cookie(const SignInState& st) {
    return auth::base64url_encode(
        json({{"n", st.nonce}, {"r", st.redirect_path}}).dump());
}

bool decode_state_cookie(const std::string& raw, SignInState& out) {
    std::string decoded;
    if (raw.empty() || !auth::base64url_decode(raw, decoded)) return false;
    json j;
    try {
        j = json::parse(decoded);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object()) return false;
    out.nonce         = j.value("n", std::string());
    out.redirect_path = j.value("r", std::string("/"));
    // 32 hex characters, as minted by generate_token(16). A cookie of any other
    // shape was not produced by /start.
    return out.nonce.size() == 32;
}

// Length-checked and content-constant: a short-circuiting == would leak how
// much of the nonce an attacker had guessed.
bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.empty() || a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// ── Redirect targets ─────────────────────────────────────────────────────────

// Re-validated here even though this server generated it and kept it in an
// HttpOnly cookie. It ends up in a Location: header, which is the precise shape
// of an open redirect, and the cost of checking it again is nothing.
//
// Accepted: a path beginning with exactly one '/'. Refused:
//   "//evil.com"        protocol-relative — a different origin entirely
//   "/\evil.com"        browsers normalise '\' to '/', so this is the same trick
//   "https://evil.com"  absolute
//   "javascript:..."    not a path at all
//   anything with a control character, which is header injection
bool is_safe_redirect_path(const std::string& p) {
    if (p.empty() || p.size() > 512) return false;
    if (p[0] != '/') return false;
    if (p.size() > 1 && (p[1] == '/' || p[1] == '\\')) return false;
    for (unsigned char c : p) {
        if (c < 0x21 || c > 0x7E) return false;  // controls, space, non-ASCII
        if (c == '\\' || c == '"' || c == '<' || c == '>' || c == '^' || c == '`') {
            return false;
        }
    }
    return true;
}

std::string app_base_url() {
    return strip_trailing_slashes(env_or("APP_BASE_URL", "http://localhost:5173"));
}

// The final target: the app, the path the user was heading for, and the flags
// the UI acts on. Never the code, the token, or the email — a URL lands in
// browser history, in the Referer of the next request, and in any proxy log
// between here and there.
std::string success_target(const std::string& redirect_path,
                           const auth::GoogleSignInResult& result) {
    const std::string path =
        is_safe_redirect_path(redirect_path) ? redirect_path : "/";
    std::string target = app_base_url() + path;
    target += (target.find('?') == std::string::npos) ? '?' : '&';
    target += "signed_in=google";
    target += "&seats=" + std::to_string(result.seats_linked);
    if (result.password_cleared) target += "&password_cleared=1";
    return target;
}

// ── Query parameters ─────────────────────────────────────────────────────────

std::string query_param(const crow::request& req, const char* name) {
    const char* v = req.url_params.get(name);
    return v ? std::string(v) : std::string();
}

}  // namespace

void register_google_routes(BsApp& app, DbPool& pool) {

    // Built once, at registration, so the fake provider's warning appears in the
    // startup log rather than on the first sign-in attempt. Null when Google is
    // not configured and the fake is off — see make_identity_provider().
    static const std::unique_ptr<auth::IdentityProvider> provider =
        auth::make_identity_provider();

    // ── GET /api/auth/google/start ───────────────────────────────────────────
    //
    // Optional ?redirect=<path> — where to land after signing in, so a user who
    // followed an invite link reaches the invite rather than the app root.
    //
    // Safe to call twice, and the frontend does: it preflights with
    // redirect:'manual' to detect the 503 below, then navigates for real. No
    // state is recorded server-side, so the second call simply overwrites the
    // first call's cookie and the last one issued is the one in play.
    CROW_ROUTE(app, "/api/auth/google/start").methods(crow::HTTPMethod::GET)
    ([](const crow::request& req) {
        try {
            if (!provider) {
                // A redirect to a consent screen that cannot work is worse than
                // an honest failure: the user would bounce through Google and
                // come back to an error with no idea what went wrong.
                return json_error(503,
                    "Google sign-in is not configured on this server.");
            }

            const std::string requested = query_param(req, "redirect");
            SignInState st;
            st.nonce = auth::generate_token(16);  // 32 hex characters
            if (is_safe_redirect_path(requested)) {
                st.redirect_path = requested;
            } else if (!requested.empty()) {
                CROW_LOG_WARNING << "google_routes: ignoring unsafe ?redirect="
                                 << requested.substr(0, 64);
            }

            crow::response res(302);
            res.add_header("Content-Type", "application/json");
            res.add_header("Set-Cookie", std::string(STATE_COOKIE) + "=" +
                                             encode_state_cookie(st) +
                                             STATE_COOKIE_OPTS);
            res.add_header("Location", provider->authorize_url(st.nonce));
            res.body = "{}";
            return res;
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "google_routes: " << e.what();
            return json_error(500, "Internal server error");
        }
    });

    // ── GET /api/auth/google/callback?code=&state= ───────────────────────────
    CROW_ROUTE(app, "/api/auth/google/callback").methods(crow::HTTPMethod::GET)
    ([&pool](const crow::request& req) {
        // Spent either way: a rejected callback must not leave a state cookie
        // behind for a second attempt to replay, and a successful one must not
        // either.
        const std::string clear_state =
            std::string(STATE_COOKIE) + "=" + STATE_COOKIE_CLEAR;

        try {
            const std::string state = query_param(req, "state");
            const std::string code  = query_param(req, "code");

            SignInState st;
            const bool have_cookie =
                decode_state_cookie(cookie_value(req, STATE_COOKIE), st);

            // Checked FIRST — before the database, before Google, before
            // anything is created. An unsolicited callback is a CSRF attempt,
            // and the cheapest correct response to one is to do no work.
            if (!have_cookie || !constant_time_equal(state, st.nonce)) {
                auto res = json_error(400, BAD_STATE);
                res.add_header("Set-Cookie", clear_state);
                return res;
            }

            // Google's own refusal (the user pressed Cancel, or consent was
            // revoked) arrives here as ?error= with no code.
            const std::string google_error = query_param(req, "error");
            if (!google_error.empty()) {
                CROW_LOG_INFO << "google_routes: authorization declined ("
                              << google_error.substr(0, 64) << ")";
                auto res = json_error(401, "Google sign-in was not completed.");
                res.add_header("Set-Cookie", clear_state);
                return res;
            }
            if (code.empty()) {
                auto res = json_error(400, "Missing authorization code.");
                res.add_header("Set-Cookie", clear_state);
                return res;
            }
            if (!provider) {
                auto res = json_error(503,
                    "Google sign-in is not configured on this server.");
                res.add_header("Set-Cookie", clear_state);
                return res;
            }

            auth::GoogleIdentity identity;
            try {
                identity = provider->exchange_code(code);
            } catch (const auth::IdentityError& e) {
                // The exchange failing is not the user's mistake and not this
                // server's bug: it is a bad or replayed code, or Google being
                // unreachable. 502, with the detail in the log only.
                // WARNING, not ERROR: a rejected or replayed code is normal
                // traffic, and the suite deliberately sends malformed ones. An
                // ERROR here is reserved for this server being broken.
                CROW_LOG_WARNING << "google_routes: code exchange failed: " << e.what();
                auto res = json_error(502,
                    "Could not complete sign-in with Google. Please try again.");
                res.add_header("Set-Cookie", clear_state);
                return res;
            }

            auth::GoogleSignInResult result;
            try {
                result = auth::sign_in_with_google(pool, identity);
            } catch (const auth::AccountLinkConflict& e) {
                auto res = json_error(409, e.what());
                res.add_header("Set-Cookie", clear_state);
                return res;
            }

            const std::string token = auth::create_session(pool, result.user.id);

            crow::response res(302);
            res.add_header("Content-Type", "application/json");
            res.add_header("Set-Cookie", clear_state);
            res.add_header("Set-Cookie", "session=" + token + SESSION_COOKIE_OPTS);
            res.add_header("Location", success_target(st.redirect_path, result));
            res.body = "{}";
            return res;
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "google_routes: " << e.what();
            auto res = json_error(500, "Internal server error");
            res.add_header("Set-Cookie", clear_state);
            return res;
        }
    });
}
