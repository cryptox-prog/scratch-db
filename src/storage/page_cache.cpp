#include "storage/page_cache.hpp"

#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
    bool full_read(int fd, uint8_t* buffer, std::size_t size, off_t offset) {
        std::size_t total = 0;
        while (total < size) {
            const ssize_t n = pread(fd, buffer + total, size - total, offset + static_cast<off_t>(total));
            if (n == 0) {
                return false;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            total += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool full_write(int fd, const uint8_t* buffer, std::size_t size, off_t offset) {
        std::size_t total = 0;
        while (total < size) {
            const ssize_t n = pwrite(fd, buffer + total, size - total, offset + static_cast<off_t>(total));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            total += static_cast<std::size_t>(n);
        }
        return true;
    }

    const char* path_c_str(const std::filesystem::path& path) {
        return path.c_str();
    }
}  // namespace

bool CachedPageKey::operator==(const CachedPageKey& other) const {
    return table_file == other.table_file && page_id == other.page_id;
}

std::size_t CachedPageKeyHash::operator()(const CachedPageKey& key) const {
    const std::size_t path_hash = std::hash<std::string>{}(key.table_file.string());
    const std::size_t page_hash = std::hash<uint32_t>{}(key.page_id);
    return path_hash ^ (page_hash + 0x9e3779b9 + (path_hash << 6) + (path_hash >> 2));
}

PageCache::PageCache(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

PageCache::~PageCache() {
    flush_all_pages();
}

Page* PageCache::fetch_page(const std::filesystem::path& table_file, uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return fetch_page(table_file, page_id, std::nullopt, nullptr);
}

Page* PageCache::fetch_page(const std::filesystem::path& table_file, uint32_t page_id, int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    return fetch_page(table_file, page_id, std::optional<int>(fd), nullptr);
}

Page* PageCache::fetch_page(const std::filesystem::path& table_file, uint32_t page_id, std::optional<int> fd, WalManager* wal_manager) {
    const CachedPageKey key = make_key(table_file, page_id);
    auto existing = pages_.find(key);
    if (existing != pages_.end()) {
        ++hit_count_;
        ++existing->second.pin_count;
        lru_.splice(lru_.begin(), lru_, existing->second.lru_position);
        existing->second.lru_position = lru_.begin();
        return &existing->second.page;
    }

    ++miss_count_;
    while (pages_.size() >= capacity_) {
        if (!evict_one_page(fd, wal_manager)) {
            return nullptr;
        }
    }

    std::optional<Page> page = read_page_from_disk(key, fd);
    if (!page.has_value()) {
        return nullptr;
    }

    lru_.push_front(key);
    CachedPageFrame frame;
    frame.key = key;
    frame.page = *page;
    frame.pin_count = 1;
    frame.lru_position = lru_.begin();
    auto inserted = pages_.emplace(key, std::move(frame));
    return &inserted.first->second.page;
}

Page* PageCache::new_page(const std::filesystem::path& table_file, uint32_t page_id, const Page& page) {
    std::lock_guard<std::mutex> lock(mutex_);
    return new_page(table_file, page_id, page, std::nullopt, nullptr);
}

Page* PageCache::new_page(const std::filesystem::path& table_file, uint32_t page_id, const Page& page, int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    return new_page(table_file, page_id, page, std::optional<int>(fd), nullptr);
}

Page* PageCache::new_page(const std::filesystem::path& table_file, uint32_t page_id, const Page& page, std::optional<int> fd, WalManager* wal_manager) {
    const CachedPageKey key = make_key(table_file, page_id);
    auto existing = pages_.find(key);
    if (existing != pages_.end()) {
        ++existing->second.pin_count;
        existing->second.page = page;
        existing->second.is_dirty = true;
        lru_.splice(lru_.begin(), lru_, existing->second.lru_position);
        existing->second.lru_position = lru_.begin();
        return &existing->second.page;
    }

    while (pages_.size() >= capacity_) {
        if (!evict_one_page(fd, wal_manager)) {
            return nullptr;
        }
    }

    lru_.push_front(key);
    CachedPageFrame frame;
    frame.key = key;
    frame.page = page;
    frame.is_dirty = true;
    frame.pin_count = 1;
    frame.lru_position = lru_.begin();
    auto inserted = pages_.emplace(key, std::move(frame));
    return &inserted.first->second.page;
}

bool PageCache::unpin_page(const std::filesystem::path& table_file, uint32_t page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(mutex_);
    const CachedPageKey key = make_key(table_file, page_id);
    auto found = pages_.find(key);
    if (found == pages_.end() || found->second.pin_count == 0) {
        return false;
    }

    --found->second.pin_count;
    found->second.is_dirty = found->second.is_dirty || is_dirty;
    return true;
}

bool PageCache::flush_page(const std::filesystem::path& table_file, uint32_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_page(table_file, page_id, std::nullopt, nullptr);
}

bool PageCache::flush_page(const std::filesystem::path& table_file, uint32_t page_id, int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_page(table_file, page_id, std::optional<int>(fd), nullptr);
}

bool PageCache::flush_page(const std::filesystem::path& table_file, uint32_t page_id, int fd, WalManager* wal_manager) {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_page(table_file, page_id, std::optional<int>(fd), wal_manager);
}

bool PageCache::flush_page(const std::filesystem::path& table_file, uint32_t page_id, std::optional<int> fd, WalManager* wal_manager) {
    const CachedPageKey key = make_key(table_file, page_id);
    auto found = pages_.find(key);
    if (found == pages_.end()) {
        return false;
    }
    if (!found->second.is_dirty) {
        return true;
    }
    if (!write_frame(found->second, fd, wal_manager)) {
        return false;
    }
    found->second.is_dirty = false;
    return true;
}

bool PageCache::flush_all_pages() {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_all_pages(std::nullopt, nullptr);
}

bool PageCache::flush_all_pages(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_all_pages(std::optional<int>(fd), nullptr);
}

bool PageCache::flush_all_pages(int fd, WalManager* wal_manager) {
    std::lock_guard<std::mutex> lock(mutex_);
    return flush_all_pages(std::optional<int>(fd), wal_manager);
}

bool PageCache::flush_all_pages(std::optional<int> fd, WalManager* wal_manager) {
    bool ok = true;
    for (auto& entry : pages_) {
        CachedPageFrame& frame = entry.second;
        if (frame.is_dirty) {
            if (write_frame(frame, fd, wal_manager)) {
                frame.is_dirty = false;
            } else {
                ok = false;
            }
        }
    }
    return ok;
}

void PageCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    pages_.clear();
    lru_.clear();
}

std::size_t PageCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pages_.size();
}

std::size_t PageCache::capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
}

std::size_t PageCache::hit_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hit_count_;
}

std::size_t PageCache::miss_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return miss_count_;
}

bool PageCache::contains_page(const std::filesystem::path& table_file, uint32_t page_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pages_.find(make_key(table_file, page_id)) != pages_.end();
}

CachedPageKey PageCache::make_key(const std::filesystem::path& table_file, uint32_t page_id) const {
    return CachedPageKey{table_file.lexically_normal(), page_id};
}

bool PageCache::evict_one_page(std::optional<int> fd, WalManager* wal_manager) {
    for (auto it = lru_.rbegin(); it != lru_.rend(); ++it) {
        auto found = pages_.find(*it);
        if (found == pages_.end() || found->second.pin_count != 0) {
            continue;
        }

        if (found->second.is_dirty && !write_frame(found->second, fd, wal_manager)) {
            return false;
        }

        lru_.erase(std::next(it).base());
        pages_.erase(found);
        return true;
    }
    return false;
}

bool PageCache::write_frame(const CachedPageFrame& frame, std::optional<int> fd, WalManager* wal_manager) {
    if (wal_manager != nullptr && !wal_manager->flush_through(frame.page.page_lsn())) {
        return false;
    }

    const int write_fd = fd.has_value() ? *fd : open(path_c_str(frame.key.table_file), O_RDWR);
    if (write_fd < 0) {
        return false;
    }

    const off_t offset = static_cast<off_t>(frame.key.page_id) * static_cast<off_t>(BYTE_SIZES::PAGE_SIZE);
    const bool ok = full_write(write_fd, frame.page.data(), BYTE_SIZES::PAGE_SIZE, offset);
    if (!fd.has_value()) {
        close(write_fd);
    }
    return ok;
}

std::optional<Page> PageCache::read_page_from_disk(const CachedPageKey& key, std::optional<int> fd) const {
    const int read_fd = fd.has_value() ? *fd : open(path_c_str(key.table_file), O_RDONLY);
    if (read_fd < 0) {
        return std::nullopt;
    }

    Page page;
    const off_t offset = static_cast<off_t>(key.page_id) * static_cast<off_t>(BYTE_SIZES::PAGE_SIZE);
    const bool ok = full_read(read_fd, page.data(), static_cast<std::size_t>(BYTE_SIZES::PAGE_SIZE), offset);
    if (!fd.has_value()) {
        close(read_fd);
    }
    if (!ok) {
        return std::nullopt;
    }
    return page;
}
