#include "record/value.hpp"

/// @brief Create a null value
/// @return Null value object
Value Value::null_value() {
    return Value();
}

/// @brief Create an int32 value
/// @param value The integer value to store
/// @return Value object holding the int32
Value Value::int32_value(int32_t value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::int32;
    result.int32_data_ = value;
    return result;
}

/// @brief Create a text value
/// @param value The string value to store
/// @return Value object holding the text
Value Value::text_value(const std::string& value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::text;
    result.text_data_ = value;
    return result;
}

/// @brief Check if this value represents SQL NULL
/// @return True if the value is null
bool Value::is_null() const {
    return is_null_;
}

/// @brief Get the type of this value
/// @return ColumnType of the value, or null_type for null values
ColumnType Value::type() const {
    return type_;
}

/// @brief Get the stored int32 data
/// @return The int32 value
int32_t Value::int32_data() const {
    return int32_data_;
}

/// @brief Get the stored text data
/// @return The text value
const std::string& Value::text_data() const {
    return text_data_;
}

/// @brief Check if this value can be stored in a given column
/// @param column The column metadata to check against
/// @return True if nullability, type, and max size rules are satisfied
bool Value::matches_column(const Column& column) const {
    if (is_null_) {
        return column.nullable();
    }

    if (type_ != column.type()) {
        return false;
    }

    if (type_ == ColumnType::text) {
        return text_data_.size() <= column.max_size();
    }

    return true;
}
