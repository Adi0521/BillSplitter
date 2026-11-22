#pragma once
#include <memory>
#include <stdexcept>
#include <string>

// Sign in with Google, behind a seam.
//
// The reason for the seam is testability: the callback's interesting behaviour
// is the account-linking table in docs/api.md, and none of it can be exercised
// if the only way to obtain an identity is a live round trip to Google's
// consent screen. With AUTH_FAKE_IDENTITY=1 the suite drives the whole flow
// offline and deterministically — see FakeIdentityProvider in
// src/auth/identity_provider.cpp for how tightly that is fenced off.
//
// Modelled on receipt::TesseractPool and the rest of services/: an interface
// plus a factory that reads the environment, so route code never touches
// configuration or libcurl directly.

namespace auth {

// What Google tells us about the person who just signed in.
//
// `sub` — not the email — identifies the account. A Google account can change
// its address, and `sub` is what proves it is the same person afterwards.
//
// `email_verified` is the whole point of this feature: it is the only signal in
// the system that an address belongs to the person claiming it, and therefore
// the only thing that makes matching a split seat by email safe.
struct GoogleIdentity {
    std::string sub;
    std::string email;
    std::string display_name;
    bool        email_verified = false;
};

// The exchange failed: Google refused the code, the network died, the response
// was not the shape we expect. Never the user's fault in a way they can fix by
// retyping something, and never a 500 — the caller answers 401 or 502.
class IdentityError : public std::runtime_error {
public:
    explicit IdentityError(const std::string& what) : std::runtime_error(what) {}
};

class IdentityProvider {
public:
    virtual ~IdentityProvider() = default;

    // Where to send the browser to ask for consent. `state` is echoed back to
    // the callback and is what ties the return leg to the request that started
    // it.
    virtual std::string authorize_url(const std::string& state) const = 0;

    // Trades an authorization code for the identity behind it.
    // Throws IdentityError on any failure; the caller turns that into a
    // 401/502.
    virtual GoogleIdentity exchange_code(const std::string& code) const = 0;
};

// Builds the provider from the environment:
//
//   GOOGLE_CLIENT_ID / GOOGLE_CLIENT_SECRET   the real thing
//   APP_BASE_URL                              redirect_uri and the return target
//   AUTH_FAKE_IDENTITY=1                      the offline test double
//
// Returns nullptr when Google is not configured and the fake is off, so that
// /api/auth/google/start can answer 503 with a clear message instead of
// redirecting the user into a broken consent screen.
std::unique_ptr<IdentityProvider> make_identity_provider();

// Decodes a base64url segment (no padding required). Returns false on any
// character outside the alphabet. Exposed because the ID token payload and the
// `state` cookie both use this encoding.
bool base64url_decode(const std::string& in, std::string& out);

// Encodes bytes as base64url without padding.
std::string base64url_encode(const std::string& in);

} // namespace auth
