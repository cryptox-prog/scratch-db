#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "common/types.hpp"
#include "record/row.hpp"

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

    Catalog catalog_;
    std::string database_name_;
};
