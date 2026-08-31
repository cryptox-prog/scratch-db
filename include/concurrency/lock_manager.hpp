#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

enum class TableLockMode {
    shared,
    exclusive,
};

class LockManager {
public:
    class TableLock {
    public:
        TableLock() = default;
        TableLock(std::string table_key, std::shared_ptr<std::shared_timed_mutex> mutex, TableLockMode mode);
        TableLock(
            std::string table_key,
            std::shared_ptr<std::shared_timed_mutex> mutex,
            TableLockMode mode,
            std::chrono::milliseconds timeout
        );
        ~TableLock();

        TableLock(const TableLock&) = delete;
        TableLock& operator=(const TableLock&) = delete;
        TableLock(TableLock&& other) noexcept;
        TableLock& operator=(TableLock&& other) noexcept;
        explicit operator bool() const;

    private:
        void unlock();

        std::string table_key_;
        std::shared_ptr<std::shared_timed_mutex> mutex_;
        TableLockMode mode_ = TableLockMode::shared;
        bool owns_lock_ = false;
        bool locked_mutex_ = false;
    };

    TableLock lock_table_shared(const std::string& table_key);
    TableLock lock_table_exclusive(const std::string& table_key);
    TableLock try_lock_table_shared_for(const std::string& table_key, std::chrono::milliseconds timeout);
    TableLock try_lock_table_exclusive_for(const std::string& table_key, std::chrono::milliseconds timeout);

private:
    std::shared_ptr<std::shared_timed_mutex> mutex_for_table(const std::string& table_key);

    std::mutex mutex_;
    std::unordered_map<std::string, std::weak_ptr<std::shared_timed_mutex>> table_locks_;
};
