#include "storage/index_file.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::path("/tmp") / ("scratch_db_index_file_" + name + ".idx");
}

std::vector<uint8_t> key_for(uint16_t value, uint16_t width = 80) {
    std::vector<uint8_t> key(width, static_cast<uint8_t>('a' + (value % 26)));
    key[0] = static_cast<uint8_t>((value >> 8) & 0xff);
    key[1] = static_cast<uint8_t>(value & 0xff);
    return key;
}

void insert_find_and_reload() {
    const std::filesystem::path path = temp_path("reload");
    std::filesystem::remove(path);

    {
        IndexFile index(path);
        require(index.insert(key_for(10), RecordId{2, 3}, false), "insert failed");
        require(index.insert(key_for(10), RecordId{4, 5}, false), "duplicate nonunique insert failed");
        std::vector<RecordId> rows = index.find(key_for(10));
        require(rows.size() == 2, "bad duplicate lookup count");
        require(rows[0].page_id == 2 && rows[0].slot_id == 3, "first lookup rid wrong");
        require(index.sync(), "sync failed");
    }

    {
        IndexFile index(path);
        std::vector<RecordId> rows = index.find(key_for(10));
        require(rows.size() == 2, "reload lookup count wrong");
        require(rows[1].page_id == 4 && rows[1].slot_id == 5, "reload lookup rid wrong");
    }

    std::filesystem::remove(path);
}

void split_into_multiple_leaves() {
    const std::filesystem::path path = temp_path("split");
    std::filesystem::remove(path);

    {
        IndexFile index(path);
        for (uint16_t i = 0; i < 140; ++i) {
            require(index.insert(key_for(i), RecordId{i, static_cast<uint16_t>(i + 1)}, true), "wide insert failed");
        }
        require(index.page_count() > 2, "index did not split into multiple leaves");
        require(index.sync(), "sync failed");
    }

    {
        IndexFile index(path);
        require(index.page_count() > 2, "page count not preserved");
        for (uint16_t i : {uint16_t{0}, uint16_t{51}, uint16_t{139}}) {
            std::vector<RecordId> rows = index.find(key_for(i));
            require(rows.size() == 1, "split lookup count wrong");
            require(rows[0].page_id == i && rows[0].slot_id == i + 1, "split lookup rid wrong");
        }
    }

    std::filesystem::remove(path);
}

void unique_rejects_duplicate_key() {
    const std::filesystem::path path = temp_path("unique");
    std::filesystem::remove(path);

    IndexFile index(path);
    require(index.insert(key_for(7), RecordId{1, 1}, true), "unique insert failed");
    require(!index.insert(key_for(7), RecordId{2, 2}, true), "unique duplicate accepted");

    std::filesystem::remove(path);
}

void remove_from_split_leaf() {
    const std::filesystem::path path = temp_path("remove");
    std::filesystem::remove(path);

    IndexFile index(path);
    for (uint16_t i = 0; i < 90; ++i) {
        require(index.insert(key_for(i), RecordId{i, i}, true), "insert before remove failed");
    }
    require(index.page_count() > 2, "remove test did not split");
    require(index.remove(key_for(70), RecordId{70, 70}), "remove failed");
    require(index.find(key_for(70)).empty(), "removed key still found");
    require(index.find(key_for(71)).size() == 1, "neighbor key missing after remove");

    std::filesystem::remove(path);
}

void build_internal_levels() {
    const std::filesystem::path path = temp_path("internal");
    std::filesystem::remove(path);

    {
        IndexFile index(path);
        for (uint16_t i = 0; i < 600; ++i) {
            require(index.insert(key_for(i, 500), RecordId{i, static_cast<uint16_t>(i + 2)}, true), "internal insert failed");
        }
        require(index.tree_height() > 2, "index did not build multiple internal levels");
        require(index.sync(), "sync failed");
    }

    {
        IndexFile index(path);
        require(index.tree_height() > 2, "tree height not preserved");
        for (uint16_t i : {uint16_t{1}, uint16_t{257}, uint16_t{599}}) {
            std::vector<RecordId> rows = index.find(key_for(i, 500));
            require(rows.size() == 1, "internal lookup count wrong");
            require(rows[0].page_id == i && rows[0].slot_id == i + 2, "internal lookup rid wrong");
        }
    }

    std::filesystem::remove(path);
}

void range_scan_across_leaves() {
    const std::filesystem::path path = temp_path("range");
    std::filesystem::remove(path);

    IndexFile index(path);
    for (uint16_t i = 0; i < 160; ++i) {
        require(index.insert(key_for(i), RecordId{i, i}, true), "range insert failed");
    }
    require(index.page_count() > 2, "range test did not split");

    std::vector<RecordId> rows = index.find_range(key_for(50), true, key_for(55), true);
    require(rows.size() == 6, "inclusive range count wrong");
    require(rows.front().page_id == 50 && rows.back().page_id == 55, "inclusive range bounds wrong");

    rows = index.find_range(key_for(50), false, key_for(55), false);
    require(rows.size() == 4, "exclusive range count wrong");
    require(rows.front().page_id == 51 && rows.back().page_id == 54, "exclusive range bounds wrong");

    rows = index.find_range(std::nullopt, true, key_for(3), true);
    require(rows.size() == 4 && rows.back().page_id == 3, "upper-only range wrong");

    rows = index.find_range(key_for(157), true, std::nullopt, true);
    require(rows.size() == 3 && rows.front().page_id == 157, "lower-only range wrong");

    std::filesystem::remove(path);
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"index insert/find/reload", insert_find_and_reload});
    tests.push_back({"index leaf split", split_into_multiple_leaves});
    tests.push_back({"index unique", unique_rejects_duplicate_key});
    tests.push_back({"index remove", remove_from_split_leaf});
    tests.push_back({"index internal levels", build_internal_levels});
    tests.push_back({"index range scan", range_scan_across_leaves});
    return run_tests(tests);
}
