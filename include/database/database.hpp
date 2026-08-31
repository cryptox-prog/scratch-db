#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "catalog/catalog.hpp"
#include "concurrency/lock_manager.hpp"
#include "common/types.hpp"
#include "record/row.hpp"
#include "storage/wal_manager.hpp"

struct TableRow {
    RecordId record_id;
    Row row;
};

class Database {
public:
    Database(const std::filesystem::path& data_root, std::string database_name);

    bool create_database();
    bool database_exists() const;
    std::vector<std::string> list_databases() const;

    bool create_table(const Schema& schema);
    bool table_exists(const std::string& table_name) const;
    std::vector<std::string> list_tables() const;
    std::optional<Schema> load_schema(const std::string& table_name) const;

    std::optional<RecordId> insert_row(const std::string& table_name, const Row& row);
    std::optional<Row> read_row(const std::string& table_name, RecordId record_id) const;
    bool scan_rows(const std::string& table_name, const std::function<bool(const TableRow&)>& callback) const;
    std::vector<TableRow> scan_rows(const std::string& table_name) const;
    bool delete_row(const std::string& table_name, RecordId record_id);
    bool update_row(const std::string& table_name, RecordId& record_id, const Row& row);
    bool add_constraint(const std::string& table_name, const ConstraintDefinition& constraint);
    bool drop_constraint(const std::string& table_name, uint64_t constraint_id);
    bool begin_transaction();
    bool commit_transaction();
    bool rollback_transaction();
    bool in_transaction() const;

private:
    LockManager::TableLock acquire_statement_lock(const std::string& table_name, TableLockMode mode) const;
    bool acquire_transaction_lock(const std::string& table_name, TableLockMode mode) const;
    uint64_t current_transaction_id();
    bool finish_statement(uint64_t transaction_id);
    bool abort_statement(uint64_t transaction_id);
    std::optional<std::vector<uint8_t>> make_record(const std::string& table_name, const Row& row) const;
    std::string serialize_schema(const Schema& schema) const;
    std::filesystem::path table_directory_path(const std::string& table_name) const;
    WalManager* wal_manager();
    uint64_t next_transaction_id();
    bool commit_statement(uint64_t transaction_id);
    bool rollback_statement(uint64_t transaction_id);
    std::string table_lock_key(const std::string& table_name) const;
    static LockManager& lock_manager();

    std::filesystem::path data_root_;
    Catalog catalog_;
    std::string database_name_;
    std::unique_ptr<WalManager> wal_manager_;
    uint64_t next_transaction_id_ = 1;
    uint64_t active_transaction_id_ = 0;
    mutable std::unordered_map<std::string, TableLockMode> active_table_locks_;
    mutable std::unordered_map<std::string, LockManager::TableLock> active_locks_;
    mutable std::mutex mutex_;
};
