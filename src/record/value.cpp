#include "record/value.hpp"

/// @brief Create a null value
/// @return Null value object
Value Value::null_value() {
    return Value();
}

namespace {
    bool is_string_like(ColumnType type) {
        return type == ColumnType::character ||
               type == ColumnType::string ||
               type == ColumnType::varstring ||
               type == ColumnType::text;
    }

    uint8_t digit_count(uint64_t value) {
        uint8_t digits = 1;
        while (value >= 10) {
            value /= 10;
            ++digits;
        }
        return digits;
    }

    uint64_t absolute_value(int64_t value) {
        if (value >= 0) {
            return static_cast<uint64_t>(value);
        }
        return static_cast<uint64_t>(-(value + 1)) + 1;
    }
}

/// @brief Create an integer value
/// @param value The integer value to store
/// @return Value object holding the integer
Value Value::integer_value(int64_t value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::integer;
    result.integer_data_ = value;
    return result;
}

/// @brief Create a fixed-point number value
/// @param scaled_value The value multiplied by 10 to the column scale
/// @return Value object holding the scaled number
Value Value::number_value(int64_t scaled_value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::number;
    result.number_data_ = scaled_value;
    return result;
}

/// @brief Create a character value
/// @param value The character value to store
/// @return Value object holding the character
Value Value::char_value(char value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::character;
    result.string_data_ = std::string(1, value);
    return result;
}

/// @brief Create a fixed string value
/// @param value The string value to store
/// @return Value object holding the string
Value Value::string_value(const std::string& value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::string;
    result.string_data_ = value;
    return result;
}

/// @brief Create a variable string value
/// @param value The string value to store
/// @return Value object holding the variable string
Value Value::varstring_value(const std::string& value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::varstring;
    result.string_data_ = value;
    return result;
}

/// @brief Create a date value
/// @param value The date value to store
/// @return Value object holding the date
Value Value::date_value(Date value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::date;
    result.date_data_ = value;
    return result;
}

/// @brief Create a time value
/// @param value The time value to store
/// @return Value object holding the time
Value Value::time_value(Time value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::time;
    result.time_data_ = value;
    return result;
}

/// @brief Create a datetime value
/// @param value The datetime value to store
/// @return Value object holding the datetime
Value Value::datetime_value(DateTime value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::datetime;
    result.datetime_data_ = value;
    return result;
}

/// @brief Create a text value
/// @param value The text value to store
/// @return Value object holding the text
Value Value::text_value(const std::string& value) {
    Value result;
    result.is_null_ = false;
    result.type_ = ColumnType::text;
    result.string_data_ = value;
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

/// @brief Get the stored integer data
/// @return The integer value
int64_t Value::integer_data() const {
    return integer_data_;
}

/// @brief Get the stored fixed-point number data
/// @return The scaled number value
int64_t Value::number_data() const {
    return number_data_;
}

/// @brief Get the stored string-like data
/// @return The string-like value
const std::string& Value::string_data() const {
    return string_data_;
}

/// @brief Get the stored date data
/// @return The date value
Date Value::date_data() const {
    return date_data_;
}

/// @brief Get the stored time data
/// @return The time value
Time Value::time_data() const {
    return time_data_;
}

/// @brief Get the stored datetime data
/// @return The datetime value
DateTime Value::datetime_data() const {
    return datetime_data_;
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

    if (type_ == ColumnType::character) {
        return string_data_.size() == Column::CHAR_SIZE;
    }

    if (type_ == ColumnType::string) {
        return string_data_.size() <= column.max_size();
    }

    if (type_ == ColumnType::number) {
        return digit_count(absolute_value(number_data_)) <= column.precision();
    }

    if (type_ == ColumnType::date ||
        type_ == ColumnType::time ||
        type_ == ColumnType::datetime) {
        return true;
    }

    if (is_string_like(type_)) {
        return string_data_.size() <= column.max_size();
    }

    return true;
}
