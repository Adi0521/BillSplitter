#include "auth/auth.h"
#include <crow/logging.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pqxx/pqxx>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace auth {

std::string generate_token(int bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), bytes) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    std::ostringstream oss;
    for (auto b : buf) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return oss.str();
}

std::string pbkdf2_hash(const std::string& password,
                        const std::string& salt_hex,
                        int iterations,
                        int key_len) {
    // Convert hex salt to bytes
    std::vector<unsigned char> salt_bytes(salt_hex.size() / 2);
    for (size_t i = 0; i < salt_bytes.size(); ++i) {
        salt_bytes[i] = (unsigned char)std::stoi(
            salt_hex.substr(i * 2, 2), nullptr, 16);
    }

    std::vector<unsigned char> dk(key_len);
    if (PKCS5_PBKDF2_HMAC(
            password.data(), (int)password.size(),
            salt_bytes.data(), (int)salt_bytes.size(),
            iterations,
            EVP_sha256(),
            key_len, dk.data()) != 1) {
        throw std::runtime_error("PBKDF2 failed");
    }

    std::ostringstream oss;
    for (auto b : dk) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return oss.str();
}

std::optional<User> register_user(DbPool& pool,
                                  const std::string& email,
                                  const std::string& password,
                                  const std::string& display_name) {
    if (password.size() < 8) {
        throw std::invalid_argument("Password must be at least 8 characters");
    }

    std::string salt = generate_token(16);
    std::string hash = pbkdf2_hash(password, salt);

    auto conn = pool.acquire();
    pqxx::work txn(*conn);

    try {
        auto r = txn.exec(
            "INSERT INTO users (email, display_name, password_hash, password_salt)"
            "  VALUES ($1, $2, $3, $4)"
            "  RETURNING id, email, COALESCE(display_name,''), created_at",
            pqxx::params{email, display_name, hash, salt});

        txn.commit();

        User u;
        u.id           = r[0][0].as<std::string>();
        u.email        = r[0][1].as<std::string>();
        u.display_name = r[0][2].as<std::string>();
        u.created_at   = r[0][3].as<std::string>();
        return u;
    } catch (const pqxx::unique_violation&) {
        txn.abort();
        return std::nullopt;  // email already registered
    }
}

namespace {

// A salt/hash pair that no password can ever match, used when there is nothing
// real to compare against: the address is not registered, or the account is
// Google-only and has no password.
//
// The point is timing. Returning early in those cases makes a password login
// measurably faster than a wrong-password login, which turns the endpoint into
// an oracle for "is this address registered" and "is this account Google-only"
// — exactly the disclosure the shared 401 message exists to prevent. Paying the
// PBKDF2 cost against a dummy keeps both paths the same shape.
//
// The dummy hash is derived from random bytes at first use rather than being a
// constant, so it is not merely unguessable but unstated anywhere.
const std::string& dummy_salt() {
    static const std::string s = "00000000000000000000000000000000";
    return s;
}
const std::string& dummy_hash() {
    static const std::string h = pbkdf2_hash(generate_token(32), dummy_salt());
    return h;
}

}  // namespace

std::optional<User> verify_credentials(DbPool& pool,
                                       const std::string& email,
                                       const std::string& password) {
    auto conn = pool.acquire();
    pqxx::work txn(*conn);

    auto r = txn.exec(
        "SELECT id, email, COALESCE(display_name,''), created_at,"
        "       password_hash, password_salt"
        "  FROM users WHERE email = $1",
        pqxx::params{email});
    txn.commit();

    // Since migration 006 a row may carry no password at all (Google-only), and
    // the address may not exist. Both still do the work below and both end in
    // the same nullopt as a wrong password.
    const bool has_password =
        !r.empty() && !r[0][4].is_null() && !r[0][5].is_null();

    std::string stored_hash = has_password ? r[0][4].as<std::string>() : dummy_hash();
    std::string salt        = has_password ? r[0][5].as<std::string>() : dummy_salt();
    std::string computed    = pbkdf2_hash(password, salt);

    // Constant-time compare so response timing does not leak the hash prefix.
    const bool match =
        computed.size() == stored_hash.size() &&
        CRYPTO_memcmp(computed.data(), stored_hash.data(), computed.size()) == 0;

    if (!has_password || !match) return std::nullopt;

    User u;
    u.id           = r[0][0].as<std::string>();
    u.email        = r[0][1].as<std::string>();
    u.display_name = r[0][2].as<std::string>();
    u.created_at   = r[0][3].as<std::string>();
    return u;
}

// ── Sign in with Google ───────────────────────────────────────────────────────

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// Every statement below returns the same four columns in the same order, so the
// User is assembled in one place.
User user_from_row(const pqxx::row& row) {
    User u;
    u.id           = row[0].as<std::string>();
    u.email        = row[1].as<std::string>();
    u.display_name = row[2].as<std::string>();
    u.created_at   = row[3].as<std::string>();
    return u;
}

const char* const USER_COLUMNS =
    " RETURNING id, email, COALESCE(display_name,''), created_at";

}  // namespace

GoogleSignInResult sign_in_with_google(DbPool& pool, const GoogleIdentity& identity) {
    GoogleSignInResult out;

    auto conn = pool.acquire();
    // One transaction for the whole table of rules. Resolving the account,
    // linking the subject, clearing a password and matching seats have to be
    // atomic: a failure between any two of them leaves an account that is
    // neither reachable by password nor by Google.
    pqxx::work txn(*conn);

    // ── Row 1: a user already holds this google_sub ──────────────────────────
    // Resolved by subject FIRST, before email, because the subject is the
    // identity. Matching on email first would hand the account over to whoever
    // currently holds the address if Google ever reassigned it.
    auto by_sub = txn.exec(
        "SELECT id, email, email_verified, COALESCE(display_name,'')"
        "  FROM users WHERE google_sub = $1 FOR UPDATE",
        pqxx::params{identity.sub});

    if (!by_sub.empty()) {
        const std::string uid             = by_sub[0][0].as<std::string>();
        const std::string current_email   = by_sub[0][1].as<std::string>();
        const bool        current_verified = by_sub[0][2].as<bool>();
        const std::string current_name    = by_sub[0][3].as<std::string>();

        // "Their address is refreshed from Google" — unless some other account
        // already holds it, in which case refreshing would raise a unique
        // violation and fail a sign-in that has nothing wrong with it. Keeping
        // the old address is the conservative outcome.
        auto clash = txn.exec(
            "SELECT 1 FROM users WHERE lower(email) = lower($1) AND id <> $2",
            pqxx::params{identity.email, uid});

        const std::string target_email = clash.empty() ? identity.email : current_email;
        const bool same_address = to_lower(target_email) == to_lower(identity.email);
        // Verification only ever describes the address it was granted for. If
        // the account keeps an older address, it keeps that address's status.
        const bool verified =
            same_address ? (current_verified || identity.email_verified) : current_verified;
        const std::string name =
            current_name.empty() ? identity.display_name : current_name;

        auto r = txn.exec(
            std::string(
                "UPDATE users SET email = $2, email_verified = $3,"
                "       verified_at = CASE WHEN $3 THEN COALESCE(verified_at, now())"
                "                         ELSE verified_at END,"
                "       display_name = NULLIF($4,'')"
                " WHERE id = $1") + USER_COLUMNS,
            pqxx::params{uid, target_email, verified, name});
        out.user = user_from_row(r[0]);

    } else {
        // ── Rows 2 and 3: no subject match, but this address is taken ────────
        // Case-insensitively: the column's UNIQUE constraint is case-sensitive,
        // so "A@x.com" and "a@x.com" can both exist. Preferring the exact match
        // makes which one is chosen deterministic rather than dependent on
        // physical row order.
        auto by_email = txn.exec(
            "SELECT id, email_verified, password_hash IS NOT NULL,"
            "       COALESCE(display_name,'')"
            "  FROM users WHERE lower(email) = lower($1)"
            "  ORDER BY (email = $1) DESC, created_at ASC"
            "  LIMIT 1 FOR UPDATE",
            pqxx::params{identity.email});

        if (!by_email.empty()) {
            const std::string uid          = by_email[0][0].as<std::string>();
            const bool        verified     = by_email[0][1].as<bool>();
            const bool        had_password = by_email[0][2].as<bool>();
            const std::string current_name = by_email[0][3].as<std::string>();
            const std::string name =
                current_name.empty() ? identity.display_name : current_name;

            // An address Google has NOT verified is an unproven claim, exactly
            // like the address a password account was registered with — so it
            // cannot be used to take one over. This is deliberately refused for
            // BOTH rows 2 and 3: joining an unverified Google identity to any
            // existing account would hand that account to whoever typed the
            // address into Google, which is the takeover this whole feature is
            // built to prevent.
            if (!identity.email_verified) {
                CROW_LOG_WARNING
                    << "auth: refused to link an unverified Google address to an "
                       "existing account (user " << uid << ")";
                throw AccountLinkConflict(
                    "Google has not verified this email address, so it cannot be "
                    "linked to an existing account.");
            }

            if (verified) {
                // Row 2: the address was already proven, so the accounts are the
                // same person. Link the subject and leave the password alone —
                // they keep both ways in.
                auto r = txn.exec(
                    std::string(
                        "UPDATE users SET google_sub = $2,"
                        "       verified_at = COALESCE(verified_at, now()),"
                        "       display_name = NULLIF($3,'')"
                        " WHERE id = $1") + USER_COLUMNS,
                    pqxx::params{uid, identity.sub, name});
                out.user = user_from_row(r[0]);
            } else {
                // Row 3: Google wins. The account's address was never proven, so
                // it has no claim to it; the Google user demonstrably controls
                // it. The password goes.
                //
                // Both password columns are cleared in the SAME statement that
                // sets google_sub. Splitting it in two would leave the row
                // momentarily with neither a password nor a subject, which
                // chk_users_has_credential rejects — the transaction would abort
                // on a constraint rather than on anything wrong with the input.
                auto r = txn.exec(
                    std::string(
                        "UPDATE users SET google_sub = $2,"
                        "       email_verified = TRUE,"
                        "       verified_at = now(),"
                        "       password_hash = NULL,"
                        "       password_salt = NULL,"
                        "       display_name = NULLIF($3,'')"
                        " WHERE id = $1") + USER_COLUMNS,
                    pqxx::params{uid, identity.sub, name});
                out.user = user_from_row(r[0]);
                out.password_cleared = had_password;
                if (had_password) {
                    // Destructive and unrecoverable (there is no password
                    // reset), so it is recorded server-side as well as reported
                    // to the UI.
                    CROW_LOG_WARNING
                        << "auth: Google verified an address held by an unverified "
                           "account; cleared the password of user " << uid;
                }
            }

        } else {
            // ── Row 4: no match at all ──────────────────────────────────────
            // No password: a Google-only account. chk_users_has_credential is
            // satisfied by google_sub.
            try {
                auto r = txn.exec(
                    std::string(
                        "INSERT INTO users (email, display_name, google_sub,"
                        "                   email_verified, verified_at)"
                        " VALUES ($1, NULLIF($2,''), $3, $4,"
                        "         CASE WHEN $4 THEN now() ELSE NULL END)") + USER_COLUMNS,
                    pqxx::params{identity.email, identity.display_name, identity.sub,
                                 identity.email_verified});
                out.user    = user_from_row(r[0]);
                out.created = true;
            } catch (const pqxx::unique_violation&) {
                // Two callbacks for the same brand-new identity at once. The
                // loser is told to retry rather than being handed a 500.
                throw AccountLinkConflict(
                    "Another sign-in for this account is already in progress. "
                    "Please try again.");
            }
        }
    }

    // Seat matching, in the same transaction, for every path above.
    //
    // Called unconditionally and with no email_verified check here on purpose:
    // the function re-reads the flag itself and returns 0 when it is false.
    // Deciding "this identity is verified, so link seats" at the call site is
    // precisely the trust that migration 006 refuses to extend.
    auto seats = txn.exec("SELECT link_seats_by_verified_email($1)",
                          pqxx::params{out.user.id});
    out.seats_linked = seats[0][0].as<int>();

    txn.commit();
    return out;
}

std::string create_session(DbPool& pool, const std::string& user_id) {
    std::string token = generate_token(32);
    auto conn = pool.acquire();
    pqxx::work txn(*conn);
    txn.exec(
        "INSERT INTO sessions (token, user_id, expires_at)"
        "  VALUES ($1, $2, now() + interval '30 days')",
        pqxx::params{token, user_id});
    txn.commit();
    return token;
}

std::optional<User> get_session_user(DbPool& pool,
                                     const std::string& session_token) {
    if (session_token.empty()) return std::nullopt;
    auto conn = pool.acquire();
    pqxx::work txn(*conn);

    auto r = txn.exec(
        "SELECT u.id, u.email, COALESCE(u.display_name,''), u.created_at"
        "  FROM sessions s JOIN users u ON s.user_id = u.id"
        "  WHERE s.token = $1 AND s.expires_at > now()",
        pqxx::params{session_token});
    txn.commit();

    if (r.empty()) return std::nullopt;

    User u;
    u.id           = r[0][0].as<std::string>();
    u.email        = r[0][1].as<std::string>();
    u.display_name = r[0][2].as<std::string>();
    u.created_at   = r[0][3].as<std::string>();
    return u;
}

void delete_session(DbPool& pool, const std::string& session_token) {
    auto conn = pool.acquire();
    pqxx::work txn(*conn);
    txn.exec("DELETE FROM sessions WHERE token = $1", pqxx::params{session_token});
    txn.commit();
}

} // namespace auth
