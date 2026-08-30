#include "storage/table_file.hpp"
#include "storage/wal_manager.hpp"
#include "test_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<uint8_t> bytes(uint8_t seed, std::size_t count) {
    std::vector<uint8_t> data(count);
    for (std::size_t i = 0; i < count; ++i) {
        data[i] = static_cast<uint8_t>(seed + i);
    }
    return data;
}

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::path("/tmp") / ("scratch_db_" + name);
}

void wal_lsn_and_flush() {
    const std::filesystem::path path = temp_path("wal_lsn.log");
    std::remove(path.c_str());

    WalManager wal(path);
    const uint64_t start_lsn = wal.begin_transaction(7);
    require(start_lsn == 1, "start lsn wrong");
    require(wal.last_lsn() == 1, "last lsn wrong after start");
    require(wal.durable_lsn() == 0, "durable lsn advanced before flush");

    const uint64_t commit_lsn = wal.commit_transaction(7);
    require(commit_lsn == 2, "commit lsn wrong");
    require(wal.flush_through(commit_lsn), "flush through commit failed");
    require(wal.durable_lsn() == commit_lsn, "durable lsn wrong after flush");

    std::remove(path.c_str());
}

void page_update_sets_page_lsn() {
    const std::filesystem::path table_path = temp_path("wal_table.tbl");
    const std::filesystem::path wal_path = temp_path("wal_table.log");
    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());

    WalManager wal(wal_path);
    RecordId record_id;
    {
        TableFile table(table_path, &wal);
        record_id = table.insert_record(bytes(1, 40));
        Page page;
        require(table.read_page(record_id.page_id, page), "read page after insert failed");
        require(page.page_lsn() == wal.last_lsn(), "page lsn did not track wal");
        require(table.flush(), "flush with wal failed");
        require(wal.durable_lsn() >= page.page_lsn(), "wal not flushed before page flush");
    }

    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());
}

Page make_page(uint8_t seed) {
    Page page;
    uint16_t slot_id = 0;
    require(page.insert(bytes(seed, 40), slot_id), "make page insert failed");
    return page;
}

Page read_disk_page(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "could not read table file");
    Page page;
    in.read(reinterpret_cast<char*>(page.data()), BYTE_SIZES::PAGE_SIZE);
    require(static_cast<bool>(in), "could not read page bytes");
    return page;
}

bool page_first_record_matches(Page& page, uint8_t seed) {
    std::vector<uint8_t> out;
    return page.read(0, out) && out == bytes(seed, 40);
}

void recovery_redoes_committed_transaction() {
    const std::filesystem::path table_path = temp_path("wal_redo.tbl");
    const std::filesystem::path wal_path = temp_path("wal_redo.log");
    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());

    Page before = make_page(1);
    Page after = make_page(2);
    {
        std::ofstream out(table_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(before.data()), BYTE_SIZES::PAGE_SIZE);
    }
    {
        WalManager wal(wal_path);
        wal.begin_transaction(10);
        wal.log_page_update(10, table_path.string(), 0, before, after);
        wal.commit_transaction(10);
        require(wal.flush(), "wal flush failed");
    }

    WalManager recovery(wal_path);
    require(recovery.recover(), "recovery failed");
    Page page = read_disk_page(table_path);
    require(page_first_record_matches(page, 2), "committed update was not redone");

    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());
}

void crash_after_wal_write_before_page_write() {
    const std::filesystem::path table_path = temp_path("wal_before_page.tbl");
    const std::filesystem::path wal_path = temp_path("wal_before_page.log");
    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());

    Page before = make_page(11);
    Page after = make_page(12);
    {
        std::ofstream out(table_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(before.data()), BYTE_SIZES::PAGE_SIZE);
    }
    {
        WalManager wal(wal_path);
        wal.begin_transaction(70);
        wal.log_page_update(70, table_path.string(), 0, before, after);
        wal.commit_transaction(70);
        require(wal.flush(), "wal flush failed");
    }

    WalManager recovery(wal_path);
    require(recovery.recover(), "recovery after wal-before-page crash failed");
    Page page = read_disk_page(table_path);
    require(page_first_record_matches(page, 12), "committed wal update was not redone");

    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());
}

void recovery_undoes_unfinished_transaction() {
    const std::filesystem::path table_path = temp_path("wal_undo.tbl");
    const std::filesystem::path wal_path = temp_path("wal_undo.log");
    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());

    Page before = make_page(3);
    Page after = make_page(4);
    {
        std::ofstream out(table_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(after.data()), BYTE_SIZES::PAGE_SIZE);
    }
    {
        WalManager wal(wal_path);
        wal.begin_transaction(20);
        wal.log_page_update(20, table_path.string(), 0, before, after);
        require(wal.flush(), "wal flush failed");
    }

    WalManager recovery(wal_path);
    require(recovery.recover(), "recovery failed");
    Page page = read_disk_page(table_path);
    require(page_first_record_matches(page, 3), "unfinished update was not undone");
    require(recovery.last_lsn() == 4, "undo and abort records were not appended during recovery");

    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());
}

void crash_after_page_write_before_commit() {
    const std::filesystem::path table_path = temp_path("page_before_commit.tbl");
    const std::filesystem::path wal_path = temp_path("page_before_commit.log");
    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());

    Page before = make_page(13);
    Page after = make_page(14);
    {
        std::ofstream out(table_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(after.data()), BYTE_SIZES::PAGE_SIZE);
    }
    {
        WalManager wal(wal_path);
        wal.begin_transaction(80);
        wal.log_page_update(80, table_path.string(), 0, before, after);
        require(wal.flush(), "wal flush failed");
    }

    WalManager recovery(wal_path);
    require(recovery.recover(), "recovery after page-before-commit crash failed");
    Page page = read_disk_page(table_path);
    require(page_first_record_matches(page, 13), "uncommitted page write was not undone");

    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());
}

void crash_during_moved_update_is_undone() {
    const std::filesystem::path table_path = temp_path("moved_update.tbl");
    const std::filesystem::path wal_path = temp_path("moved_update.log");
    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());

    RecordId old_record_id;
    RecordId moved_record_id;
    {
        TableFile table(table_path);
        old_record_id = table.insert_record(bytes(15, 100));
        for (int i = 0; i < 3; ++i) {
            table.insert_record(bytes(static_cast<uint8_t>(20 + i), 1200));
        }
    }

    {
        WalManager wal(wal_path);
        require(wal.begin_transaction(90) == 1, "moved update begin failed");
        TableFile table(table_path, &wal, 90);
        moved_record_id = old_record_id;
        require(table.update_record(moved_record_id, bytes(44, 3000)), "moved update failed");
        require(moved_record_id.page_id != old_record_id.page_id || moved_record_id.slot_id != old_record_id.slot_id, "record was not moved");
        require(table.flush(), "moved update flush failed");
        require(wal.flush(), "moved update wal flush failed");
    }

    WalManager recovery(wal_path);
    require(recovery.recover(), "moved update recovery failed");
    TableFile table(table_path);
    std::vector<uint8_t> out;
    require(table.read_record(old_record_id, out) && out == bytes(15, 100), "old record was not restored");
    require(!table.read_record(moved_record_id, out), "moved uncommitted record survived");

    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());
}

void recovery_repeats_history_after_abort() {
    const std::filesystem::path table_path = temp_path("wal_repeat.tbl");
    const std::filesystem::path wal_path = temp_path("wal_repeat.log");
    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());

    Page before = make_page(5);
    Page after = make_page(6);
    {
        std::ofstream out(table_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(after.data()), BYTE_SIZES::PAGE_SIZE);
    }
    {
        WalManager wal(wal_path);
        wal.begin_transaction(30);
        wal.log_page_update(30, table_path.string(), 0, before, after);
        require(wal.flush(), "wal flush failed");
    }

    {
        WalManager recovery(wal_path);
        require(recovery.recover(), "first recovery failed");
    }
    {
        WalManager recovery(wal_path);
        require(recovery.recover(), "second recovery failed");
    }

    Page page = read_disk_page(table_path);
    require(page_first_record_matches(page, 5), "repeat history recovery did not preserve undo");

    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());
}

void rollback_transaction_restores_pages() {
    const std::filesystem::path table_path = temp_path("wal_rollback.tbl");
    const std::filesystem::path wal_path = temp_path("wal_rollback.log");
    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());

    Page before = make_page(7);
    Page after = make_page(8);
    {
        std::ofstream out(table_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(after.data()), BYTE_SIZES::PAGE_SIZE);
    }

    WalManager wal(wal_path);
    wal.begin_transaction(40);
    wal.log_page_update(40, table_path.string(), 0, before, after);
    require(wal.rollback_transaction(40), "rollback failed");

    Page page = read_disk_page(table_path);
    require(page_first_record_matches(page, 7), "rollback did not restore before image");
    require(wal.last_lsn() == 4, "rollback did not append compensation and abort");

    std::remove(table_path.c_str());
    std::remove(wal_path.c_str());
}

void recovery_redoes_committed_create_table() {
    const std::filesystem::path table_dir = temp_path("wal_create_table");
    const std::filesystem::path wal_path = temp_path("wal_create_table.log");
    std::filesystem::remove_all(table_dir);
    std::remove(wal_path.c_str());

    const std::string schema_text = "table items\ncolumn id integer 0 8 0 0\n";
    {
        WalManager wal(wal_path);
        wal.begin_transaction(50);
        wal.log_create_table(50, table_dir, schema_text);
        wal.commit_transaction(50);
        require(wal.flush(), "wal flush failed");
    }

    WalManager recovery(wal_path);
    require(recovery.recover(), "create table recovery failed");
    require(std::filesystem::is_directory(table_dir), "table dir was not recreated");
    require(std::filesystem::is_regular_file(table_dir / "schema.catalog"), "schema was not recreated");
    require(std::filesystem::is_regular_file(table_dir / "data.tbl"), "data file was not recreated");

    std::filesystem::remove_all(table_dir);
    std::remove(wal_path.c_str());
}

void recovery_undoes_unfinished_create_table() {
    const std::filesystem::path table_dir = temp_path("wal_create_undo");
    const std::filesystem::path wal_path = temp_path("wal_create_undo.log");
    std::filesystem::remove_all(table_dir);
    std::remove(wal_path.c_str());

    const std::string schema_text = "table items\ncolumn id integer 0 8 0 0\n";
    {
        WalManager wal(wal_path);
        wal.begin_transaction(60);
        wal.log_create_table(60, table_dir, schema_text);
        require(wal.flush(), "wal flush failed");
    }
    std::filesystem::create_directories(table_dir);

    WalManager recovery(wal_path);
    require(recovery.recover(), "create table undo recovery failed");
    require(!std::filesystem::exists(table_dir), "unfinished create table was not removed");

    std::filesystem::remove_all(table_dir);
    std::remove(wal_path.c_str());
}

void concurrent_wal_appends_get_unique_lsns() {
    const std::filesystem::path wal_path = temp_path("wal_concurrent.log");
    std::remove(wal_path.c_str());

    constexpr int THREAD_COUNT = 4;
    constexpr int RECORDS_PER_THREAD = 25;
    WalManager wal(wal_path);
    std::vector<uint64_t> lsns(static_cast<std::size_t>(THREAD_COUNT * RECORDS_PER_THREAD), 0);

    std::vector<std::thread> threads;
    for (int thread_id = 0; thread_id < THREAD_COUNT; ++thread_id) {
        threads.emplace_back([&wal, &lsns, thread_id]() {
            for (int i = 0; i < RECORDS_PER_THREAD; ++i) {
                const std::size_t index = static_cast<std::size_t>(thread_id * RECORDS_PER_THREAD + i);
                lsns[index] = wal.begin_transaction(static_cast<uint64_t>(index + 1));
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    std::set<uint64_t> unique_lsns(lsns.begin(), lsns.end());
    require(unique_lsns.size() == lsns.size(), "wal produced duplicate lsns");
    require(*unique_lsns.begin() == 1, "wal first lsn wrong after concurrent appends");
    require(*unique_lsns.rbegin() == lsns.size(), "wal last lsn wrong after concurrent appends");
    require(wal.last_lsn() == lsns.size(), "wal last_lsn did not match append count");

    std::remove(wal_path.c_str());
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"wal lsn flush", wal_lsn_and_flush});
    tests.push_back({"wal page lsn", page_update_sets_page_lsn});
    tests.push_back({"wal redo recovery", recovery_redoes_committed_transaction});
    tests.push_back({"crash wal before page", crash_after_wal_write_before_page_write});
    tests.push_back({"wal undo recovery", recovery_undoes_unfinished_transaction});
    tests.push_back({"crash page before commit", crash_after_page_write_before_commit});
    tests.push_back({"crash moved update", crash_during_moved_update_is_undone});
    tests.push_back({"wal repeat history", recovery_repeats_history_after_abort});
    tests.push_back({"wal rollback", rollback_transaction_restores_pages});
    tests.push_back({"wal redo create table", recovery_redoes_committed_create_table});
    tests.push_back({"wal undo create table", recovery_undoes_unfinished_create_table});
    tests.push_back({"wal concurrent append", concurrent_wal_appends_get_unique_lsns});
    return run_tests(tests);
}
