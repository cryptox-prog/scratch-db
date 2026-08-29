#include "storage/page.hpp"
#include "test_utils.hpp"

#include <cstdint>
#include <vector>

namespace {

std::vector<uint8_t> bytes(uint8_t seed, std::size_t count) {
    std::vector<uint8_t> data(count);
    for (std::size_t i = 0; i < count; ++i) {
        data[i] = static_cast<uint8_t>(seed + i);
    }
    return data;
}

uint16_t read_uint16(const uint8_t* ptr) {
    return static_cast<uint16_t>(ptr[0]) | static_cast<uint16_t>(static_cast<uint16_t>(ptr[1]) << 8);
}

void write_uint16(uint8_t* ptr, uint16_t value) {
    ptr[0] = static_cast<uint8_t>(value & 0xff);
    ptr[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void initialize_empty_page() {
    Page page;
    require(page.slot_count() == 0, "slot_count should be 0");
    require(page.contiguous_free_space() == BYTE_SIZES::PAGE_SIZE - BYTE_SIZES::PAGE_HEADER_SIZE, "bad free space");
    require(page.compact_and_get_free_space() == BYTE_SIZES::PAGE_SIZE - BYTE_SIZES::PAGE_HEADER_SIZE, "bad compacted free space");
}

void slot_layout_length_first() {
    Page page;
    uint16_t slot_id = 0;
    const auto record = bytes(1, 25);

    require(page.insert(record, slot_id), "insert failed");
    require(slot_id == 0, "unexpected slot_id");

    const uint8_t* slot = page.data() + BYTE_SIZES::PAGE_HEADER_SIZE;
    const uint16_t size = read_uint16(slot);
    const uint16_t offset = read_uint16(slot + 2);

    require(size == static_cast<uint16_t>(record.size()), "slot size is not first");
    require(offset == static_cast<uint16_t>(BYTE_SIZES::PAGE_SIZE - record.size()), "slot offset is not second");
}

void insert_uses_contiguous_space_first() {
    Page page;
    uint16_t first_slot = 0;
    uint16_t second_slot = 0;
    const auto first = bytes(1, 20);
    const auto second = bytes(2, 30);

    require(page.insert(first, first_slot), "first insert failed");
    const uint8_t* first_slot_ptr = page.data() + BYTE_SIZES::PAGE_HEADER_SIZE;
    const uint16_t first_offset_before = read_uint16(first_slot_ptr + BYTE_SIZES::LENGTH);

    require(page.insert(second, second_slot), "second insert failed");
    const uint16_t first_offset_after = read_uint16(first_slot_ptr + BYTE_SIZES::LENGTH);

    require(first_offset_after == first_offset_before, "insert compacted even though contiguous space was enough");
}

void invalid_slot_count_rejected() {
    Page page;
    write_uint16(page.data(), static_cast<uint16_t>(BYTE_SIZES::MAX_SLOTS + 1));
    write_uint16(page.data() + BYTE_SIZES::SLOT_COUNT, BYTE_SIZES::PAGE_SIZE);

    uint16_t slot_id = 0;
    std::vector<uint8_t> out;
    require(!page.insert(bytes(1, 5), slot_id), "insert accepted invalid slot_count");
    require(!page.read(0, out), "read accepted invalid slot_count");
    require(page.contiguous_free_space() == 0, "invalid layout reported free space");
}

void insert_and_read_records() {
    Page page;
    uint16_t a = 0;
    uint16_t b = 0;
    uint16_t c = 0;
    const auto r1 = bytes(1, 5);
    const auto r2 = bytes(20, 37);
    const auto r3 = bytes(80, 120);

    require(page.insert(r1, a), "first insert failed");
    require(page.insert(r2, b), "second insert failed");
    require(page.insert(r3, c), "third insert failed");

    std::vector<uint8_t> out;
    require(page.read(a, out) && out == r1, "record 1 mismatch");
    require(page.read(b, out) && out == r2, "record 2 mismatch");
    require(page.read(c, out) && out == r3, "record 3 mismatch");
}

void delete_and_reuse_slot() {
    Page page;
    uint16_t a = 0;
    uint16_t b = 0;
    uint16_t c = 0;
    const auto r1 = bytes(1, 20);
    const auto r2 = bytes(2, 30);
    const auto r3 = bytes(3, 40);
    page.insert(r1, a);
    page.insert(r2, b);
    page.insert(r3, c);

    require(page.remove(b), "remove returned false");
    std::vector<uint8_t> out;
    require(!page.read(b, out), "deleted slot still readable");
    require(page.read(a, out) && out == r1, "record before delete changed");
    require(page.read(c, out) && out == r3, "record after delete changed");

    uint16_t reused = 99;
    const auto replacement = bytes(9, 10);
    require(page.insert(replacement, reused), "insert into deleted slot failed");
    require(reused == b, "slot_id was not reused");
}

void update_records() {
    Page page;
    uint16_t slot_id = 0;
    page.insert(bytes(1, 80), slot_id);

    const auto same = bytes(10, 80);
    require(page.update(slot_id, same), "same-size update failed");
    std::vector<uint8_t> out;
    require(page.read(slot_id, out) && out == same, "same-size bytes mismatch");

    const auto smaller = bytes(20, 12);
    require(page.update(slot_id, smaller), "smaller update failed");
    require(page.read(slot_id, out) && out == smaller, "smaller bytes mismatch");

    const auto larger = bytes(30, 500);
    require(page.update(slot_id, larger), "larger update failed");
    require(page.read(slot_id, out) && out == larger, "larger bytes mismatch");

    const auto huge = bytes(40, BYTE_SIZES::PAGE_SIZE);
    require(!page.update(slot_id, huge), "oversized update succeeded");
    require(page.read(slot_id, out) && out == larger, "old bytes not preserved");
}

void fill_page_until_full() {
    Page page;
    const auto record = bytes(7, 200);
    uint16_t slot_id = 0;
    int inserted = 0;
    while (page.insert(record, slot_id)) {
        ++inserted;
    }
    require(inserted > 0, "no records inserted");
    require(!page.insert(bytes(1, BYTE_SIZES::PAGE_SIZE), slot_id), "oversized insert succeeded");
}

}  // namespace

void add_page_tests(std::vector<TestCase>& tests) {
    tests.push_back({"page init", initialize_empty_page});
    tests.push_back({"slot layout", slot_layout_length_first});
    tests.push_back({"insert fast path", insert_uses_contiguous_space_first});
    tests.push_back({"bad slot count", invalid_slot_count_rejected});
    tests.push_back({"page insert/read", insert_and_read_records});
    tests.push_back({"page delete/reuse", delete_and_reuse_slot});
    tests.push_back({"page update", update_records});
    tests.push_back({"page full", fill_page_until_full});
}

int main() {
    std::vector<TestCase> tests;
    add_page_tests(tests);
    return run_tests(tests);
}
