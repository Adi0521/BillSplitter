#pragma once
#include <pqxx/pqxx>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>

// Thread-safe PostgreSQL connection pool.
//
// Connections are created lazily: the pool starts with `pool_size` empty slots
// and opens a real connection the first time a slot is used. This means the
// backend starts even if the database is briefly unreachable — the failure
// surfaces on the request that needs it, not at process launch.
//
// Acquire with pool.acquire(), which returns an RAII guard that returns the
// connection to the pool on destruction.
class DbPool {
public:
    explicit DbPool(std::string conn_str, int pool_size = 8)
        : conn_str_(std::move(conn_str)) {
        if (pool_size < 1) throw std::invalid_argument("pool_size must be >= 1");
        for (int i = 0; i < pool_size; ++i) {
            pool_.push(nullptr);  // empty slot, filled on first use
        }
    }

    // RAII guard: holds one connection, returns it to the pool when destroyed.
    // Non-copyable and non-movable — guaranteed copy elision (C++17) makes a
    // move constructor unnecessary, and having one risks returning a null
    // connection to the pool alongside the real one.
    class Guard {
    public:
        Guard(DbPool& pool, std::unique_ptr<pqxx::connection> conn)
            : pool_(pool), conn_(std::move(conn)) {}

        ~Guard() { pool_.release(std::move(conn_)); }

        pqxx::connection& get() { return *conn_; }
        pqxx::connection* operator->() { return conn_.get(); }
        pqxx::connection& operator*() { return *conn_; }

        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&)                 = delete;
        Guard& operator=(Guard&&)      = delete;

    private:
        DbPool& pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    // Blocks until a slot is free. Throws pqxx::broken_connection if the
    // database cannot be reached; the slot is returned to the pool either way.
    Guard acquire() {
        std::unique_ptr<pqxx::connection> conn;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !pool_.empty(); });
            conn = std::move(pool_.front());
            pool_.pop();
        }

        // libpqxx 7 has no reconnect: a closed connection is replaced outright.
        if (!conn || !conn->is_open()) {
            try {
                conn = std::make_unique<pqxx::connection>(conn_str_);
            } catch (...) {
                release(nullptr);  // give the slot back before propagating
                throw;
            }
        }
        return Guard(*this, std::move(conn));
    }

    // Opens one connection to verify configuration. Returns false and leaves
    // the pool usable if the database is unreachable.
    bool ping() noexcept {
        try {
            auto guard = acquire();
            pqxx::nontransaction txn(*guard);
            txn.exec("SELECT 1");
            return true;
        } catch (const std::exception& e) {
            std::cerr << "DbPool: ping failed: " << e.what() << "\n";
            return false;
        }
    }

private:
    void release(std::unique_ptr<pqxx::connection> conn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_.push(std::move(conn));  // may be null: an empty slot
        }
        cv_.notify_one();
    }

    const std::string conn_str_;
    std::queue<std::unique_ptr<pqxx::connection>> pool_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
