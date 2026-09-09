#pragma once
#include "db/db_pool.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

// Periodically deletes expired rows from `sessions`.
//
// Nothing else removes them: every login inserts a row with a 30-day expiry and
// the auth lookup simply ignores rows past it. Without a sweep the table grows
// without bound for the life of the deployment.
//
// The thread sleeps on a condition variable rather than in fixed increments, so
// shutdown is immediate instead of waiting out the remainder of an interval.
class SessionSweeper {
public:
    SessionSweeper(DbPool& pool, std::chrono::seconds interval = std::chrono::hours(1))
        : pool_(pool), interval_(interval) {}

    // Non-copyable, non-movable: the running thread captures `this`.
    SessionSweeper(const SessionSweeper&)            = delete;
    SessionSweeper& operator=(const SessionSweeper&) = delete;

    void start() {
        thread_ = std::thread([this] {
            // Sweep once at startup so a long-stopped process catches up
            // immediately rather than after a full interval.
            do {
                sweep();
            } while (!wait_for_next());
        });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    ~SessionSweeper() { stop(); }

private:
    // A failed sweep must never take the process down — the API is fully
    // functional with a stale sessions table, so this logs and retries later.
    void sweep() {
        try {
            auto conn = pool_.acquire();
            pqxx::work txn(*conn);
            auto r = txn.exec("DELETE FROM sessions WHERE expires_at <= now()");
            txn.commit();
            if (r.affected_rows() > 0) {
                std::cout << "session sweeper: removed " << r.affected_rows()
                          << " expired session(s)\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "session sweeper: " << e.what() << "\n";
        }
    }

    // Returns true when it is time to stop.
    bool wait_for_next() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, interval_, [this] { return stopping_; });
        return stopping_;
    }

    DbPool&                   pool_;
    std::chrono::seconds      interval_;
    std::thread               thread_;
    std::mutex                mutex_;
    std::condition_variable   cv_;
    bool                      stopping_ = false;
};
