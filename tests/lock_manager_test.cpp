#include "concurrency/lock_manager.hpp"
#include "test_utils.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

void read_read_is_allowed() {
    LockManager manager;
    std::atomic<bool> second_reader_entered = false;

    {
        LockManager::TableLock first = manager.lock_table_shared("db/table");
        std::thread second([&]() {
            LockManager::TableLock lock = manager.lock_table_shared("db/table");
            second_reader_entered = true;
        });

        second.join();
        require(second_reader_entered.load(), "second reader was blocked by shared lock");
    }
}

void read_write_blocks_until_reader_releases() {
    LockManager manager;
    std::atomic<bool> writer_entered = false;

    {
        LockManager::TableLock reader = manager.lock_table_shared("db/table");
        std::thread writer([&]() {
            LockManager::TableLock lock = manager.lock_table_exclusive("db/table");
            writer_entered = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(!writer_entered.load(), "writer entered while reader held shared lock");
        reader = LockManager::TableLock();
        writer.join();
    }

    require(writer_entered.load(), "writer did not enter after reader released");
}

void write_write_is_serialized() {
    LockManager manager;
    std::atomic<int> active_writers = 0;
    std::atomic<int> max_active_writers = 0;

    auto writer = [&]() {
        LockManager::TableLock lock = manager.lock_table_exclusive("db/table");
        const int active = ++active_writers;
        int old_max = max_active_writers.load();
        while (active > old_max && !max_active_writers.compare_exchange_weak(old_max, active)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        --active_writers;
    };

    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back(writer);
    }
    for (std::thread& thread : writers) {
        thread.join();
    }

    require(max_active_writers.load() == 1, "exclusive writers overlapped");
}

void timed_lock_fails_cleanly() {
    LockManager manager;
    LockManager::TableLock writer = manager.lock_table_exclusive("db/table");

    std::atomic<bool> lock_succeeded = true;
    std::atomic<bool> returned = false;
    auto start = std::chrono::steady_clock::now();
    std::thread contender([&]() {
        LockManager::TableLock second = manager.try_lock_table_exclusive_for("db/table", std::chrono::milliseconds(25));
        lock_succeeded = static_cast<bool>(second);
        returned = true;
    });
    contender.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    require(returned.load(), "timed lock did not return");
    require(!lock_succeeded.load(), "timed lock unexpectedly succeeded");
    require(elapsed < std::chrono::seconds(1), "timed lock did not return promptly");
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"lock read/read", read_read_is_allowed});
    tests.push_back({"lock read/write", read_write_blocks_until_reader_releases});
    tests.push_back({"lock write/write", write_write_is_serialized});
    tests.push_back({"lock timeout", timed_lock_fails_cleanly});
    return run_tests(tests);
}
