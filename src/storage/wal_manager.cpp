#include "storage/wal_manager.hpp"

#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <unistd.h>

namespace {
    constexpr uint32_t WAL_MAGIC = 0x304c4157;
    constexpr uint32_t NO_PAGE_ID = 0xffffffff;
    constexpr const char* SCHEMA_FILE_NAME = "schema.catalog";
    constexpr const char* TABLE_FILE_NAME = "data.tbl";

    bool full_write(int fd, const uint8_t* buffer, std::size_t size) {
        std::size_t total = 0;
        while (total < size) {
            const ssize_t n = write(fd, buffer + total, size - total);
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

    bool full_write_at(int fd, const uint8_t* buffer, std::size_t size, off_t offset) {
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

    bool full_read_at(int fd, uint8_t* buffer, std::size_t size, off_t offset) {
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
            total += static_cast<std::size_t>(n);
        }
        return true;
    }

    void append_uint16(std::vector<uint8_t>& out, uint16_t value) {
        out.push_back(static_cast<uint8_t>(value & 0xff));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    }

    void append_uint32(std::vector<uint8_t>& out, uint32_t value) {
        for (uint16_t i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
        }
    }

    void append_uint64(std::vector<uint8_t>& out, uint64_t value) {
        for (uint16_t i = 0; i < 8; ++i) {
            out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
        }
    }

    uint16_t read_uint16(const uint8_t* ptr) {
        return static_cast<uint16_t>(ptr[0]) | static_cast<uint16_t>(static_cast<uint16_t>(ptr[1]) << 8);
    }

    uint32_t read_uint32(const uint8_t* ptr) {
        uint32_t value = 0;
        for (uint16_t i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(ptr[i]) << (i * 8);
        }
        return value;
    }

    uint64_t read_uint64(const uint8_t* ptr) {
        uint64_t value = 0;
        for (uint16_t i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(ptr[i]) << (i * 8);
        }
        return value;
    }

    std::vector<uint8_t> page_image(const Page& page) {
        return std::vector<uint8_t>(page.data(), page.data() + BYTE_SIZES::PAGE_SIZE);
    }

    Page page_from_image(const std::vector<uint8_t>& image) {
        Page page;
        if (image.size() == BYTE_SIZES::PAGE_SIZE) {
            std::copy(image.begin(), image.end(), page.data());
        }
        return page;
    }

    const char* path_c_str(const std::filesystem::path& path) {
        return path.c_str();
    }

    off_t file_size(int fd) {
        struct stat st {};
        if (fstat(fd, &st) != 0) {
            return -1;
        }
        return st.st_size;
    }

    struct WalRecord {
        WalRecordType type = WalRecordType::begin_transaction;
        uint64_t lsn = 0;
        uint64_t transaction_id = 0;
        uint32_t page_id = NO_PAGE_ID;
        std::string table_file;
        std::vector<uint8_t> before_image;
        std::vector<uint8_t> after_image;
        std::vector<uint64_t> active_transactions;
    };

    bool read_all_records(int fd, std::vector<WalRecord>& records) {
        constexpr std::size_t HEADER_SIZE = 42;
        const off_t size = file_size(fd);
        if (size < 0) {
            return false;
        }

        off_t offset = 0;
        while (offset + static_cast<off_t>(HEADER_SIZE) <= size) {
            uint8_t header[HEADER_SIZE] {};
            if (!full_read_at(fd, header, HEADER_SIZE, offset)) {
                return false;
            }
            if (read_uint32(header) != WAL_MAGIC) {
                return false;
            }

            WalRecord record;
            record.type = static_cast<WalRecordType>(read_uint16(header + 4));
            record.lsn = read_uint64(header + 6);
            record.transaction_id = read_uint64(header + 14);
            record.page_id = read_uint32(header + 22);
            const uint32_t table_file_size = read_uint32(header + 26);
            const uint32_t before_size = read_uint32(header + 30);
            const uint32_t after_size = read_uint32(header + 34);
            const uint32_t active_count = read_uint32(header + 38);

            const uint64_t body_size = static_cast<uint64_t>(table_file_size) + before_size + after_size + static_cast<uint64_t>(active_count) * 8;
            if (offset + static_cast<off_t>(HEADER_SIZE) + static_cast<off_t>(body_size) > size) {
                break;
            }

            std::vector<uint8_t> body(static_cast<std::size_t>(body_size));
            if (!body.empty() && !full_read_at(fd, body.data(), body.size(), offset + static_cast<off_t>(HEADER_SIZE))) {
                return false;
            }

            std::size_t body_offset = 0;
            record.table_file.assign(reinterpret_cast<const char*>(body.data()), table_file_size);
            body_offset += table_file_size;
            record.before_image.assign(body.begin() + static_cast<std::ptrdiff_t>(body_offset), body.begin() + static_cast<std::ptrdiff_t>(body_offset + before_size));
            body_offset += before_size;
            record.after_image.assign(body.begin() + static_cast<std::ptrdiff_t>(body_offset), body.begin() + static_cast<std::ptrdiff_t>(body_offset + after_size));
            body_offset += after_size;
            for (uint32_t i = 0; i < active_count; ++i) {
                record.active_transactions.push_back(read_uint64(body.data() + body_offset));
                body_offset += 8;
            }

            records.push_back(std::move(record));
            offset += static_cast<off_t>(HEADER_SIZE) + static_cast<off_t>(body_size);
        }

        return true;
    }

    bool write_page_image(const WalRecord& record, const std::vector<uint8_t>& image, uint64_t page_lsn) {
        if (record.page_id == NO_PAGE_ID || record.table_file.empty() || image.size() != BYTE_SIZES::PAGE_SIZE) {
            return false;
        }

        const int fd = open(path_c_str(record.table_file), O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            return false;
        }

        Page page = page_from_image(image);
        page.set_page_lsn(page_lsn);
        const off_t offset = static_cast<off_t>(record.page_id) * static_cast<off_t>(BYTE_SIZES::PAGE_SIZE);
        const bool ok = full_write_at(fd, page.data(), BYTE_SIZES::PAGE_SIZE, offset);
        close(fd);
        return ok;
    }

    std::string text_from_bytes(const std::vector<uint8_t>& bytes) {
        return std::string(bytes.begin(), bytes.end());
    }

    bool redo_create_table(const WalRecord& record) {
        if (record.table_file.empty()) {
            return false;
        }

        const std::filesystem::path table_dir(record.table_file);
        std::error_code error;
        std::filesystem::create_directories(table_dir, error);
        if (error) {
            return false;
        }

        std::ofstream data_file(table_dir / TABLE_FILE_NAME, std::ios::binary | std::ios::app);
        if (!data_file) {
            return false;
        }
        data_file.close();

        std::ofstream schema_file(table_dir / SCHEMA_FILE_NAME, std::ios::binary | std::ios::trunc);
        if (!schema_file) {
            return false;
        }
        const std::string schema_text = text_from_bytes(record.after_image);
        schema_file.write(schema_text.data(), static_cast<std::streamsize>(schema_text.size()));
        return static_cast<bool>(schema_file);
    }

    bool redo_drop_table(const WalRecord& record) {
        if (record.table_file.empty()) {
            return false;
        }

        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(record.table_file), error);
        return !error;
    }
}  // namespace

WalManager::WalManager(const std::filesystem::path& path) : path_(path) {
    fd_ = open(path_c_str(path_), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("could not open wal file: " + path_.string());
    }

    std::vector<WalRecord> records;
    if (read_all_records(fd_, records)) {
        for (const WalRecord& record : records) {
            if (record.lsn > last_lsn_) {
                last_lsn_ = record.lsn;
            }
        }
        durable_lsn_ = last_lsn_;
    }
}

WalManager::~WalManager() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ >= 0) {
        flush();
        close(fd_);
        fd_ = -1;
    }
}

uint64_t WalManager::begin_transaction(uint64_t transaction_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return append_record(WalRecordType::begin_transaction, transaction_id, "", NO_PAGE_ID, {}, {}, {});
}

uint64_t WalManager::log_page_update(
    uint64_t transaction_id,
    const std::string& table_file,
    uint32_t page_id,
    const Page& before_page,
    const Page& after_page
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return append_record(
        WalRecordType::page_update,
        transaction_id,
        table_file,
        page_id,
        page_image(before_page),
        page_image(after_page),
        {}
    );
}

uint64_t WalManager::commit_transaction(uint64_t transaction_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return append_record(WalRecordType::commit_transaction, transaction_id, "", NO_PAGE_ID, {}, {}, {});
}

uint64_t WalManager::abort_transaction(uint64_t transaction_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return append_record(WalRecordType::abort_transaction, transaction_id, "", NO_PAGE_ID, {}, {}, {});
}

uint64_t WalManager::checkpoint(const std::vector<uint64_t>& active_transactions) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return append_record(WalRecordType::checkpoint, 0, "", NO_PAGE_ID, {}, {}, active_transactions);
}

uint64_t WalManager::log_create_table(uint64_t transaction_id, const std::filesystem::path& table_dir, const std::string& schema_text) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return append_record(
        WalRecordType::create_table,
        transaction_id,
        table_dir.lexically_normal().string(),
        NO_PAGE_ID,
        {},
        std::vector<uint8_t>(schema_text.begin(), schema_text.end()),
        {}
    );
}

uint64_t WalManager::log_drop_table(uint64_t transaction_id, const std::filesystem::path& table_dir) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return append_record(WalRecordType::drop_table, transaction_id, table_dir.lexically_normal().string(), NO_PAGE_ID, {}, {}, {});
}

bool WalManager::flush() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0) {
        return false;
    }
    while (fdatasync(fd_) != 0) {
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
    durable_lsn_ = last_lsn_;
    return true;
}

bool WalManager::flush_through(uint64_t lsn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (lsn <= durable_lsn_) {
        return true;
    }
    return flush();
}

bool WalManager::sync() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return flush();
}

bool WalManager::recover() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0) {
        return false;
    }

    std::vector<WalRecord> records;
    if (!read_all_records(fd_, records)) {
        return false;
    }

    std::set<uint64_t> started;
    std::set<uint64_t> committed;
    std::set<uint64_t> aborted;
    for (const WalRecord& record : records) {
        if (record.type == WalRecordType::begin_transaction) {
            started.insert(record.transaction_id);
        } else if (record.type == WalRecordType::commit_transaction) {
            committed.insert(record.transaction_id);
        } else if (record.type == WalRecordType::abort_transaction) {
            aborted.insert(record.transaction_id);
        }
        if (record.lsn > last_lsn_) {
            last_lsn_ = record.lsn;
        }
    }

    std::set<uint64_t> losers;
    for (uint64_t transaction_id : started) {
        if (committed.find(transaction_id) == committed.end() && aborted.find(transaction_id) == aborted.end()) {
            losers.insert(transaction_id);
        }
    }

    for (const WalRecord& record : records) {
        if (record.transaction_id != 0 && committed.find(record.transaction_id) == committed.end() && aborted.find(record.transaction_id) == aborted.end() && losers.find(record.transaction_id) == losers.end()) {
            continue;
        }
        if (record.type == WalRecordType::page_update && !record.after_image.empty()) {
            if (!write_page_image(record, record.after_image, record.lsn)) {
                return false;
            }
        } else if (record.type == WalRecordType::create_table) {
            if (!redo_create_table(record)) {
                return false;
            }
        } else if (record.type == WalRecordType::drop_table) {
            if (!redo_drop_table(record)) {
                return false;
            }
        }
    }

    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const WalRecord& record = *it;
        if (losers.find(record.transaction_id) == losers.end()) {
            continue;
        }
        if (record.type == WalRecordType::page_update && !record.before_image.empty()) {
            Page before_page = page_from_image(record.after_image);
            Page after_page = page_from_image(record.before_image);
            const uint64_t compensation_lsn = log_page_update(record.transaction_id, record.table_file, record.page_id, before_page, after_page);
            if (compensation_lsn == 0) {
                return false;
            }
            if (!write_page_image(record, record.before_image, compensation_lsn)) {
                return false;
            }
        } else if (record.type == WalRecordType::create_table) {
            if (log_drop_table(record.transaction_id, record.table_file) == 0 || !redo_drop_table(record)) {
                return false;
            }
        }
    }

    for (uint64_t transaction_id : losers) {
        if (abort_transaction(transaction_id) == 0) {
            return false;
        }
    }

    return flush();
}

bool WalManager::rollback_transaction(uint64_t transaction_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0 || transaction_id == 0) {
        return false;
    }

    std::vector<WalRecord> records;
    if (!read_all_records(fd_, records)) {
        return false;
    }

    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const WalRecord& record = *it;
        if (record.transaction_id != transaction_id) {
            continue;
        }

        if (record.type == WalRecordType::page_update && !record.before_image.empty()) {
            Page before_page = page_from_image(record.after_image);
            Page after_page = page_from_image(record.before_image);
            const uint64_t compensation_lsn = log_page_update(transaction_id, record.table_file, record.page_id, before_page, after_page);
            if (compensation_lsn == 0) {
                return false;
            }
            if (!write_page_image(record, record.before_image, compensation_lsn)) {
                return false;
            }
        } else if (record.type == WalRecordType::create_table) {
            if (log_drop_table(transaction_id, record.table_file) == 0 || !redo_drop_table(record)) {
                return false;
            }
        }
    }

    if (abort_transaction(transaction_id) == 0) {
        return false;
    }
    return flush();
}

uint64_t WalManager::last_lsn() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return last_lsn_;
}

uint64_t WalManager::durable_lsn() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return durable_lsn_;
}

uint64_t WalManager::append_record(
    WalRecordType type,
    uint64_t transaction_id,
    const std::string& table_file,
    uint32_t page_id,
    const std::vector<uint8_t>& before_image,
    const std::vector<uint8_t>& after_image,
    const std::vector<uint64_t>& active_transactions
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (fd_ < 0) {
        return 0;
    }

    const uint64_t lsn = last_lsn_ + 1;
    std::vector<uint8_t> record;
    append_uint32(record, WAL_MAGIC);
    append_uint16(record, static_cast<uint16_t>(type));
    append_uint64(record, lsn);
    append_uint64(record, transaction_id);
    append_uint32(record, page_id);
    append_uint32(record, static_cast<uint32_t>(table_file.size()));
    append_uint32(record, static_cast<uint32_t>(before_image.size()));
    append_uint32(record, static_cast<uint32_t>(after_image.size()));
    append_uint32(record, static_cast<uint32_t>(active_transactions.size()));
    record.insert(record.end(), table_file.begin(), table_file.end());
    record.insert(record.end(), before_image.begin(), before_image.end());
    record.insert(record.end(), after_image.begin(), after_image.end());
    for (uint64_t active_transaction : active_transactions) {
        append_uint64(record, active_transaction);
    }

    if (!full_write(fd_, record.data(), record.size())) {
        return 0;
    }

    last_lsn_ = lsn;
    return lsn;
}
