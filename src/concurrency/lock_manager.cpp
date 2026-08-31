#include "concurrency/lock_manager.hpp"

#include <unordered_map>

namespace {
    thread_local std::unordered_map<std::string, uint16_t> held_table_locks;
}

LockManager::TableLock::TableLock(std::string table_key, std::shared_ptr<std::shared_timed_mutex> mutex, TableLockMode mode)
    : table_key_(std::move(table_key)), mutex_(std::move(mutex)), mode_(mode), owns_lock_(true) {
    uint16_t& held_count = held_table_locks[table_key_];
    if (held_count == 0) {
        if (mode_ == TableLockMode::shared) {
            mutex_->lock_shared();
        } else {
            mutex_->lock();
        }
        locked_mutex_ = true;
    }
    ++held_count;
}

LockManager::TableLock::TableLock(
    std::string table_key,
    std::shared_ptr<std::shared_timed_mutex> mutex,
    TableLockMode mode,
    std::chrono::milliseconds timeout
) : table_key_(std::move(table_key)), mutex_(std::move(mutex)), mode_(mode), owns_lock_(false) {
    uint16_t& held_count = held_table_locks[table_key_];
    if (held_count == 0) {
        bool locked = false;
        if (mode_ == TableLockMode::shared) {
            locked = mutex_->try_lock_shared_for(timeout);
        } else {
            locked = mutex_->try_lock_for(timeout);
        }
        if (!locked) {
            held_table_locks.erase(table_key_);
            return;
        }
        locked_mutex_ = true;
    }
    ++held_count;
    owns_lock_ = true;
}

LockManager::TableLock::~TableLock() {
    unlock();
}

LockManager::TableLock::TableLock(TableLock&& other) noexcept
    : table_key_(std::move(other.table_key_)),
      mutex_(std::move(other.mutex_)),
      mode_(other.mode_),
      owns_lock_(other.owns_lock_),
      locked_mutex_(other.locked_mutex_) {
    other.owns_lock_ = false;
    other.locked_mutex_ = false;
}

LockManager::TableLock& LockManager::TableLock::operator=(TableLock&& other) noexcept {
    if (this != &other) {
        unlock();
        table_key_ = std::move(other.table_key_);
        mutex_ = std::move(other.mutex_);
        mode_ = other.mode_;
        owns_lock_ = other.owns_lock_;
        locked_mutex_ = other.locked_mutex_;
        other.owns_lock_ = false;
        other.locked_mutex_ = false;
    }
    return *this;
}

LockManager::TableLock::operator bool() const {
    return owns_lock_;
}

void LockManager::TableLock::unlock() {
    if (!owns_lock_ || mutex_ == nullptr) {
        return;
    }

    auto found = held_table_locks.find(table_key_);
    if (found != held_table_locks.end()) {
        --found->second;
        if (found->second == 0) {
            held_table_locks.erase(found);
            if (locked_mutex_) {
                if (mode_ == TableLockMode::shared) {
                    mutex_->unlock_shared();
                } else {
                    mutex_->unlock();
                }
            }
        }
    }
    owns_lock_ = false;
    locked_mutex_ = false;
}

LockManager::TableLock LockManager::lock_table_shared(const std::string& table_key) {
    return TableLock(table_key, mutex_for_table(table_key), TableLockMode::shared);
}

LockManager::TableLock LockManager::lock_table_exclusive(const std::string& table_key) {
    return TableLock(table_key, mutex_for_table(table_key), TableLockMode::exclusive);
}

LockManager::TableLock LockManager::try_lock_table_shared_for(const std::string& table_key, std::chrono::milliseconds timeout) {
    return TableLock(table_key, mutex_for_table(table_key), TableLockMode::shared, timeout);
}

LockManager::TableLock LockManager::try_lock_table_exclusive_for(const std::string& table_key, std::chrono::milliseconds timeout) {
    return TableLock(table_key, mutex_for_table(table_key), TableLockMode::exclusive, timeout);
}

std::shared_ptr<std::shared_timed_mutex> LockManager::mutex_for_table(const std::string& table_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<std::shared_timed_mutex> table_mutex = table_locks_[table_key].lock();
    if (table_mutex == nullptr) {
        table_mutex = std::make_shared<std::shared_timed_mutex>();
        table_locks_[table_key] = table_mutex;
    }
    return table_mutex;
}
