#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "common/types.hpp"
#include "storage/wal_manager.hpp"

class IndexFile {
public:
    explicit IndexFile(const std::filesystem::path& path);
    IndexFile(const std::filesystem::path& path, WalManager* wal_manager, uint64_t transaction_id);
    ~IndexFile();

    bool insert(const std::vector<uint8_t>& key, RecordId record_id, bool unique);
    bool remove(const std::vector<uint8_t>& key, RecordId record_id);
    bool reset();
    std::vector<RecordId> find(const std::vector<uint8_t>& key) const;
    std::vector<RecordId> find_range(
        const std::optional<std::vector<uint8_t>>& lower_key,
        bool include_lower,
        const std::optional<std::vector<uint8_t>>& upper_key,
        bool include_upper
    ) const;
    uint32_t page_count() const;
    uint16_t tree_height() const;
    bool flush();
    bool sync();

private:
    struct Entry {
        std::vector<uint8_t> key;
        RecordId record_id;
    };

    bool load();
    bool write_pages();
    bool write_logged_page(uint32_t page_id, const std::vector<uint8_t>& page);
    std::vector<RecordId> find_from_pages(const std::vector<uint8_t>& key) const;
    std::vector<RecordId> find_range_from_pages(
        const std::optional<std::vector<uint8_t>>& lower_key,
        bool include_lower,
        const std::optional<std::vector<uint8_t>>& upper_key,
        bool include_upper
    ) const;
    uint32_t find_leaf_page(const std::vector<uint8_t>& key) const;
    static uint16_t encoded_size(const std::vector<Entry>& entries);
    static std::vector<std::vector<Entry>> split_leaf_entries(const std::vector<Entry>& entries);
    static bool entry_fits_empty_leaf(const Entry& entry);
    static bool entry_less(const Entry& left, const Entry& right);
    static bool same_entry(const Entry& left, const Entry& right);

    std::filesystem::path path_;
    int fd_ = -1;
    WalManager* wal_manager_ = nullptr;
    uint64_t transaction_id_ = 0;
    std::vector<Entry> entries_;
    uint32_t page_count_ = 2;
    uint32_t root_page_id_ = 1;
    uint16_t tree_height_ = 1;
};
