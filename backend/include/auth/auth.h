#pragma once
#include "db/db_pool.h"
#include "models/user.h"
#include <optional>
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
std::optional<User> verify_credentials(DbPool& pool,
                                       const std::string& email,
                                       const std::string& password);

// ── Session ───────────────────────────────────────────────────────────────────

// Creates a session for the given user, returns the session token.
std::string create_session(DbPool& pool, const std::string& user_id);

// Looks up an active session and returns the associated user.
std::optional<User> get_session_user(DbPool& pool,
                                     const std::string& session_token);

// Deletes a session (logout).
void delete_session(DbPool& pool, const std::string& session_token);

} // namespace auth
