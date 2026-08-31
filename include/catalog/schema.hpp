#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/column.hpp"

enum class TableStorageMode {
    disk,
    memory,
};

struct ConstraintDefinition {
    uint64_t id = 0;
    std::string kind;
    std::vector<std::string> columns;
    std::vector<std::string> args;

    static ConstraintDefinition make_primary_key(uint64_t constraint_id, std::vector<std::string> columns);
    static ConstraintDefinition make_unique(uint64_t constraint_id, std::vector<std::string> columns);
    static ConstraintDefinition make_foreign_key(uint64_t constraint_id, std::vector<std::string> columns, std::string referenced_table, std::string referenced_column);
    static ConstraintDefinition make_check(uint64_t constraint_id, std::vector<std::string> expression_parts);

    std::string serialized() const;
    static ConstraintDefinition from_serialized(const std::string& text);
};

struct IndexDefinition {
    uint64_t id = 0;
    std::string name;
    std::vector<std::string> columns;
    bool unique = false;

    static IndexDefinition make(uint64_t index_id, std::string name, std::vector<std::string> columns, bool unique);

    std::string serialized() const;
    static IndexDefinition from_serialized(const std::string& text);
};

class Schema {
public:
    Schema(std::string table_name, std::vector<Column> columns);
    Schema(std::string table_name, std::vector<Column> columns, std::vector<ConstraintDefinition> constraints);
    Schema(std::string table_name, std::vector<Column> columns, std::vector<ConstraintDefinition> constraints, std::vector<IndexDefinition> indexes);
    Schema(std::string table_name, std::vector<Column> columns, std::vector<ConstraintDefinition> constraints, std::vector<IndexDefinition> indexes, TableStorageMode storage_mode);

    const std::string& table_name() const;
    const std::vector<Column>& columns() const;
    const std::vector<ConstraintDefinition>& constraints() const;
    const std::vector<IndexDefinition>& indexes() const;
    TableStorageMode storage_mode() const;
    uint16_t column_count() const;

    const Column* column(uint16_t column_index) const;
    const Column* find_column(const std::string& column_name) const;
    int column_index(const std::string& column_name) const;

    bool set_table_name(const std::string& table_name);
    bool set_columns(const std::vector<Column>& columns);
    bool set_constraints(const std::vector<ConstraintDefinition>& constraints);
    bool set_indexes(const std::vector<IndexDefinition>& indexes);
    bool set_storage_mode(TableStorageMode storage_mode);

    bool is_valid() const;

    static bool is_valid_table_name(const std::string& table_name);

private:
    std::string table_name_;
    std::vector<Column> columns_;
    std::vector<ConstraintDefinition> constraints_;
    std::vector<IndexDefinition> indexes_;
    TableStorageMode storage_mode_ = TableStorageMode::disk;
};
