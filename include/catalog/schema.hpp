#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/column.hpp"

class Schema {
public:
    Schema(std::string table_name, std::vector<Column> columns);

    const std::string& table_name() const;
    const std::vector<Column>& columns() const;
    uint16_t column_count() const;

    const Column* column(uint16_t column_index) const;
    const Column* find_column(const std::string& column_name) const;
    int column_index(const std::string& column_name) const;

    bool set_table_name(const std::string& table_name);
    bool set_columns(const std::vector<Column>& columns);

    bool is_valid() const;

    static bool is_valid_table_name(const std::string& table_name);

private:
    std::string table_name_;
    std::vector<Column> columns_;
};
