#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "storage/page.hpp"
#include "storage/wal_manager.hpp"

struct CachedPageKey {
    std::filesystem::path table_file;
    uint32_t page_id = 0;

    bool operator==(const CachedPageKey& other) const;
};

struct CachedPageKeyHash {
    std::size_t operator()(const CachedPageKey& key) const;
};

class PageCache {
public:
    explicit PageCache(std::size_t capacity = 64);
    ~PageCache();

    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

    Page* fetch_page(const std::filesystem::path& table_file, uint32_t page_id);
    Page* fetch_page(const std::filesystem::path& table_file, uint32_t page_id, int fd);
    Page* new_page(const std::filesystem::path& table_file, uint32_t page_id, const Page& page);
    Page* new_page(const std::filesystem::path& table_file, uint32_t page_id, const Page& page, int fd);
    bool unpin_page(const std::filesystem::path& table_file, uint32_t page_id, bool is_dirty);
    bool flush_page(const std::filesystem::path& table_file, uint32_t page_id);
    bool flush_page(const std::filesystem::path& table_file, uint32_t page_id, int fd);
    bool flush_page(const std::filesystem::path& table_file, uint32_t page_id, int fd, WalManager* wal_manager);
    bool flush_all_pages();
    bool flush_all_pages(int fd);
    bool flush_all_pages(int fd, WalManager* wal_manager);
    void clear();

    std::size_t size() const;
    std::size_t capacity() const;
    std::size_t hit_count() const;
    std::size_t miss_count() const;
    bool contains_page(const std::filesystem::path& table_file, uint32_t page_id) const;

private:
    struct CachedPageFrame {
        CachedPageKey key;
        Page page;
        bool is_dirty = false;
        uint32_t pin_count = 0;
        std::list<CachedPageKey>::iterator lru_position;
    };

    CachedPageKey make_key(const std::filesystem::path& table_file, uint32_t page_id) const;
    Page* fetch_page(const std::filesystem::path& table_file, uint32_t page_id, std::optional<int> fd, WalManager* wal_manager);
    Page* new_page(const std::filesystem::path& table_file, uint32_t page_id, const Page& page, std::optional<int> fd, WalManager* wal_manager);
    bool flush_page(const std::filesystem::path& table_file, uint32_t page_id, std::optional<int> fd, WalManager* wal_manager);
    bool flush_all_pages(std::optional<int> fd, WalManager* wal_manager);
    bool evict_one_page(std::optional<int> fd, WalManager* wal_manager);
    bool write_frame(const CachedPageFrame& frame, std::optional<int> fd, WalManager* wal_manager);
    std::optional<Page> read_page_from_disk(const CachedPageKey& key, std::optional<int> fd) const;

    std::size_t capacity_;
    std::list<CachedPageKey> lru_;
    std::unordered_map<CachedPageKey, CachedPageFrame, CachedPageKeyHash> pages_;
    std::size_t hit_count_ = 0;
    std::size_t miss_count_ = 0;
    mutable std::mutex mutex_;
};
