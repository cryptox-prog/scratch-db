#include "storage/page.hpp"

#include <cstring>
#include <utility>

namespace {
    /// @brief Read 2 bytes starting from pointer
    /// @param ptr pointer to lower byte
    /// @return 2 byte unsigned number read in little endian format
    uint16_t read_uint16(const uint8_t* ptr) {
        // since little endian lower mem makes LSB
        // read lower byte, read and shift the next byte left by 1 byte and OR them to combine into one 16 bit number
        return static_cast<uint16_t>(ptr[0]) | static_cast<uint16_t>(static_cast<uint16_t>(ptr[1]) << 8);
    }

    /// @brief Write 2 bytes starting from pointer
    /// @param ptr pointer to lower byte
    /// @param value 2 byte unsigned value to write
    void write_uint16(uint8_t* ptr, uint16_t value) {
        // since little endian lower mem makes LSB
        // write the lower byte first (AND with 0xFF to make upper byte 0)
        // shift upper byte to lower byte then AND with 0xFF to zero now useless upper byte
        ptr[0] = static_cast<uint8_t>(value & 0xff);
        ptr[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    }

    uint64_t read_uint64(const uint8_t* ptr) {
        uint64_t value = 0;
        for (uint16_t i = 0; i < BYTE_SIZES::UINT64; ++i) {
            value |= static_cast<uint64_t>(ptr[i]) << (i * 8);
        }
        return value;
    }

    void write_uint64(uint8_t* ptr, uint64_t value) {
        for (uint16_t i = 0; i < BYTE_SIZES::UINT64; ++i) {
            ptr[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
        }
    }

    /// @brief Find where a given slot is in a page
    /// @param slot_id The index of the slot in the page
    /// @return The offset to shift by from page start to reach the slot start
    uint16_t slot_offset(uint16_t slot_id) {
        // Slot IDs are array indexes: header first, then slot_id * BYTE_SIZES::SLOT_SIZE.
        return static_cast<uint16_t>(BYTE_SIZES::PAGE_HEADER_SIZE + slot_id * BYTE_SIZES::SLOT_SIZE);
    }
}  // namespace

/// @brief Creates a blank page and writes the header
/// @note Internally calls the reset method
Page::Page() {
    reset();
}

/// @brief Cleans the page by filling with zeroes and writes the blank page header
/// @note Blank Space Header => 0 slot count and free space end at end of page
void Page::reset() {
    data_.fill(0);
    write_header(PageHeader{0, static_cast<uint16_t>(BYTE_SIZES::PAGE_SIZE), 0});
}

/// @brief Insert a new record into the page
/// @param record The new record to insert
/// @param slot_id The index of the slot the record is linked too (this is to receive information not to pass your wanted slot id)
/// @return False if invalid record or page or insufficient space in page True otherwise
bool Page::insert(const std::vector<uint8_t>& record, uint16_t& slot_id) {
    if (record.empty() || !has_valid_layout()) {
        return false;
    }

    PageHeader h = header();
    bool reuse_slot = false;
    uint16_t target_slot = h.slot_count;
    for (uint16_t i = 0; i < h.slot_count; ++i) {
        if (slot(i).size == 0) {
            target_slot = i;
            reuse_slot = true;
            break;
        }
    }

    const uint16_t extra_slot_bytes = reuse_slot ? 0 : BYTE_SIZES::SLOT_SIZE;
    // if we cant reuse any slot then make sure the slot directory still fits in the page
    if (!reuse_slot && h.slot_count == BYTE_SIZES::MAX_SLOTS) {
        return false;
    }

    const std::size_t needed_bytes = record.size() + extra_slot_bytes;
    if (static_cast<std::size_t>(contiguous_free_space()) < needed_bytes &&
        static_cast<std::size_t>(compact_and_get_free_space()) < needed_bytes) {
        return false;
    }
    h = header();

    // Records grow backward from end of page towards the header
    const uint16_t new_offset = static_cast<uint16_t>(h.free_space_end - record.size());
    std::memcpy(data_.data() + new_offset, record.data(), record.size());
    write_slot(target_slot, Slot{static_cast<uint16_t>(record.size()), new_offset});

    h.free_space_end = new_offset;
    if (!reuse_slot) {
        ++h.slot_count;
    }
    write_header(h);

    slot_id = target_slot;
    return true;
}


/// @brief Perform the deletion of a record by zeroing the slot
/// @param slot_id The slot index in the page
/// @return False id invalid slot and True otherwise
bool Page::remove(uint16_t slot_id) {
    if (!has_valid_layout()) {
        return false;
    }

    if (slot_id >= header().slot_count || slot(slot_id).size == 0) {
        return false;
    }
    write_slot(slot_id, Slot{0, 0});
    return true;
}

/// @brief Update the record in a given slot
/// @param slot_id The slot index in the page
/// @param record The updated record
/// @return False if impossible to perform update within page True if update successful
bool Page::update(uint16_t slot_id, const std::vector<uint8_t>& record) {
    if (record.empty() || !has_valid_layout() || slot_id >= header().slot_count) {
        return false;
    }

    const Slot old_slot = slot(slot_id);
    if (old_slot.size == 0) {
        return false;
    }

    if (record.size() == old_slot.size) {
        std::memcpy(data_.data() + old_slot.offset, record.data(), record.size());
        return true;
    }

    // Update record in a copy of the page
    Page trial = *this;
    trial.write_slot(slot_id, Slot{0, 0}); // so that the record of this slot is considered for compaction
    // For update first try if already available space is enough for record otherwise compact and try again
    if (static_cast<std::size_t>(trial.contiguous_free_space()) < record.size() &&
        static_cast<std::size_t>(trial.compact_and_get_free_space()) < record.size()) {
        return false;
    }

    PageHeader h = trial.header();
    const uint16_t new_offset = static_cast<uint16_t>(h.free_space_end - record.size());
    std::memcpy(trial.data_.data() + new_offset, record.data(), record.size());
    trial.write_slot(slot_id, Slot{static_cast<uint16_t>(record.size()), new_offset});
    h.free_space_end = new_offset;
    trial.write_header(h);

    *this = trial;
    return true;
}


/// @brief Read the requested record
/// @param slot_id The slot index in the page
/// @param record The vector to store the record in
/// @return False if slot read failed true otherwise
bool Page::read(uint16_t slot_id, std::vector<uint8_t>& record) const {
    if (!has_valid_layout() || slot_id >= header().slot_count) {
        return false;
    }

    const Slot s = slot(slot_id);
    if (s.size == 0 || s.offset + s.size > BYTE_SIZES::PAGE_SIZE) {
        return false;
    }

    record.assign(data_.begin() + s.offset, data_.begin() + s.offset + s.size);
    return true;
}

/// @brief Get the continuos free space available
/// @return The free space before compaction
uint16_t Page::contiguous_free_space() const {
    if (!has_valid_layout()) {
        return 0;
    }

    const PageHeader h = header();
    const uint16_t start = free_space_start();
    if (h.free_space_end < start) {
        return 0;
    }
    return static_cast<uint16_t>(h.free_space_end - start);
}

/// @brief Compact the page and get the free space
/// @return The free space available
uint16_t Page::compact_and_get_free_space() {
    compact();
    return contiguous_free_space();
}

/// @brief Remove any holes in the page
void Page::compact() {
    if (!has_valid_layout()) {
        return;
    }

    PageHeader h = header();

    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> records;
    records.reserve(h.slot_count);

    // Read all the non zeroed slots records into array to rewrite them starting from end
    uint16_t expected_end = static_cast<uint16_t>(BYTE_SIZES::PAGE_SIZE);
    bool already_compact = true;
    for (uint16_t i = 0; i < h.slot_count; ++i) {
        const Slot s = slot(i);
        if (s.size == 0) {
            continue;
        }
        
        // after each valid slot subtract the slots records size if it is at the expected end then it compacted
        expected_end = static_cast<uint16_t>(expected_end - s.size);
        if (s.offset != expected_end) {
            already_compact = false;
        }

        records.push_back({
            i,
            std::vector<uint8_t>(data_.begin() + s.offset, data_.begin() + s.offset + s.size)
        });
    }

    if (already_compact && h.free_space_end == expected_end) {
        return;
    }

    // Rewrite without gaps
    uint16_t end = static_cast<uint16_t>(BYTE_SIZES::PAGE_SIZE);
    for (const auto& record : records) {
        end = static_cast<uint16_t>(end - record.second.size());
        std::memcpy(data_.data() + end, record.second.data(), record.second.size());
        write_slot(record.first, Slot{static_cast<uint16_t>(record.second.size()), end});
    }

    h.free_space_end = end;
    write_header(h);
}

/// @brief Get the page bytes in read only manner
/// @return Raw pointer to the first byte of data
const uint8_t* Page::data() const {
    return data_.data();
}

/// @brief Get the page bytes in read write manner
/// @return Raw pointer to the first byte of data
uint8_t* Page::data() {
    return data_.data();
}

/// @brief Get the number of slots in the page
/// @return The slot count
uint16_t Page::slot_count() const {
    return header().slot_count;
}

uint64_t Page::page_lsn() const {
    return header().page_lsn;
}

void Page::set_page_lsn(uint64_t page_lsn) {
    PageHeader h = header();
    h.page_lsn = page_lsn;
    write_header(h);
}

/// @brief Read the Page header
/// @return The Slot Count and Free Space End shift
PageHeader Page::header() const {
    return PageHeader{
        read_uint16(data_.data()),                              // The slot count
        read_uint16(data_.data() + BYTE_SIZES::SLOT_COUNT),     // The free space end
        read_uint64(data_.data() + BYTE_SIZES::PAGE_LSN),       // Last log record flushed before this page
    };
}

/// @brief Write the header data into th page
/// @param header The slot count and the free space end shift
void Page::write_header(const PageHeader& header) {
    write_uint16(data_.data(), header.slot_count);
    write_uint16(data_.data() + BYTE_SIZES::SLOT_COUNT, header.free_space_end);
    write_uint64(data_.data() + BYTE_SIZES::PAGE_LSN, header.page_lsn);
}

/// @brief Get slot data
/// @param slot_id The slot index in the given page
/// @return The record size followed by the record offset
Slot Page::slot(uint16_t slot_id) const {
    const uint16_t pos = slot_offset(slot_id);
    return Slot{
        read_uint16(data_.data() + pos),
        read_uint16(data_.data() + pos + BYTE_SIZES::LENGTH),
    };
}

/// @brief Write the slot data into page
/// @param slot_id The slot index in the given page
/// @param slot The record size followed by the record offset
void Page::write_slot(uint16_t slot_id, const Slot& slot) {
    const uint16_t pos = slot_offset(slot_id);
    write_uint16(data_.data() + pos, slot.size);
    write_uint16(data_.data() + pos + BYTE_SIZES::LENGTH, slot.offset);
}

/// @brief Finds the start of free space using header size and slot count
/// @return offset to start of free space
uint16_t Page::free_space_start() const {
    return static_cast<uint16_t>(
        BYTE_SIZES::PAGE_HEADER_SIZE + header().slot_count * BYTE_SIZES::SLOT_SIZE
    );
}

/// @brief Check if the start of free space falls befor its end and the end is the size of the page
/// @return true id the layout is valid
bool Page::has_valid_layout() const {
    const PageHeader h = header();

    // use 32 bit so in case invalid slot count is too large we capture it
    const uint32_t free_space_start = BYTE_SIZES::PAGE_HEADER_SIZE + static_cast<uint32_t>(h.slot_count) * BYTE_SIZES::SLOT_SIZE;
    return h.slot_count <= BYTE_SIZES::MAX_SLOTS &&
           free_space_start <= h.free_space_end &&
           h.free_space_end <= BYTE_SIZES::PAGE_SIZE;
}
