#pragma once
#include "auth/identity_provider.h"
#include "db/db_pool.h"
#include "models/user.h"
#include <optional>
#include <stdexcept>
#include <string>

namespace auth {

// Generate a cryptographically random hex string of `bytes` random bytes.
std::string generate_token(int bytes = 32);

// PBKDF2-SHA256: derive a hex hash from password + hex salt.
// Returns the hex-encoded derived key.
std::string pbkdf2_hash(const std::string& password,
                        const std::string& salt_hex,
                        int iterations = 100000,
                        int key_len    = 32);

// ── Account management ────────────────────────────────────────────────────────

// Creates a new user. Returns nullopt if email already exists.
std::optional<User> register_user(DbPool& pool,
                                  const std::string& email,
                                  const std::string& password,
                                  const std::string& display_name = "");

// Verifies credentials. Returns nullopt on bad email/password.
//
// Since migration 006, password_hash is nullable: a Google-only account has no
// password at all. Such a row fails exactly as a wrong password does — same
// nullopt, and the PBKDF2 work is still performed against a dummy hash so the
// response time does not tell an attacker which addresses are Google-only.
std::optional<User> verify_credentials(DbPool& pool,
                                       const std::string& email,
                                       const std::string& password);

// ── Sign in with Google ───────────────────────────────────────────────────────

// The Google identity cannot be joined to the local account it collides with.
// Callers answer 409; the message is safe to show a user.
class AccountLinkConflict : public std::runtime_error {
public:
    explicit AccountLinkConflict(const std::string& what) : std::runtime_error(what) {}
};

struct GoogleSignInResult {
    User user;
    bool created          = false;  // the account was created by this sign-in
    bool password_cleared = false;  // an unverified account's password was wiped
    int  seats_linked     = 0;      // split seats matched by verified email
};

// Applies the account-linking rules from docs/api.md ("Account linking — the
// rules that prevent a takeover") to a Google identity, in ONE transaction,
// then matches split seats by calling the SQL function
// link_seats_by_verified_email().
//
// Throws AccountLinkConflict when an unverified Google address collides with an
// existing local account, which is the one case where there is no safe answer.
GoogleSignInResult sign_in_with_google(DbPool& pool, const GoogleIdentity& identity);

// ── Session ───────────────────────────────────────────────────────────────────

// Creates a session for the given user, returns the session token.
std::string create_session(DbPool& pool, const std::string& user_id);

// Looks up an active session and returns the associated user.
std::optional<User> get_session_user(DbPool& pool,
                                     const std::string& session_token);

// Deletes a session (logout).
void delete_session(DbPool& pool, const std::string& session_token);

} // namespace auth
