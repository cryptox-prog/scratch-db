#include "storage/table_file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
    /// @brief Write the given bytes to the file
    /// @param fd The file Descriptor to write into
    /// @param buffer The pointer to where we store the bytes to write
    /// @param size The number of bytes to write
    /// @param offset The offset in file to start reading from
    /// @return False if some error is encountered in writing to file True otherwise
    /// @note This is needed cause posix might fewer then requested bytes
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
            if (n == 0) {
                return false;
            }
            total += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// @brief Get size of file in bytes
    /// @param fd The file descriptor by POSIX
    /// @return The size of the file (-1 if failure)
    off_t file_size(int fd) {
        struct stat st {};
        if (fstat(fd, &st) != 0) {
            return -1;
        }
        return st.st_size;
    }

    /// @brief Get a const c type string from filesystem path
    /// @param path The path to convert to string
    /// @return The const pointe to the c string path
    /// @note Only needed because .c_str() give mutable string
    const char* path_c_str(const std::filesystem::path& path) {
        return path.c_str();
    }
}  // namespace

/// @brief Create or open the file and check if it is aligned with page size
/// @param path The path to the file
/// @exception If failed to open the table file
/// @exception If file size is not aligned with page size
TableFile::TableFile(const std::filesystem::path& path) : TableFile(path, nullptr) {}

TableFile::TableFile(const std::filesystem::path& path, WalManager* wal_manager) : TableFile(path, wal_manager, 0) {}

TableFile::TableFile(const std::filesystem::path& path, WalManager* wal_manager, uint64_t transaction_id)
    : path_(path), wal_manager_(wal_manager), transaction_id_(transaction_id) {
    // open file for read write if not exists create it
    // octal permission code for read write for owner
    fd_ = open(path_c_str(path_), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("could not open table file: " + path_.string());
    }

    const off_t size = file_size(fd_);
    if (size < 0 || size % static_cast<off_t>(BYTE_SIZES::PAGE_SIZE) != 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("table file size is not page aligned: " + path_.string());
    }

    page_count_ = static_cast<uint32_t>(size / static_cast<off_t>(BYTE_SIZES::PAGE_SIZE));
}

TableFile::~TableFile() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ >= 0) {
        flush();
        page_cache_.clear();
        close(fd_);
        fd_ = -1;
    }
}

/// @brief Get the number of pages in the file
/// @return The number of pages in the file
uint32_t TableFile::page_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return page_count_;
}

bool TableFile::flush() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0) {
        return false;
    }
    return page_cache_.flush_all_pages(fd_, wal_manager_);
}

bool TableFile::sync() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!flush()) {
        return false;
    }
    while (fsync(fd_) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void TableFile::discard_cache() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    page_cache_.clear();
}

/// @brief Read a given page from the file
/// @param page_id The index of the page in the file
/// @param page The object to write page data into
/// @return False if failed to read page True otherwise
bool TableFile::read_page(uint32_t page_id, Page& page) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // TODO: Add a LFU cache for pages (across tables only for session???)
    if (page_id >= page_count()) {
        return false;
    }

    Page* cached_page = page_cache_.fetch_page(path_, page_id, fd_);
    if (cached_page == nullptr) {
        return false;
    }
    page = *cached_page;
    return page_cache_.unpin_page(path_, page_id, false);
}

/// @brief Write a give page into the file
/// @param page_id The index of the page in the file
/// @param page The object in which the page data is stored
/// @return False if some error occured in write otherwise True
bool TableFile::write_page(uint32_t page_id, const Page& page) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (page_id >= page_count()) {
        return false;
    }

    Page* cached_page = page_cache_.fetch_page(path_, page_id, fd_);
    if (cached_page == nullptr) {
        return false;
    }

    Page before = *cached_page;
    *cached_page = page;
    if (!log_page_update(page_id, before, *cached_page)) {
        *cached_page = before;
        page_cache_.unpin_page(path_, page_id, false);
        return false;
    }
    return page_cache_.unpin_page(path_, page_id, true);
}

/// @brief Insert a given record into a page and write to file
/// @param record The record to insert
/// @return The page_id and slot_id to identify record uniquely in table file
/// @exception If the record is larger than page
/// @exception If append file failed
RecordId TableFile::insert_record(const std::vector<uint8_t>& record) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // TODO: Free space map
    // Current: First fit scan
    for (uint32_t page_id = 0; page_id < page_count(); ++page_id) {
        Page* page = page_cache_.fetch_page(path_, page_id, fd_);
        if (page == nullptr) {
            continue;
        }

        uint16_t slot_id = 0;
        Page before = *page;
        if (page->insert(record, slot_id)) {
            if (!log_page_update(page_id, before, *page)) {
                *page = before;
                page_cache_.unpin_page(path_, page_id, false);
                throw std::runtime_error("could not write wal record");
            }
            page_cache_.unpin_page(path_, page_id, true);
            return RecordId{page_id, slot_id};
        }
        page_cache_.unpin_page(path_, page_id, false);
    }

    Page page;
    uint16_t slot_id = 0;
    if (!page.insert(record, slot_id)) {
        throw std::runtime_error("record is too large for a page");
    }

    const uint32_t new_page_id = page_count();
    if (!append_page(page)) {
        throw std::runtime_error("could not append page");
    }

    return RecordId{new_page_id, slot_id};
}

/// @brief Read a record from a given page and slot
/// @param record_id The page_id and the slot_id
/// @param record The record buffer in which will be stored
/// @return True if read from file succeded and then reading slot with given index succeds
bool TableFile::read_record(RecordId record_id, std::vector<uint8_t>& record) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Page page;
    return read_page(record_id.page_id, page) && page.read(record_id.slot_id, record);
}

/// @brief Scan all readable records in the table file
/// @param callback Called once for each readable record. Return false to stop scanning early.
/// @return False if the callback stops the scan, true if the full table file is scanned
bool TableFile::scan_records(const std::function<bool(RecordId, const std::vector<uint8_t>&)>& callback) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const uint32_t pages = page_count();
    for (uint32_t page_id = 0; page_id < pages; ++page_id) {
        Page page;
        if (!read_page(page_id, page)) {
            continue;
        }

        for (uint16_t slot_id = 0; slot_id < page.slot_count(); ++slot_id) {
            std::vector<uint8_t> record;
            if (page.read(slot_id, record)) {
                if (!callback(RecordId{page_id, slot_id}, record)) {
                    return false;
                }
            }
        }
    }

    return true;
}

/// @brief Scan all readable records in the table file
/// @return Pairs of record ids and record bytes, skipping deleted slots and unreadable pages
std::vector<std::pair<RecordId, std::vector<uint8_t>>> TableFile::scan_records() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::pair<RecordId, std::vector<uint8_t>>> records;
    scan_records([&records](RecordId record_id, const std::vector<uint8_t>& record) {
        records.push_back({record_id, record});
        return true;
    });
    return records;
}

/// @brief Delete a given record from its page and write the new page
/// @param record_id The page_id followed by slot_id
/// @return False if reading page, removing page or wrting new page fails True otherwise
bool TableFile::delete_record(RecordId record_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (record_id.page_id >= page_count()) {
        return false;
    }

    Page* page = page_cache_.fetch_page(path_, record_id.page_id, fd_);
    if (page == nullptr) {
        return false;
    }
    Page before = *page;
    if (!page->remove(record_id.slot_id)) {
        page_cache_.unpin_page(path_, record_id.page_id, false);
        return false;
    }
    if (!log_page_update(record_id.page_id, before, *page)) {
        *page = before;
        page_cache_.unpin_page(path_, record_id.page_id, false);
        return false;
    }
    return page_cache_.unpin_page(path_, record_id.page_id, true);
}

/// @brief Update a given record in the table and write new page (may be in different page from original)
/// @param record_id The page_id and slot_id of the record
/// @param record The data of the new record
/// @return False if the update fails True otherwise
bool TableFile::update_record(RecordId& record_id, const std::vector<uint8_t>& record) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (record_id.page_id >= page_count()) {
        return false;
    }

    Page* page = page_cache_.fetch_page(path_, record_id.page_id, fd_);
    std::vector<uint8_t> old_record;
    if (page == nullptr || !page->read(record_id.slot_id, old_record)) {
        if (page != nullptr) {
            page_cache_.unpin_page(path_, record_id.page_id, false);
        }
        return false;
    }
    Page before = *page;
    
    // Try updating in same page
    if (page->update(record_id.slot_id, record)) {
        if (!log_page_update(record_id.page_id, before, *page)) {
            *page = before;
            page_cache_.unpin_page(path_, record_id.page_id, false);
            return false;
        }
        return page_cache_.unpin_page(path_, record_id.page_id, true);
    }
    page_cache_.unpin_page(path_, record_id.page_id, false);

    // The insert and delete are logged under the same statement transaction by Database.
    // Recovery will undo both if the statement does not commit.
    RecordId new_record_id;
    try {
        new_record_id = insert_record(record);
    } catch (...) {
        return false;
    }

    if (!delete_record(record_id)) {
        delete_record(new_record_id);
        return false;
    }

    record_id = new_record_id;
    return true;
}

/// @brief Append a new page to the table file
/// @param page The object with the page data
/// @return False if fail to write new page True otherwise
bool TableFile::append_page(const Page& page) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0) {
        return false;
    }

    const off_t size = static_cast<off_t>(page_count_) * static_cast<off_t>(BYTE_SIZES::PAGE_SIZE);
    if (size < 0 || size % static_cast<off_t>(BYTE_SIZES::PAGE_SIZE) != 0) {
        return false;
    }

    Page logged_page = page;
    Page before_page;
    if (!log_page_update(page_count_, before_page, logged_page)) {
        return false;
    }
    if (wal_manager_ != nullptr && !wal_manager_->flush_through(logged_page.page_lsn())) {
        return false;
    }

    const bool ok = full_write(fd_, logged_page.data(), BYTE_SIZES::PAGE_SIZE, size);
    if (!ok) {
        return false;
    }

    const uint32_t page_id = page_count_;
    ++page_count_;
    Page* cached_page = page_cache_.new_page(path_, page_id, logged_page, fd_);
    if (cached_page == nullptr) {
        --page_count_;
        return false;
    }
    return page_cache_.unpin_page(path_, page_id, false);
}

bool TableFile::log_page_update(uint32_t page_id, const Page& before_page, Page& after_page) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (wal_manager_ == nullptr) {
        return true;
    }

    const uint64_t lsn = wal_manager_->log_page_update(transaction_id_, path_.string(), page_id, before_page, after_page);
    if (lsn == 0) {
        return false;
    }
    after_page.set_page_lsn(lsn);
    return true;
}
