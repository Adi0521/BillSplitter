#include "auth/identity_provider.h"

#include <crow/logging.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace auth {
namespace {

// ── Small helpers ────────────────────────────────────────────────────────────

std::string env_or(const char* name, const std::string& fallback = "") {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// Percent-encoding for query parameters. Hand-rolled rather than via
// curl_easy_escape so that authorize_url() needs no curl handle and stays
// usable (and testable) without touching the network stack.
std::string url_encode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Trailing slashes in APP_BASE_URL would produce "http://host//api/...", which
// is a different path to some proxies and a different redirect_uri to Google
// than the one registered with it.
std::string strip_trailing_slashes(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// JWTs and OAuth state both use base64url, which nlohmann does not provide.
const char* const B64URL =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Google may send `email_verified` as a JSON boolean or as the string "true";
// both appear in the wild, and reading only one of them would silently treat a
// verified address as unverified (or, far worse, the reverse).
bool read_email_verified(const json& claims) {
    auto it = claims.find("email_verified");
    if (it == claims.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        const std::string v = it->get<std::string>();
        return v == "true" || v == "1";
    }
    return false;
}

// Turns a Google-ish claim set into a GoogleIdentity, or throws. Shared by the
// real and fake providers so both enforce the same minimum: an identity with
// no `sub` is not an identity, and one with no address cannot be linked to
// anything.
GoogleIdentity identity_from_claims(const json& claims) {
    GoogleIdentity id;
    id.sub   = claims.value("sub", std::string());
    id.email = claims.value("email", std::string());
    // "name" is Google's field; "display_name" is ours, accepted so a fake
    // identity can be written in either vocabulary.
    id.display_name = claims.value("name", claims.value("display_name", std::string()));
    id.email_verified = read_email_verified(claims);

    if (id.sub.empty())   throw IdentityError("identity has no subject (sub)");
    if (id.email.empty()) throw IdentityError("identity has no email address");
    if (id.sub.size() > 255 || id.email.size() > 320) {
        throw IdentityError("identity fields are implausibly long");
    }
    if (id.display_name.size() > 200) id.display_name.resize(200);
    return id;
}

// ── The real provider ────────────────────────────────────────────────────────

std::size_t curl_write(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const std::size_t n = size * nmemb;
    // A hostile or broken endpoint must not be able to grow this without
    // bound; a token response is a couple of kilobytes.
    if (out->size() + n > 256 * 1024) return 0;  // 0 aborts the transfer
    out->append(ptr, n);
    return n;
}

// curl_global_init is not thread-safe and must happen once before any easy
// handle is created. Crow serves on 8 worker threads, so "the first request
// happens to do it" is not good enough.
void curl_init_once() {
    static std::once_flag flag;
    std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct CurlHandle {
    CURL* h = nullptr;
    CurlHandle() : h(curl_easy_init()) {}
    ~CurlHandle() { if (h) curl_easy_cleanup(h); }
    CurlHandle(const CurlHandle&)            = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

class GoogleIdentityProvider : public IdentityProvider {
public:
    GoogleIdentityProvider(std::string client_id,
                           std::string client_secret,
                           std::string app_base_url)
        : client_id_(std::move(client_id)),
          client_secret_(std::move(client_secret)),
          redirect_uri_(strip_trailing_slashes(std::move(app_base_url)) +
                        "/api/auth/google/callback") {
        curl_init_once();
    }

    std::string authorize_url(const std::string& state) const override {
        return "https://accounts.google.com/o/oauth2/v2/auth"
               "?client_id="     + url_encode(client_id_) +
               "&redirect_uri="  + url_encode(redirect_uri_) +
               "&response_type=code"
               "&scope="         + url_encode("openid email profile") +
               "&state="         + url_encode(state);
    }

    GoogleIdentity exchange_code(const std::string& code) const override {
        const std::string body =
            "code="           + url_encode(code) +
            "&client_id="     + url_encode(client_id_) +
            "&client_secret=" + url_encode(client_secret_) +
            "&redirect_uri="  + url_encode(redirect_uri_) +
            "&grant_type=authorization_code";

        CurlHandle curl;
        if (!curl.h) throw IdentityError("curl_easy_init failed");

        std::string response;
        curl_easy_setopt(curl.h, CURLOPT_URL, "https://oauth2.googleapis.com/token");
        curl_easy_setopt(curl.h, CURLOPT_POST, 1L);
        curl_easy_setopt(curl.h, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl.h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl.h, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(curl.h, CURLOPT_WRITEDATA, &response);
        // Timeouts are not optional here. This call happens on a Crow worker
        // thread, and a hung TCP connection with no timeout occupies that
        // thread forever — eight of them and the server stops answering
        // anything at all.
        curl_easy_setopt(curl.h, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl.h, CURLOPT_TIMEOUT, 10L);
        // libcurl's DNS timeout is implemented with SIGALRM unless told
        // otherwise, which is unsafe off the main thread.
        curl_easy_setopt(curl.h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl.h, CURLOPT_FOLLOWLOCATION, 0L);
        // TLS verification left at its (on) default deliberately: the entire
        // reason the ID token needs no local signature check is that it came
        // from Google over a verified TLS connection.
        curl_easy_setopt(curl.h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.h, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.h, CURLOPT_USERAGENT, "billsplitter/1.0");

        const CURLcode rc = curl_easy_perform(curl.h);
        if (rc != CURLE_OK) {
            throw IdentityError(std::string("token endpoint unreachable: ") +
                                curl_easy_strerror(rc));
        }
        long status = 0;
        curl_easy_getinfo(curl.h, CURLINFO_RESPONSE_CODE, &status);

        json token;
        try {
            token = json::parse(response);
        } catch (const std::exception&) {
            throw IdentityError("token endpoint returned a non-JSON body (HTTP " +
                                std::to_string(status) + ")");
        }
        if (status < 200 || status >= 300) {
            // Google's own error code ("invalid_grant" for a replayed or
            // expired code) is the single most useful thing in the log here.
            throw IdentityError("token endpoint rejected the code (HTTP " +
                                std::to_string(status) + "): " +
                                token.value("error", std::string("unknown")));
        }

        const std::string id_token = token.value("id_token", std::string());
        if (id_token.empty()) throw IdentityError("token response carried no id_token");

        return identity_from_id_token(id_token);
    }

private:
    // Decodes the ID token's payload segment and reads the claims out of it.
    //
    // The signature is deliberately NOT verified, and that is safe only on
    // this exact path: this token was fetched by us, over TLS with certificate
    // verification on, directly from https://oauth2.googleapis.com/token, in
    // response to a code we exchanged with our own client secret. There is no
    // untrusted party between Google and this function, so the signature would
    // only be re-proving what TLS already proved.
    //
    // An ID token arriving by ANY other route — posted by a browser, forwarded
    // by a client, read from a header — has no such guarantee and MUST have
    // its signature checked against Google's JWKS before a single claim is
    // trusted. If this function ever grows a second caller, that is the moment
    // to add it.
    static GoogleIdentity identity_from_id_token(const std::string& id_token) {
        const auto first = id_token.find('.');
        if (first == std::string::npos) throw IdentityError("id_token is not a JWT");
        const auto second = id_token.find('.', first + 1);
        if (second == std::string::npos) throw IdentityError("id_token is not a JWT");

        const std::string payload_b64 =
            id_token.substr(first + 1, second - first - 1);
        std::string payload;
        if (!base64url_decode(payload_b64, payload)) {
            throw IdentityError("id_token payload is not valid base64url");
        }

        json claims;
        try {
            claims = json::parse(payload);
        } catch (const std::exception&) {
            throw IdentityError("id_token payload is not JSON");
        }
        if (!claims.is_object()) throw IdentityError("id_token payload is not an object");
        return identity_from_claims(claims);
    }

    const std::string client_id_;
    const std::string client_secret_;
    const std::string redirect_uri_;
};

// ── The test double ──────────────────────────────────────────────────────────
//
// Active ONLY when AUTH_FAKE_IDENTITY=1 and GOOGLE_CLIENT_ID is unset (see
// make_identity_provider). It accepts base64url(json) as the authorization
// code and returns whatever identity that JSON describes, which is what lets
// the suite walk every row of the account-linking table — including the
// password-clearing row — without a browser or a Google account.
class FakeIdentityProvider : public IdentityProvider {
public:
    explicit FakeIdentityProvider(std::string app_base_url)
        : app_base_url_(strip_trailing_slashes(std::move(app_base_url))) {
        CROW_LOG_WARNING << "";
        CROW_LOG_WARNING << "  ****************************************************************";
        CROW_LOG_WARNING << "  *  AUTH_FAKE_IDENTITY=1: Google sign-in is FAKED.              *";
        CROW_LOG_WARNING << "  *  Anyone can sign in as ANY email by crafting the callback    *";
        CROW_LOG_WARNING << "  *  code. This is for the test suite only and must NEVER be     *";
        CROW_LOG_WARNING << "  *  set in a deployed environment.                              *";
        CROW_LOG_WARNING << "  ****************************************************************";
        CROW_LOG_WARNING << "";
    }

    // Points at a route that does not exist, on purpose: the fake has no
    // consent screen, and a test that follows this URL should fail loudly
    // rather than appear to work.
    std::string authorize_url(const std::string& state) const override {
        return app_base_url_ + "/fake-google-consent?state=" + url_encode(state);
    }

    GoogleIdentity exchange_code(const std::string& code) const override {
        std::string decoded;
        if (!base64url_decode(code, decoded)) {
            throw IdentityError("fake identity: code is not base64url");
        }
        json claims;
        try {
            claims = json::parse(decoded);
        } catch (const std::exception&) {
            throw IdentityError("fake identity: code does not decode to JSON");
        }
        if (!claims.is_object()) {
            throw IdentityError("fake identity: code is not a JSON object");
        }
        return identity_from_claims(claims);
    }

private:
    const std::string app_base_url_;
};

}  // namespace

bool base64url_decode(const std::string& in, std::string& out) {
    static const auto table = [] {
        std::vector<int> t(256, -1);
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(B64URL[i])] = i;
        return t;
    }();

    out.clear();
    out.reserve(in.size() * 3 / 4 + 3);

    unsigned int buffer = 0;
    int bits = 0;
    for (char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '=') break;  // tolerate padding even though we never emit it
        const int v = table[c];
        if (v < 0) return false;
        buffer = (buffer << 6) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

std::string base64url_encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    unsigned int buffer = 0;
    int bits = 0;
    for (char ch : in) {
        buffer = (buffer << 8) | static_cast<unsigned char>(ch);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(B64URL[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(B64URL[(buffer << (6 - bits)) & 0x3F]);
    }
    return out;  // no padding: '=' is not cookie- or URL-friendly
}

std::unique_ptr<IdentityProvider> make_identity_provider() {
    const std::string client_id     = env_or("GOOGLE_CLIENT_ID");
    const std::string client_secret = env_or("GOOGLE_CLIENT_SECRET");
    const std::string app_base_url  = env_or("APP_BASE_URL", "http://localhost:5173");
    const bool        want_fake     = env_or("AUTH_FAKE_IDENTITY") == "1";

    // GOOGLE_CLIENT_ID set means this is a real deployment, and the fake is
    // refused outright rather than merged with it. An environment where a
    // stray AUTH_FAKE_IDENTITY could shadow a configured client would let
    // anyone sign in as anyone, so the real credentials always win and the
    // attempt is logged as an error.
    if (!client_id.empty()) {
        if (want_fake) {
            CROW_LOG_ERROR << "auth: AUTH_FAKE_IDENTITY is set but GOOGLE_CLIENT_ID is "
                              "configured; IGNORING the fake identity provider. Unset "
                              "AUTH_FAKE_IDENTITY.";
        }
        if (client_secret.empty()) {
            CROW_LOG_ERROR << "auth: GOOGLE_CLIENT_ID is set but GOOGLE_CLIENT_SECRET is "
                              "missing; Google sign-in is disabled.";
            return nullptr;
        }
        CROW_LOG_INFO << "auth: Google sign-in enabled (redirect_uri "
                      << strip_trailing_slashes(app_base_url)
                      << "/api/auth/google/callback)";
        return std::make_unique<GoogleIdentityProvider>(client_id, client_secret,
                                                        app_base_url);
    }

    if (want_fake) {
        return std::make_unique<FakeIdentityProvider>(app_base_url);
    }

    CROW_LOG_INFO << "auth: Google sign-in is not configured "
                     "(GOOGLE_CLIENT_ID unset); /api/auth/google/start will answer 503.";
    return nullptr;
}

}  // namespace auth
