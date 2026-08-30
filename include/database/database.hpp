#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
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

private:
    std::optional<std::vector<uint8_t>> make_record(const std::string& table_name, const Row& row) const;
    std::string serialize_schema(const Schema& schema) const;
    std::filesystem::path table_directory_path(const std::string& table_name) const;
    WalManager* wal_manager();
    uint64_t next_transaction_id();
    bool commit_statement(uint64_t transaction_id);
    bool rollback_statement(uint64_t transaction_id);

    std::filesystem::path data_root_;
    Catalog catalog_;
    std::string database_name_;
    std::unique_ptr<WalManager> wal_manager_;
    uint64_t next_transaction_id_ = 1;
};
