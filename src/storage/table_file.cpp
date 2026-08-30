#include "storage/table_file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
    /// @brief Read the given amount of bytes from the given file
    /// @param fd The file Descriptor to read from
    /// @param buffer The pointer to where we store the bytes read
    /// @param size The number of bytes to read
    /// @param offset The offset in file to start reading from
    /// @return False if some error is encountered in reading file True otherwise
    /// @note This is needed cause posix might fewer then requested bytes
    bool full_read(int fd, uint8_t* buffer, std::size_t size, off_t offset) {
        std::size_t total = 0;
        while (total < size) {
            const ssize_t n = pread(fd, buffer + total, size - total, offset + static_cast<off_t>(total));
            if (n == 0) {
                return false;
            }
            if (n < 0) {
                if (errno == EINTR) { // try to read again
                    continue;
                }
                return false;
            }
            total += static_cast<std::size_t>(n);
        }
        return true;
    }

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
TableFile::TableFile(const std::filesystem::path& path) : path_(path) {
    // open file for read write if not exists create it
    // octal permission code for read write for owner
    const int fd = open(path_c_str(path_), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        throw std::runtime_error("could not open table file: " + path_.string());
    }

    const off_t size = file_size(fd);
    if (size < 0 || size % static_cast<off_t>(BYTE_SIZES::PAGE_SIZE) != 0) {
        close(fd);
        throw std::runtime_error("table file size is not page aligned: " + path_.string());
    }

    close(fd);
}

/// @brief Get the number of pages in the file
/// @return The number of pages in the file
uint32_t TableFile::page_count() const {
    const int fd = open(path_c_str(path_), O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    const off_t size = file_size(fd);
    close(fd);
    // since page size is constant and file is a multiple of pages, a division gives number of pages
    if (size < 0 || size % static_cast<off_t>(BYTE_SIZES::PAGE_SIZE) != 0) {
        return 0;
    }

    return static_cast<uint32_t>(size / static_cast<off_t>(BYTE_SIZES::PAGE_SIZE));
}

/// @brief Read a given page from the file
/// @param page_id The index of the page in the file
/// @param page The object to write page data into
/// @return False if failed to read page True otherwise
bool TableFile::read_page(uint32_t page_id, Page& page) const {
    // TODO: Add a LFU cache for pages (across tables only for session???)
    if (page_id >= page_count()) {
        return false;
    }

    const int fd = open(path_c_str(path_), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    // Page IDs are positional: page n starts at n * BYTE_SIZES::PAGE_SIZE.
    const off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(BYTE_SIZES::PAGE_SIZE);
    const bool ok = full_read(fd, page.data(), static_cast<std::size_t>(BYTE_SIZES::PAGE_SIZE), offset);
    close(fd);
    return ok;
}

/// @brief Write a give page into the file
/// @param page_id The index of the page in the file
/// @param page The object in which the page data is stored
/// @return False if some error occured in write otherwise True
bool TableFile::write_page(uint32_t page_id, const Page& page) {
    if (page_id >= page_count()) {
        return false;
    }

    const int fd = open(path_c_str(path_), O_RDWR);
    if (fd < 0) {
        return false;
    }

    const off_t offset = static_cast<off_t>(page_id) * static_cast<off_t>(BYTE_SIZES::PAGE_SIZE);
    const bool ok = full_write(fd, page.data(), BYTE_SIZES::PAGE_SIZE, offset);
    close(fd);
    return ok;
}

/// @brief Insert a given record into a page and write to file
/// @param record The record to insert
/// @return The page_id and slot_id to identify record uniquely in table file
/// @exception If the record is larger than page
/// @exception If append file failed
RecordId TableFile::insert_record(const std::vector<uint8_t>& record) {
    // TODO: Free space map
    // Current: First fit scan
    for (uint32_t page_id = 0; page_id < page_count(); ++page_id) {
        Page page;
        if (!read_page(page_id, page)) {
            continue;
        }

        uint16_t slot_id = 0;
        if (page.insert(record, slot_id) && write_page(page_id, page)) {
            return RecordId{page_id, slot_id};
        }
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
    Page page;
    return read_page(record_id.page_id, page) && page.read(record_id.slot_id, record);
}

/// @brief Scan all readable records in the table file
/// @param callback Called once for each readable record. Return false to stop scanning early.
/// @return False if the callback stops the scan, true if the full table file is scanned
bool TableFile::scan_records(const std::function<bool(RecordId, const std::vector<uint8_t>&)>& callback) const {
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
    Page page;
    if (!read_page(record_id.page_id, page) || !page.remove(record_id.slot_id)) {
        return false;
    }
    return write_page(record_id.page_id, page);
}

/// @brief Update a given record in the table and write new page (may be in different page from original)
/// @param record_id The page_id and slot_id of the record
/// @param record The data of the new record
/// @return False if the update fails True otherwise
bool TableFile::update_record(RecordId& record_id, const std::vector<uint8_t>& record) {
    Page page;
    std::vector<uint8_t> old_record;
    if (!read_page(record_id.page_id, page) || !page.read(record_id.slot_id, old_record)) {
        return false;
    }
    
    // Try updating in same page
    if (page.update(record_id.slot_id, record)) {
        return write_page(record_id.page_id, page);
    }

    // If fails try inserting in differnt page
    // then delete it from its original page
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
    const int fd = open(path_c_str(path_), O_RDWR);
    if (fd < 0) {
        return false;
    }

    const off_t size = file_size(fd);
    if (size < 0 || size % static_cast<off_t>(BYTE_SIZES::PAGE_SIZE) != 0) {
        close(fd);
        return false;
    }

    const bool ok = full_write(fd, page.data(), BYTE_SIZES::PAGE_SIZE, size);
    close(fd);
    return ok;
}
