#include "storage/table_file.hpp"
#include "test_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
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
    return std::filesystem::path("/tmp") / ("scratch_db_" + name + ".tbl");
}

void multiple_pages_and_reopen() {
    const std::filesystem::path path = temp_path("multiple_pages");
    std::remove(path.c_str());

    std::vector<RecordId> record_ids;
    const auto record = bytes(5, 1000);
    {
        TableFile table(path);
        for (int i = 0; i < 12; ++i) {
            record_ids.push_back(table.insert_record(record));
        }
        require(table.page_count() > 1, "expected more than one page");
    }

    {
        TableFile reopened(path);
        std::vector<uint8_t> out;
        require(reopened.page_count() > 1, "page count was not preserved");
        for (RecordId record_id : record_ids) {
            require(reopened.read_record(record_id, out) && out == record, "record mismatch after reopen");
        }
    }

    std::remove(path.c_str());
}

void delete_and_update_by_record_id() {
    const std::filesystem::path path = temp_path("record_id_ops");
    std::remove(path.c_str());
    TableFile table(path);

    RecordId record_id = table.insert_record(bytes(1, 50));
    const auto updated = bytes(2, 70);
    require(table.update_record(record_id, updated), "update failed");
    std::vector<uint8_t> out;
    require(table.read_record(record_id, out) && out == updated, "updated bytes mismatch");
    require(table.delete_record(record_id), "delete failed");
    require(!table.read_record(record_id, out), "deleted record still readable");

    std::remove(path.c_str());
}

void relocate_growing_update() {
    const std::filesystem::path path = temp_path("relocate");
    std::remove(path.c_str());
    TableFile table(path);

    RecordId moving = table.insert_record(bytes(1, 100));
    for (int i = 0; i < 3; ++i) {
        table.insert_record(bytes(static_cast<uint8_t>(10 + i), 1200));
    }

    const auto large = bytes(90, 3000);
    RecordId old = moving;
    require(table.update_record(moving, large), "relocation update failed");
    require(moving.page_id != old.page_id || moving.slot_id != old.slot_id, "record_id did not change");

    std::vector<uint8_t> out;
    require(table.read_record(moving, out) && out == large, "new record mismatch");
    require(!table.read_record(old, out), "old record_id still readable");

    std::remove(path.c_str());
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"table pages/reopen", multiple_pages_and_reopen});
    tests.push_back({"table record id ops", delete_and_update_by_record_id});
    tests.push_back({"table relocate", relocate_growing_update});
    return run_tests(tests);
}
