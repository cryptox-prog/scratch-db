#include "catalog/schema.hpp"

#include <limits>
#include <stdexcept>

namespace {
    /// @brief Check if schema has valid columns
    /// @param columns The vector of columns in the schema
    /// @return False if invalid columns or invalid column combination
    bool are_valid_columns(const std::vector<Column>& columns) {
        if (columns.empty() || columns.size() > std::numeric_limits<uint16_t>::max()) {
            return false;
        }

        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (!columns[i].is_valid()) {
                return false;
            }

            for (std::size_t j = i + 1; j < columns.size(); ++j) {
                if (columns[i].name() == columns[j].name()) {
                    return false;
                }
            }
        }

        return true;
    }
}  // namespace

/// @brief Constructor for schema
/// @param table_name The name to set for the table
/// @param columns The vector of columns for the table
Schema::Schema(std::string table_name, std::vector<Column> columns) {
    if (!set_table_name(table_name)) {
        throw std::invalid_argument("invalid table name");
    }
    if (!set_columns(columns)) {
        throw std::invalid_argument("invalid schema columns");
    }
}

/// @brief Getter for table name
/// @return The table name
const std::string& Schema::table_name() const {
    return table_name_;
}

/// @brief Getter for columns in the table
/// @return The constant vector of columns
const std::vector<Column>& Schema::columns() const {
    return columns_;
}

/// @brief The number of columns in the schma
/// @return The number of columns
uint16_t Schema::column_count() const {
    return static_cast<uint16_t>(columns_.size());
}

/// @brief Get a column by its index
/// @param column_index The index of the column in the schema
/// @return Pointer to the column, or nullptr if index is invalid
const Column* Schema::column(uint16_t column_index) const {
    if (column_index >= columns_.size()) {
        return nullptr;
    }

    return &columns_[column_index];
}

/// @brief Find a column by name
/// @param column_name The name of the column to find
/// @return Pointer to the column, or nullptr if not found
const Column* Schema::find_column(const std::string& column_name) const {
    const int index = column_index(column_name);
    if (index < 0) {
        return nullptr;
    }

    return &columns_[static_cast<std::size_t>(index)];
}

/// @brief Find the index of a column by name
/// @param column_name The name of the column to find
/// @return Column index, or -1 if not found
int Schema::column_index(const std::string& column_name) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].name() == column_name) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

/// @brief Set the table name after validating it
/// @param table_name The table name to set
/// @return False if the table name is invalid
bool Schema::set_table_name(const std::string& table_name) {
    if (!is_valid_table_name(table_name)) {
        return false;
    }

    table_name_ = table_name;
    return true;
}

/// @brief Set the schema columns after validating them
/// @param columns The columns to set
/// @return False if columns are empty, invalid, too many, or have duplicate names
bool Schema::set_columns(const std::vector<Column>& columns) {
    if (!are_valid_columns(columns)) {
        return false;
    }
    columns_ = columns;
    return true;
}

/// @brief Check if the schema is valid
/// @return True if table name and columns are valid
bool Schema::is_valid() const {
    if (!is_valid_table_name(table_name_)) {
        return false;
    }

    return are_valid_columns(columns_);
}

/// @brief Check if a table name is allowed
/// @param table_name The table name to check
/// @return True if the table name is a valid identifier
bool Schema::is_valid_table_name(const std::string& table_name) {
    return Column::is_valid_name(table_name);
}
