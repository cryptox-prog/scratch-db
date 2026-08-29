#include "catalog/column.hpp"

#include <cctype>
#include <stdexcept>

/// @brief Constructor for the class performs all validation checks
/// @param name The name of the column
/// @param type The datatype of the column
/// @param nullable True if column can have null value
/// @param max_size The max size for columns like text
/// @exception If invalid column name
/// @exception If invalid column max size
/// @note Invalid column name has special charecters or space
Column::Column(std::string name, ColumnType type, bool nullable, uint16_t max_size) {
    if (type == ColumnType::null_type) {
        throw std::invalid_argument("null_type cannot be used for a column");
    }

    if (!set_name(name)) {
        throw std::invalid_argument("invalid column name");
    }
    set_type(type);
    set_nullable(nullable);
    if (!set_max_size(max_size)) {
        throw std::invalid_argument("invalid column max_size");
    }
}

/// @brief Get a column of int32 type
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @return The column object
/// @exception If invalid column name
/// @note Invalid column name has special charecters or space
Column Column::int32_column(const std::string& name, bool nullable) {
    return Column(name, ColumnType::int32, nullable, fixed_size(ColumnType::int32));
}

/// @brief Get a column of text type
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @param max_size The max size for columns like text
/// @exception If invalid column name
/// @exception If invalid column max size
/// @note Invalid column name has special charecters or space
Column Column::text_column(const std::string& name, bool nullable, uint16_t max_size) {
    return Column(name, ColumnType::text, nullable, max_size);
}

/// @brief A helper function for creating columns from catalogue
/// @param name The name of the column
/// @param type The datatype of the column
/// @param nullable True if column can have null value
/// @param max_size The max size for columns like text
/// @return The column of the requested type
/// @exception If non matching size of the column to the type
/// @exception If invalid column name
/// @exception If invalid column max size
/// @exception If unknown column type
/// @note Invalid column name has special charecters or space
Column Column::from_catalog(
    const std::string& name,
    ColumnType type,
    bool nullable,
    uint16_t max_size
) {
    if (type == ColumnType::int32) {
        if (max_size != fixed_size(ColumnType::int32)) {
            throw std::invalid_argument("invalid int32 column max_size");
        }
        return int32_column(name, nullable);
    }

    if (type == ColumnType::text) {
        return text_column(name, nullable, max_size);
    }

    if (type == ColumnType::null_type) {
        throw std::invalid_argument("null_type cannot be used for a column");
    }

    throw std::invalid_argument("invalid column type");
}

/// @brief Getter for column name
/// @return The column name
const std::string& Column::name() const {
    return name_;
}

/// @brief Getter for column type
/// @return The column type
ColumnType Column::type() const {
    return type_;
}

/// @brief Getter for nullable property
/// @return True if column is nullable
bool Column::nullable() const {
    return nullable_;
}

/// @brief Getter for the max size property
/// @return The max size of the column
uint16_t Column::max_size() const {
    return max_size_;
}

/// @brief Setter for name property
/// @param name The name to set
/// @return False if invalid name
bool Column::set_name(const std::string& name) {
    if (!is_valid_name(name)) {
        return false;
    }

    name_ = name;
    return true;
}

/// @brief Setter for the type property
/// @param type The datatype to for the column
void Column::set_type(ColumnType type) {
    if (type == ColumnType::null_type) {
        return;
    }

    type_ = type;
    if (type_ == ColumnType::int32) {
        max_size_ = fixed_size(ColumnType::int32);
    }
}

/// @brief Setter for the nullable property
/// @param nullable True if null values are allowed in the column
void Column::set_nullable(bool nullable) {
    nullable_ = nullable;
}

/// @brief Setter for the max size property
/// @param max_size The max size to set
/// @return False if invalid max size true otherwise
bool Column::set_max_size(uint16_t max_size) {
    if (max_size == 0) {
        return false;
    }

    if (type_ == ColumnType::int32 && max_size != fixed_size(ColumnType::int32)) {
        return false;
    }

    if (type_ == ColumnType::text && max_size > TEXT_MAX_SIZE) {
        return false;
    }

    max_size_ = max_size;
    return true;
}

/// @brief Checks if the column is valid according to its type
/// @return True if name and max size are valid for the column type
bool Column::is_valid() const {
    if (!is_valid_name(name_) || max_size_ == 0) {
        return false;
    }

    if (type_ == ColumnType::int32) {
        return max_size_ == fixed_size(ColumnType::int32);
    }

    if (type_ == ColumnType::text) {
        return max_size_ <= TEXT_MAX_SIZE;
    }

    return true;
}

/// @brief Checks if a column name is allowed
/// @param name The column name to check
/// @return True if name starts with letter or underscore and contains only letters, digits, or underscore
bool Column::is_valid_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(name[0]);
    if (!std::isalpha(first) && name[0] != '_') {
        return false;
    }

    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '_') {
            return false;
        }
    }

    return true;
}

/// @brief Converts a column type enum to catalogue text
/// @param type The column type to convert
/// @return String name of the type
std::string Column::type_to_string(ColumnType type) {
    switch (type) {
        case ColumnType::null_type:
            return "null";
        case ColumnType::int32:
            return "int32";
        case ColumnType::text:
            return "text";
    }

    return "unknown";
}

/// @brief Converts catalogue text into a column type enum
/// @param text The type name stored in the catalogue
/// @param type Output parameter that receives the parsed type
/// @return True if the text matched a known column type
bool Column::type_from_string(const std::string& text, ColumnType& type) {
    if (text == "null") {
        type = ColumnType::null_type;
        return true;
    }

    if (text == "int32") {
        type = ColumnType::int32;
        return true;
    }

    if (text == "text") {
        type = ColumnType::text;
        return true;
    }

    return false;
}

/// @brief Gets the fixed byte size for a column type
/// @param type The column type to inspect
/// @return Fixed byte size, or VARIABLE_SIZE for variable size types
uint16_t Column::fixed_size(ColumnType type) {
    switch (type) {
        case ColumnType::null_type:
            return VARIABLE_SIZE;
        case ColumnType::int32:
            return INT32_SIZE;
        case ColumnType::text:
            return VARIABLE_SIZE;
    }

    return VARIABLE_SIZE;
}
