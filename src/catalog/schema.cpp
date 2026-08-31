#include "catalog/schema.hpp"

#include <stdexcept>

#include "common/constants.hpp"

namespace {
    /// @brief Check if schema has valid columns
    /// @param columns The vector of columns in the schema
    /// @return False if invalid columns or invalid column combination
    bool are_valid_columns(const std::vector<Column>& columns) {
        if (columns.empty() || columns.size() > LIMITS::MAX_COLUMNS) {
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

    bool is_valid_constraint_name(const std::string& kind) {
        return kind == "primary_key" || kind == "unique" || kind == "foreign_key" || kind == "check";
    }
}  // namespace

ConstraintDefinition ConstraintDefinition::make_primary_key(uint64_t constraint_id, std::vector<std::string> columns) {
    ConstraintDefinition constraint;
    constraint.id = constraint_id;
    constraint.kind = "primary_key";
    constraint.columns = std::move(columns);
    return constraint;
}

ConstraintDefinition ConstraintDefinition::make_unique(uint64_t constraint_id, std::vector<std::string> columns) {
    ConstraintDefinition constraint;
    constraint.id = constraint_id;
    constraint.kind = "unique";
    constraint.columns = std::move(columns);
    return constraint;
}

ConstraintDefinition ConstraintDefinition::make_foreign_key(uint64_t constraint_id, std::vector<std::string> columns, std::string referenced_table, std::string referenced_column) {
    ConstraintDefinition constraint;
    constraint.id = constraint_id;
    constraint.kind = "foreign_key";
    constraint.columns = std::move(columns);
    constraint.args = {std::move(referenced_table), std::move(referenced_column)};
    return constraint;
}

ConstraintDefinition ConstraintDefinition::make_check(uint64_t constraint_id, std::vector<std::string> expression_parts) {
    ConstraintDefinition constraint;
    constraint.id = constraint_id;
    constraint.kind = "check";
    constraint.args = std::move(expression_parts);
    return constraint;
}

namespace {
    std::vector<std::string> split_csv(const std::string& text) {
        std::vector<std::string> parts;
        if (text.empty()) {
            return parts;
        }

        std::string current;
        for (char ch : text) {
            if (ch == ',') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(ch);
            }
        }
        if (!current.empty()) {
            parts.push_back(current);
        }
        return parts;
    }
}  // namespace

std::string ConstraintDefinition::serialized() const {
    std::string text = std::to_string(id) + "|" + kind;
    if (!columns.empty()) {
        text += "|";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) {
                text += ",";
            }
            text += columns[i];
        }
    }
    if (!args.empty()) {
        text += "|";
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                text += ",";
            }
            text += args[i];
        }
    }
    return text;
}

ConstraintDefinition ConstraintDefinition::from_serialized(const std::string& text) {
    ConstraintDefinition constraint;
    std::string copied = text;
    std::size_t first = copied.find('|');
    if (first == std::string::npos) {
        return constraint;
    }

    const std::string id_text = copied.substr(0, first);
    if (!id_text.empty()) {
        constraint.id = static_cast<uint64_t>(std::stoull(id_text));
    }

    std::size_t second = copied.find('|', first + 1);
    if (second == std::string::npos) {
        constraint.kind = copied.substr(first + 1);
        return constraint;
    }

    constraint.kind = copied.substr(first + 1, second - first - 1);
    std::string remaining = copied.substr(second + 1);

    std::size_t third = remaining.find('|');
    if (third == std::string::npos) {
        if (constraint.kind == "check") {
            constraint.args = split_csv(remaining);
        } else {
            constraint.columns = split_csv(remaining);
        }
        return constraint;
    }

    const std::string columns_text = remaining.substr(0, third);
    const std::string args_text = remaining.substr(third + 1);
    if (!columns_text.empty()) {
        constraint.columns = split_csv(columns_text);
    }
    if (!args_text.empty()) {
        constraint.args = split_csv(args_text);
    }

    if (constraint.kind == "check" && constraint.columns.empty()) {
        constraint.args = split_csv(remaining);
        constraint.columns.clear();
    }

    return constraint;
}

IndexDefinition IndexDefinition::make(uint64_t index_id, std::string name, std::vector<std::string> columns, bool unique) {
    IndexDefinition index;
    index.id = index_id;
    index.name = std::move(name);
    index.columns = std::move(columns);
    index.unique = unique;
    return index;
}

std::string IndexDefinition::serialized() const {
    std::string text = std::to_string(id) + "|" + name + "|" + (unique ? "1" : "0") + "|";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) {
            text += ",";
        }
        text += columns[i];
    }
    return text;
}

IndexDefinition IndexDefinition::from_serialized(const std::string& text) {
    IndexDefinition index;
    std::size_t first = text.find('|');
    std::size_t second = first == std::string::npos ? std::string::npos : text.find('|', first + 1);
    std::size_t third = second == std::string::npos ? std::string::npos : text.find('|', second + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos) {
        return index;
    }

    index.id = static_cast<uint64_t>(std::stoull(text.substr(0, first)));
    index.name = text.substr(first + 1, second - first - 1);
    index.unique = text.substr(second + 1, third - second - 1) == "1";
    index.columns = split_csv(text.substr(third + 1));
    return index;
}

/// @brief Constructor for schema
/// @param table_name The name to set for the table
/// @param columns The vector of columns for the table
Schema::Schema(std::string table_name, std::vector<Column> columns) : Schema(std::move(table_name), std::move(columns), {}) {}

Schema::Schema(std::string table_name, std::vector<Column> columns, std::vector<ConstraintDefinition> constraints)
    : Schema(std::move(table_name), std::move(columns), std::move(constraints), {}) {}

Schema::Schema(std::string table_name, std::vector<Column> columns, std::vector<ConstraintDefinition> constraints, std::vector<IndexDefinition> indexes)
    : Schema(std::move(table_name), std::move(columns), std::move(constraints), std::move(indexes), TableStorageMode::disk) {}

Schema::Schema(std::string table_name, std::vector<Column> columns, std::vector<ConstraintDefinition> constraints, std::vector<IndexDefinition> indexes, TableStorageMode storage_mode) {
    if (!set_table_name(table_name)) {
        throw std::invalid_argument("invalid table name");
    }
    if (!set_columns(columns)) {
        throw std::invalid_argument("invalid schema columns");
    }
    if (!set_constraints(constraints)) {
        throw std::invalid_argument("invalid schema constraints");
    }
    if (!set_indexes(indexes)) {
        throw std::invalid_argument("invalid schema indexes");
    }
    if (!set_storage_mode(storage_mode)) {
        throw std::invalid_argument("invalid storage mode");
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

const std::vector<ConstraintDefinition>& Schema::constraints() const {
    return constraints_;
}

const std::vector<IndexDefinition>& Schema::indexes() const {
    return indexes_;
}

TableStorageMode Schema::storage_mode() const {
    return storage_mode_;
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

bool Schema::set_constraints(const std::vector<ConstraintDefinition>& constraints) {
    bool has_primary_key = false;
    for (std::size_t i = 0; i < constraints.size(); ++i) {
        const ConstraintDefinition& constraint = constraints[i];
        if (!is_valid_constraint_name(constraint.kind) || constraint.id == 0) {
            return false;
        }
        for (std::size_t j = i + 1; j < constraints.size(); ++j) {
            if (constraint.id == constraints[j].id) {
                return false;
            }
        }
        if (constraint.kind == "primary_key" || constraint.kind == "unique") {
            if (constraint.columns.empty()) {
                return false;
            }
            if (constraint.kind == "primary_key") {
                if (has_primary_key) {
                    return false;
                }
                has_primary_key = true;
            }
            for (const std::string& column_name : constraint.columns) {
                if (column_index(column_name) < 0) {
                    return false;
                }
            }
        }
        if (constraint.kind == "foreign_key") {
            if (constraint.columns.size() != 1 || constraint.args.size() != 2 || constraint.args[0].empty() || constraint.args[1].empty()) {
                return false;
            }
            if (column_index(constraint.columns[0]) < 0) {
                return false;
            }
        }
        if (constraint.kind == "check") {
            if (constraint.args.size() != 3) {
                return false;
            }
            if (column_index(constraint.args[0]) < 0) {
                return false;
            }
        }
    }
    constraints_ = constraints;
    return true;
}

bool Schema::set_indexes(const std::vector<IndexDefinition>& indexes) {
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        const IndexDefinition& index = indexes[i];
        if (index.id == 0 || !Column::is_valid_name(index.name) || index.columns.size() != 1) {
            return false;
        }
        if (column_index(index.columns[0]) < 0) {
            return false;
        }
        for (std::size_t j = i + 1; j < indexes.size(); ++j) {
            if (index.id == indexes[j].id || index.name == indexes[j].name) {
                return false;
            }
        }
    }
    indexes_ = indexes;
    return true;
}

bool Schema::set_storage_mode(TableStorageMode storage_mode) {
    storage_mode_ = storage_mode;
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
