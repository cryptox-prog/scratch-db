#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/constants.hpp"

struct PageHeader {
    uint16_t slot_count;
    uint16_t free_space_end;
    uint64_t page_lsn;
};

struct Slot {
    uint16_t size;
    uint16_t offset;
};

class Page {
public:
    Page();

    void reset();

    bool insert(const std::vector<uint8_t>& record, uint16_t& slot_id);
    bool remove(uint16_t slot_id);
    bool update(uint16_t slot_id, const std::vector<uint8_t>& record);
    bool read(uint16_t slot_id, std::vector<uint8_t>& record) const;

    uint16_t contiguous_free_space() const;
    uint16_t compact_and_get_free_space();
    void compact();

    const uint8_t* data() const;
    uint8_t* data();

    uint16_t slot_count() const;
    uint64_t page_lsn() const;
    void set_page_lsn(uint64_t page_lsn);

private:
    PageHeader header() const;
    void write_header(const PageHeader& header);
    Slot slot(uint16_t slot_id) const;
    void write_slot(uint16_t slot_id, const Slot& slot);
    uint16_t free_space_start() const;
    bool has_valid_layout() const;

    std::array<uint8_t, BYTE_SIZES::PAGE_SIZE> data_;
};
