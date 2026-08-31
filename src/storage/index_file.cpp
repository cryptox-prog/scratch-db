#include "storage/index_file.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

#include "common/constants.hpp"

namespace {
    constexpr uint32_t INDEX_MAGIC = 0x58444953;
    constexpr uint16_t INDEX_VERSION = 2;
    constexpr uint8_t PAGE_TYPE_HEADER = 1;
    constexpr uint8_t PAGE_TYPE_LEAF = 2;
    constexpr uint8_t PAGE_TYPE_INTERNAL = 3;
    constexpr uint32_t NO_PAGE = UINT32_MAX;
    constexpr uint16_t LEAF_HEADER_SIZE = 12;
    constexpr uint16_t ENTRY_HEADER_SIZE = 8;
    constexpr uint16_t INTERNAL_HEADER_SIZE = 7;
    constexpr uint16_t INTERNAL_CHILD_HEADER_SIZE = 6;

    struct PageSummary {
        uint32_t page_id = 0;
        std::vector<uint8_t> first_key;
    };

    void write_uint16(std::vector<uint8_t>& data, uint16_t offset, uint16_t value) {
        data[offset] = static_cast<uint8_t>(value & 0xff);
        data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    }

    void write_uint32(std::vector<uint8_t>& data, uint16_t offset, uint32_t value) {
        data[offset] = static_cast<uint8_t>(value & 0xff);
        data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
        data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
        data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
    }

    uint16_t read_uint16(const std::vector<uint8_t>& data, uint16_t offset) {
        return static_cast<uint16_t>(data[offset]) |
               static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
    }

    uint32_t read_uint32(const std::vector<uint8_t>& data, uint16_t offset) {
        return static_cast<uint32_t>(data[offset]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
    }

    bool full_read(int fd, uint32_t page_id, std::vector<uint8_t>& data) {
        uint8_t* out = data.data();
        std::size_t remaining = data.size();
        const off_t base = static_cast<off_t>(page_id) * BYTE_SIZES::PAGE_SIZE;
        while (remaining > 0) {
            const off_t offset = base + static_cast<off_t>(data.size() - remaining);
            const ssize_t read_count = ::pread(fd, out, remaining, offset);
            if (read_count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (read_count == 0) {
                std::fill(out, out + remaining, 0);
                return true;
            }
            out += read_count;
            remaining -= static_cast<std::size_t>(read_count);
        }
        return true;
    }

    bool full_write(int fd, uint32_t page_id, const std::vector<uint8_t>& data) {
        const uint8_t* input = data.data();
        std::size_t remaining = data.size();
        const off_t base = static_cast<off_t>(page_id) * BYTE_SIZES::PAGE_SIZE;
        while (remaining > 0) {
            const off_t offset = base + static_cast<off_t>(data.size() - remaining);
            const ssize_t write_count = ::pwrite(fd, input, remaining, offset);
            if (write_count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            input += write_count;
            remaining -= static_cast<std::size_t>(write_count);
        }
        return true;
    }

}

IndexFile::IndexFile(const std::filesystem::path& path) : IndexFile(path, nullptr, 0) {}

IndexFile::IndexFile(const std::filesystem::path& path, WalManager* wal_manager, uint64_t transaction_id)
    : path_(path), wal_manager_(wal_manager), transaction_id_(transaction_id) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("could not open index file");
    }
    if (!load()) {
        throw std::runtime_error("could not load index file");
    }
}

IndexFile::~IndexFile() {
    if (fd_ >= 0) {
        flush();
        ::close(fd_);
    }
}

bool IndexFile::insert(const std::vector<uint8_t>& key, RecordId record_id, bool unique) {
    Entry entry{key, record_id};
    if (!IndexFile::entry_fits_empty_leaf(entry)) {
        return false;
    }

    auto position = std::lower_bound(entries_.begin(), entries_.end(), entry, entry_less);
    if (unique) {
        auto duplicate = std::lower_bound(entries_.begin(), entries_.end(), Entry{key, RecordId{0, 0}}, entry_less);
        if (duplicate != entries_.end() && duplicate->key == key) {
            return false;
        }
    }
    if (position != entries_.end() && same_entry(*position, entry)) {
        return true;
    }

    entries_.insert(position, std::move(entry));
    return flush();
}

bool IndexFile::remove(const std::vector<uint8_t>& key, RecordId record_id) {
    Entry entry{key, record_id};
    auto position = std::lower_bound(entries_.begin(), entries_.end(), entry, entry_less);
    if (position == entries_.end() || !same_entry(*position, entry)) {
        return true;
    }
    entries_.erase(position);
    return flush();
}

bool IndexFile::reset() {
    entries_.clear();
    page_count_ = 2;
    root_page_id_ = 1;
    tree_height_ = 1;
    return flush();
}

std::vector<RecordId> IndexFile::find(const std::vector<uint8_t>& key) const {
    return find_from_pages(key);
}

std::vector<RecordId> IndexFile::find_range(
    const std::optional<std::vector<uint8_t>>& lower_key,
    bool include_lower,
    const std::optional<std::vector<uint8_t>>& upper_key,
    bool include_upper
) const {
    return find_range_from_pages(lower_key, include_lower, upper_key, include_upper);
}

uint32_t IndexFile::page_count() const {
    return page_count_;
}

uint16_t IndexFile::tree_height() const {
    return tree_height_;
}

bool IndexFile::flush() {
    return write_pages();
}

bool IndexFile::sync() {
    return fd_ >= 0 && ::fsync(fd_) == 0;
}

std::vector<RecordId> IndexFile::find_from_pages(const std::vector<uint8_t>& key) const {
    std::vector<RecordId> result;
    uint32_t page_id = find_leaf_page(key);
    while (page_id != NO_PAGE) {
        std::vector<uint8_t> page(BYTE_SIZES::PAGE_SIZE, 0);
        if (!full_read(fd_, page_id, page) || read_uint32(page, 0) != INDEX_MAGIC || page[4] != PAGE_TYPE_LEAF) {
            return {};
        }

        const uint32_t next_page_id = read_uint32(page, 5);
        const uint16_t entry_count = read_uint16(page, 9);
        uint16_t offset = LEAF_HEADER_SIZE;
        bool maybe_next_leaf = false;
        for (uint16_t i = 0; i < entry_count; ++i) {
            if (offset + ENTRY_HEADER_SIZE > BYTE_SIZES::PAGE_SIZE) {
                return {};
            }
            const uint16_t key_size = read_uint16(page, offset);
            RecordId record_id{read_uint32(page, static_cast<uint16_t>(offset + 2)), read_uint16(page, static_cast<uint16_t>(offset + 6))};
            offset = static_cast<uint16_t>(offset + ENTRY_HEADER_SIZE);
            if (offset + key_size > BYTE_SIZES::PAGE_SIZE) {
                return {};
            }
            std::vector<uint8_t> entry_key(page.begin() + offset, page.begin() + offset + key_size);
            if (entry_key == key) {
                result.push_back(record_id);
                maybe_next_leaf = true;
            } else if (entry_key > key) {
                return result;
            }
            offset = static_cast<uint16_t>(offset + key_size);
        }
        if (!maybe_next_leaf) {
            return result;
        }
        page_id = next_page_id;
    }
    return result;
}

std::vector<RecordId> IndexFile::find_range_from_pages(
    const std::optional<std::vector<uint8_t>>& lower_key,
    bool include_lower,
    const std::optional<std::vector<uint8_t>>& upper_key,
    bool include_upper
) const {
    std::vector<RecordId> result;
    uint32_t page_id = lower_key.has_value() ? find_leaf_page(*lower_key) : 1;
    while (page_id != NO_PAGE) {
        std::vector<uint8_t> page(BYTE_SIZES::PAGE_SIZE, 0);
        if (!full_read(fd_, page_id, page) || read_uint32(page, 0) != INDEX_MAGIC || page[4] != PAGE_TYPE_LEAF) {
            return {};
        }

        const uint32_t next_page_id = read_uint32(page, 5);
        const uint16_t entry_count = read_uint16(page, 9);
        uint16_t offset = LEAF_HEADER_SIZE;
        for (uint16_t i = 0; i < entry_count; ++i) {
            if (offset + ENTRY_HEADER_SIZE > BYTE_SIZES::PAGE_SIZE) {
                return {};
            }
            const uint16_t key_size = read_uint16(page, offset);
            RecordId record_id{read_uint32(page, static_cast<uint16_t>(offset + 2)), read_uint16(page, static_cast<uint16_t>(offset + 6))};
            offset = static_cast<uint16_t>(offset + ENTRY_HEADER_SIZE);
            if (offset + key_size > BYTE_SIZES::PAGE_SIZE) {
                return {};
            }
            std::vector<uint8_t> entry_key(page.begin() + offset, page.begin() + offset + key_size);
            offset = static_cast<uint16_t>(offset + key_size);

            if (lower_key.has_value()) {
                if (entry_key < *lower_key || (!include_lower && entry_key == *lower_key)) {
                    continue;
                }
            }
            if (upper_key.has_value()) {
                if (entry_key > *upper_key || (!include_upper && entry_key == *upper_key)) {
                    return result;
                }
            }
            result.push_back(record_id);
        }
        page_id = next_page_id;
    }
    return result;
}

uint32_t IndexFile::find_leaf_page(const std::vector<uint8_t>& key) const {
    uint32_t page_id = root_page_id_;
    while (page_id != NO_PAGE) {
        if (page_id >= page_count_) {
            return NO_PAGE;
        }
        std::vector<uint8_t> page(BYTE_SIZES::PAGE_SIZE, 0);
        if (!full_read(fd_, page_id, page) || read_uint32(page, 0) != INDEX_MAGIC) {
            return NO_PAGE;
        }
        if (page[4] == PAGE_TYPE_LEAF) {
            return page_id;
        }
        if (page[4] != PAGE_TYPE_INTERNAL) {
            return NO_PAGE;
        }

        const uint16_t child_count = read_uint16(page, 5);
        if (child_count == 0) {
            return NO_PAGE;
        }
        uint16_t offset = INTERNAL_HEADER_SIZE;
        uint32_t selected_child = NO_PAGE;
        for (uint16_t i = 0; i < child_count; ++i) {
            if (offset + INTERNAL_CHILD_HEADER_SIZE > BYTE_SIZES::PAGE_SIZE) {
                return NO_PAGE;
            }
            const uint32_t child_page_id = read_uint32(page, offset);
            const uint16_t key_size = read_uint16(page, static_cast<uint16_t>(offset + 4));
            offset = static_cast<uint16_t>(offset + INTERNAL_CHILD_HEADER_SIZE);
            if (offset + key_size > BYTE_SIZES::PAGE_SIZE) {
                return NO_PAGE;
            }
            std::vector<uint8_t> separator_key(page.begin() + offset, page.begin() + offset + key_size);
            if (i == 0 || separator_key <= key) {
                selected_child = child_page_id;
            } else {
                break;
            }
            offset = static_cast<uint16_t>(offset + key_size);
        }
        page_id = selected_child;
    }
    return NO_PAGE;
}

bool IndexFile::load() {
    std::vector<uint8_t> header(BYTE_SIZES::PAGE_SIZE, 0);
    if (!full_read(fd_, 0, header)) {
        return false;
    }
    if (read_uint32(header, 0) == 0) {
        page_count_ = 2;
        return write_pages();
    }
    if (read_uint32(header, 0) != INDEX_MAGIC || header[4] != PAGE_TYPE_HEADER) {
        return false;
    }
    if (read_uint16(header, 5) != INDEX_VERSION) {
        return false;
    }

    root_page_id_ = read_uint32(header, 7);
    page_count_ = read_uint32(header, 11);
    tree_height_ = read_uint16(header, 15);
    if (root_page_id_ == NO_PAGE || page_count_ < 2 || tree_height_ == 0) {
        return false;
    }

    entries_.clear();
    uint32_t page_id = 1;
    while (page_id != NO_PAGE) {
        if (page_id >= page_count_) {
            return false;
        }
        std::vector<uint8_t> page(BYTE_SIZES::PAGE_SIZE, 0);
        if (!full_read(fd_, page_id, page)) {
            return false;
        }
        if (read_uint32(page, 0) != INDEX_MAGIC) {
            return false;
        }
        if (page[4] != PAGE_TYPE_LEAF) {
            ++page_id;
            continue;
        }
        const uint32_t next_page_id = read_uint32(page, 5);
        const uint16_t entry_count = read_uint16(page, 9);
        uint16_t offset = LEAF_HEADER_SIZE;
        for (uint16_t i = 0; i < entry_count; ++i) {
            if (offset + ENTRY_HEADER_SIZE > BYTE_SIZES::PAGE_SIZE) {
                return false;
            }
            const uint16_t key_size = read_uint16(page, offset);
            RecordId record_id{read_uint32(page, static_cast<uint16_t>(offset + 2)), read_uint16(page, static_cast<uint16_t>(offset + 6))};
            offset = static_cast<uint16_t>(offset + ENTRY_HEADER_SIZE);
            if (offset + key_size > BYTE_SIZES::PAGE_SIZE) {
                return false;
            }
            entries_.push_back(Entry{std::vector<uint8_t>(page.begin() + offset, page.begin() + offset + key_size), record_id});
            offset = static_cast<uint16_t>(offset + key_size);
        }
        page_id = next_page_id;
    }

    std::sort(entries_.begin(), entries_.end(), entry_less);
    return true;
}

bool IndexFile::write_pages() {
    const std::vector<std::vector<Entry>> leaves = split_leaf_entries(entries_);
    if (leaves.empty()) {
        return false;
    }

    std::vector<PageSummary> current_level;
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        std::vector<uint8_t> page(BYTE_SIZES::PAGE_SIZE, 0);
        const uint32_t page_id = static_cast<uint32_t>(i + 1);
        const uint32_t next_page_id = i + 1 >= leaves.size() ? NO_PAGE : static_cast<uint32_t>(i + 2);
        write_uint32(page, 0, INDEX_MAGIC);
        page[4] = PAGE_TYPE_LEAF;
        write_uint32(page, 5, next_page_id);
        write_uint16(page, 9, static_cast<uint16_t>(leaves[i].size()));
        uint16_t offset = LEAF_HEADER_SIZE;
        for (const Entry& entry : leaves[i]) {
            if (entry.key.size() > UINT16_MAX || offset + ENTRY_HEADER_SIZE + entry.key.size() > BYTE_SIZES::PAGE_SIZE) {
                return false;
            }
            write_uint16(page, offset, static_cast<uint16_t>(entry.key.size()));
            write_uint32(page, static_cast<uint16_t>(offset + 2), entry.record_id.page_id);
            write_uint16(page, static_cast<uint16_t>(offset + 6), entry.record_id.slot_id);
            offset = static_cast<uint16_t>(offset + ENTRY_HEADER_SIZE);
            std::copy(entry.key.begin(), entry.key.end(), page.begin() + offset);
            offset = static_cast<uint16_t>(offset + entry.key.size());
        }
        if (!write_logged_page(page_id, page)) {
            return false;
        }
        PageSummary summary;
        summary.page_id = page_id;
        if (!leaves[i].empty()) {
            summary.first_key = leaves[i].front().key;
        }
        current_level.push_back(summary);
    }

    uint32_t next_page_id = static_cast<uint32_t>(leaves.size() + 1);
    tree_height_ = 1;
    while (current_level.size() > 1) {
        std::vector<PageSummary> next_level;
        std::vector<PageSummary> children;
        auto flush_internal = [&]() {
            if (children.empty()) {
                return true;
            }
            std::vector<uint8_t> page(BYTE_SIZES::PAGE_SIZE, 0);
            write_uint32(page, 0, INDEX_MAGIC);
            page[4] = PAGE_TYPE_INTERNAL;
            write_uint16(page, 5, static_cast<uint16_t>(children.size()));
            uint16_t offset = INTERNAL_HEADER_SIZE;
            for (const PageSummary& child : children) {
                if (child.first_key.size() > UINT16_MAX ||
                    offset + INTERNAL_CHILD_HEADER_SIZE + child.first_key.size() > BYTE_SIZES::PAGE_SIZE) {
                    return false;
                }
                write_uint32(page, offset, child.page_id);
                write_uint16(page, static_cast<uint16_t>(offset + 4), static_cast<uint16_t>(child.first_key.size()));
                offset = static_cast<uint16_t>(offset + INTERNAL_CHILD_HEADER_SIZE);
                std::copy(child.first_key.begin(), child.first_key.end(), page.begin() + offset);
                offset = static_cast<uint16_t>(offset + child.first_key.size());
            }
            if (!write_logged_page(next_page_id, page)) {
                return false;
            }
            next_level.push_back(PageSummary{next_page_id, children.front().first_key});
            ++next_page_id;
            children.clear();
            return true;
        };

        for (const PageSummary& child : current_level) {
            children.push_back(child);
            uint32_t size = INTERNAL_HEADER_SIZE;
            for (const PageSummary& packed : children) {
                size += INTERNAL_CHILD_HEADER_SIZE + static_cast<uint32_t>(packed.first_key.size());
            }
            if (size > BYTE_SIZES::PAGE_SIZE) {
                PageSummary overflow = children.back();
                children.pop_back();
                if (!flush_internal()) {
                    return false;
                }
                children.push_back(overflow);
            }
        }
        if (!flush_internal()) {
            return false;
        }
        current_level = next_level;
        ++tree_height_;
    }

    root_page_id_ = current_level.front().page_id;
    page_count_ = next_page_id;

    std::vector<uint8_t> header(BYTE_SIZES::PAGE_SIZE, 0);
    write_uint32(header, 0, INDEX_MAGIC);
    header[4] = PAGE_TYPE_HEADER;
    write_uint16(header, 5, INDEX_VERSION);
    write_uint32(header, 7, root_page_id_);
    write_uint32(header, 11, page_count_);
    write_uint16(header, 15, tree_height_);
    if (!write_logged_page(0, header)) {
        return false;
    }

    return true;
}

bool IndexFile::write_logged_page(uint32_t page_id, const std::vector<uint8_t>& page) {
    if (page.size() != BYTE_SIZES::PAGE_SIZE) {
        return false;
    }
    if (wal_manager_ != nullptr) {
        std::vector<uint8_t> before(BYTE_SIZES::PAGE_SIZE, 0);
        if (!full_read(fd_, page_id, before)) {
            return false;
        }
        const uint64_t lsn = wal_manager_->log_page_update_raw(transaction_id_, path_.string(), page_id, before, page);
        if (lsn == 0 || !wal_manager_->flush_through(lsn)) {
            return false;
        }
    }
    return full_write(fd_, page_id, page);
}

uint16_t IndexFile::encoded_size(const std::vector<Entry>& entries) {
    uint32_t size = LEAF_HEADER_SIZE;
    for (const Entry& entry : entries) {
        size += ENTRY_HEADER_SIZE + static_cast<uint32_t>(entry.key.size());
    }
    return size > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(size);
}

std::vector<std::vector<IndexFile::Entry>> IndexFile::split_leaf_entries(const std::vector<Entry>& entries) {
    std::vector<std::vector<Entry>> leaves(1);
    for (const Entry& entry : entries) {
        if (!IndexFile::entry_fits_empty_leaf(entry)) {
            return {};
        }
        std::vector<Entry>& leaf = leaves.back();
        leaf.push_back(entry);
        if (encoded_size(leaf) > BYTE_SIZES::PAGE_SIZE) {
            leaf.pop_back();
            leaves.push_back({entry});
        }
    }
    return leaves;
}

bool IndexFile::entry_fits_empty_leaf(const Entry& entry) {
    return entry.key.size() <= UINT16_MAX &&
           LEAF_HEADER_SIZE + ENTRY_HEADER_SIZE + entry.key.size() <= BYTE_SIZES::PAGE_SIZE;
}

bool IndexFile::entry_less(const Entry& left, const Entry& right) {
    if (left.key != right.key) {
        return left.key < right.key;
    }
    if (left.record_id.page_id != right.record_id.page_id) {
        return left.record_id.page_id < right.record_id.page_id;
    }
    return left.record_id.slot_id < right.record_id.slot_id;
}

bool IndexFile::same_entry(const Entry& left, const Entry& right) {
    return left.key == right.key &&
           left.record_id.page_id == right.record_id.page_id &&
           left.record_id.slot_id == right.record_id.slot_id;
}
