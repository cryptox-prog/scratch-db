#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "storage/page.hpp"

enum class WalRecordType : uint16_t {
    begin_transaction = 1,
    page_update = 2,
    commit_transaction = 3,
    abort_transaction = 4,
    checkpoint = 5,
    create_table = 6,
    drop_table = 7,
    schema_update = 8,
};

class WalManager {
public:
    explicit WalManager(const std::filesystem::path& path);
    ~WalManager();

    WalManager(const WalManager&) = delete;
    WalManager& operator=(const WalManager&) = delete;

    uint64_t begin_transaction(uint64_t transaction_id);
    uint64_t log_page_update(
        uint64_t transaction_id,
        const std::string& table_file,
        uint32_t page_id,
        const Page& before_page,
        const Page& after_page
    );
    uint64_t commit_transaction(uint64_t transaction_id);
    uint64_t abort_transaction(uint64_t transaction_id);
    uint64_t checkpoint(const std::vector<uint64_t>& active_transactions);
    uint64_t log_create_table(uint64_t transaction_id, const std::filesystem::path& table_dir, const std::string& schema_text);
    uint64_t log_drop_table(uint64_t transaction_id, const std::filesystem::path& table_dir);
    uint64_t log_schema_update(
        uint64_t transaction_id,
        const std::filesystem::path& table_dir,
        const std::string& before_schema_text,
        const std::string& after_schema_text
    );

    bool flush();
    bool flush_through(uint64_t lsn);
    bool sync();
    bool recover();
    bool rollback_transaction(uint64_t transaction_id);

    uint64_t last_lsn() const;
    uint64_t durable_lsn() const;

private:
    uint64_t append_record(
        WalRecordType type,
        uint64_t transaction_id,
        const std::string& table_file,
        uint32_t page_id,
        const std::vector<uint8_t>& before_image,
        const std::vector<uint8_t>& after_image,
        const std::vector<uint64_t>& active_transactions
    );

    std::filesystem::path path_;
    int fd_ = -1;
    uint64_t last_lsn_ = 0;
    uint64_t durable_lsn_ = 0;
    mutable std::recursive_mutex mutex_;
};
