#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "common/types.hpp"
#include "storage/page_cache.hpp"
#include "storage/page.hpp"

class TableFile {
public:
    explicit TableFile(const std::filesystem::path& path);
    TableFile(const std::filesystem::path& path, WalManager* wal_manager);
    TableFile(const std::filesystem::path& path, WalManager* wal_manager, uint64_t transaction_id);
    ~TableFile();

    TableFile(const TableFile&) = delete;
    TableFile& operator=(const TableFile&) = delete;

    uint32_t page_count() const;
    bool flush();
    bool sync();
    void discard_cache();

    bool read_page(uint32_t page_id, Page& page) const;
    bool write_page(uint32_t page_id, const Page& page);

    RecordId insert_record(const std::vector<uint8_t>& record);
    bool read_record(RecordId record_id, std::vector<uint8_t>& record) const;
    bool scan_records(const std::function<bool(RecordId, const std::vector<uint8_t>&)>& callback) const;
    std::vector<std::pair<RecordId, std::vector<uint8_t>>> scan_records() const;
    bool delete_record(RecordId record_id);
    bool update_record(RecordId& record_id, const std::vector<uint8_t>& record);

private:
    bool append_page(const Page& page);
    bool log_page_update(uint32_t page_id, const Page& before_page, Page& after_page);

    std::filesystem::path path_;
    int fd_ = -1;
    uint32_t page_count_ = 0;
    mutable PageCache page_cache_;
    WalManager* wal_manager_ = nullptr;
    uint64_t transaction_id_ = 0;
    mutable std::recursive_mutex mutex_;
};
