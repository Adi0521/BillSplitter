#pragma once
#include <pqxx/pqxx>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <memory>
#include <stdexcept>

// Thread-safe PostgreSQL connection pool.
// Acquire a connection with pool.acquire(), which returns a guard that
// releases the connection back to the pool on destruction.
class DbPool {
public:
    explicit DbPool(const std::string& conn_str, int pool_size = 8) {
        for (int i = 0; i < pool_size; ++i) {
            pool_.push(std::make_unique<pqxx::connection>(conn_str));
        }
    }

    // RAII guard: holds one connection, releases it when destroyed.
    class Guard {
    public:
        Guard(DbPool& pool, std::unique_ptr<pqxx::connection> conn)
            : pool_(pool), conn_(std::move(conn)) {}

        ~Guard() { pool_.release(std::move(conn_)); }

        pqxx::connection& get() { return *conn_; }
        pqxx::connection* operator->() { return conn_.get(); }
        pqxx::connection& operator*() { return *conn_; }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&) = default;

    private:
        DbPool& pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    Guard acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !pool_.empty(); });
        auto conn = std::move(pool_.front());
        pool_.pop();
        // Reconnect if the connection dropped
        if (!conn->is_open()) {
            conn->activate();
        }
        return Guard(*this, std::move(conn));
    }

private:
    void release(std::unique_ptr<pqxx::connection> conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push(std::move(conn));
        cv_.notify_one();
    }

    std::queue<std::unique_ptr<pqxx::connection>> pool_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
