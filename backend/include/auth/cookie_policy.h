#pragma once
#include <crow/logging.h>
#include <cstdlib>
#include <string>

// Cookie attributes, derived from the environment.
//
// This lived in auth_routes.cpp until Google sign-in needed it too: the
// callback sets the same session cookie as a password login, and the `state`
// cookie has to be Secure under exactly the same rule. Two copies of this rule
// would eventually disagree, and the failure mode is silent — the browser
// simply declines to send the cookie and every request after sign-in looks
// unauthenticated.
//
// Two deployment shapes need different answers:
//
//   same-origin  (frontend proxies /api to the backend)  SameSite=Lax
//   cross-origin (frontend and API on different domains) SameSite=None; Secure
//
// SameSite=Lax is the safer default and the reason DEPLOYMENT.md recommends the
// proxy. "None" is only honoured alongside "Secure", so it is forced on.
struct CookiePolicy {
    std::string same_site = "Lax";
    bool        secure    = false;

    // The attribute tail for a Set-Cookie value, e.g.
    //   "; HttpOnly; Path=/; SameSite=Lax; Max-Age=2592000"
    std::string attributes(long max_age_seconds, const std::string& path = "/") const {
        std::string out = "; HttpOnly; Path=" + path + "; SameSite=" + same_site;
        if (secure) out += "; Secure";
        out += "; Max-Age=" + std::to_string(max_age_seconds);
        return out;
    }
};

inline CookiePolicy cookie_policy() {
    CookiePolicy p;

    if (const char* ss = std::getenv("SESSION_COOKIE_SAMESITE")) {
        const std::string v(ss);
        if (v == "None" || v == "none")           p.same_site = "None";
        else if (v == "Strict" || v == "strict")   p.same_site = "Strict";
        else                                      p.same_site = "Lax";
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

// Session cookie lifetime, matching the `sessions.expires_at` the database sets.
inline constexpr long SESSION_COOKIE_MAX_AGE = 2592000;  // 30 days
