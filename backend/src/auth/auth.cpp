#include "auth/auth.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pqxx/pqxx>
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

    if (r.empty()) return std::nullopt;

    std::string stored_hash = r[0][4].as<std::string>();
    std::string salt        = r[0][5].as<std::string>();
    std::string computed    = pbkdf2_hash(password, salt);

    // Constant-time compare so response timing does not leak the hash prefix.
    if (computed.size() != stored_hash.size() ||
        CRYPTO_memcmp(computed.data(), stored_hash.data(), computed.size()) != 0) {
        return std::nullopt;
    }

    User u;
    u.id           = r[0][0].as<std::string>();
    u.email        = r[0][1].as<std::string>();
    u.display_name = r[0][2].as<std::string>();
    u.created_at   = r[0][3].as<std::string>();
    return u;
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
