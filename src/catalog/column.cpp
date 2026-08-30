#include "catalog/column.hpp"

#include <cctype>
#include <stdexcept>

#include "common/constants.hpp"

namespace {
    bool is_variable_type(ColumnType type) {
        return type == ColumnType::varstring || type == ColumnType::text;
    }
}

/// @brief Constructor for the class performs all validation checks
/// @param name The name of the column
/// @param type The datatype of the column
/// @param nullable True if column can have null value
/// @param max_size The fixed size or variable max size for the column
/// @exception If invalid column name, type, or max size
Column::Column(std::string name, ColumnType type, bool nullable, uint16_t max_size, uint8_t precision, uint8_t scale) {
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
    if (!set_number_format(precision, scale)) {
        throw std::invalid_argument("invalid number format");
    }
}

/// @brief Get an integer column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @return The column object
Column Column::integer_column(const std::string& name, bool nullable) {
    return Column(name, ColumnType::integer, nullable, fixed_size(ColumnType::integer));
}

/// @brief Get a fixed-point number column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @param precision Total number of digits allowed
/// @param scale Number of digits after the decimal point
/// @return The column object
Column Column::number_column(const std::string& name, bool nullable, uint8_t precision, uint8_t scale) {
    return Column(name, ColumnType::number, nullable, fixed_size(ColumnType::number), precision, scale);
}

/// @brief Get a one-character column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @return The column object
Column Column::char_column(const std::string& name, bool nullable) {
    return Column(name, ColumnType::character, nullable, fixed_size(ColumnType::character));
}

/// @brief Get a fixed-size string column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @param size The exact number of bytes reserved for the string
/// @return The column object
Column Column::string_column(const std::string& name, bool nullable, uint16_t size) {
    return Column(name, ColumnType::string, nullable, size);
}

/// @brief Get a variable-size string column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @param max_size The maximum number of bytes allowed
/// @return The column object
Column Column::varstring_column(const std::string& name, bool nullable, uint16_t max_size) {
    return Column(name, ColumnType::varstring, nullable, max_size);
}

/// @brief Get a date column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @return The column object
Column Column::date_column(const std::string& name, bool nullable) {
    return Column(name, ColumnType::date, nullable, fixed_size(ColumnType::date));
}

/// @brief Get a time column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @return The column object
Column Column::time_column(const std::string& name, bool nullable) {
    return Column(name, ColumnType::time, nullable, fixed_size(ColumnType::time));
}

/// @brief Get a datetime column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @return The column object
Column Column::datetime_column(const std::string& name, bool nullable) {
    return Column(name, ColumnType::datetime, nullable, fixed_size(ColumnType::datetime));
}

/// @brief Get a text column
/// @param name The name of the column
/// @param nullable True if column can have null value
/// @return The column object
Column Column::text_column(const std::string& name, bool nullable) {
    return Column(name, ColumnType::text, nullable, TEXT_MAX_SIZE);
}

/// @brief A helper function for creating columns from catalogue
/// @param name The name of the column
/// @param type The datatype of the column
/// @param nullable True if column can have null value
/// @param max_size The fixed size or variable max size stored in catalog
/// @return The column of the requested type
/// @exception If type and size do not match
Column Column::from_catalog(const std::string& name, ColumnType type, bool nullable, uint16_t max_size) {
    return from_catalog(name, type, nullable, max_size, 0, 0);
}

Column Column::from_catalog(
    const std::string& name,
    ColumnType type,
    bool nullable,
    uint16_t max_size,
    uint8_t precision,
    uint8_t scale
) {
    if (type == ColumnType::integer) {
        if (max_size != fixed_size(type)) {
            throw std::invalid_argument("invalid integer column max_size");
        }
        return integer_column(name, nullable);
    }
    if (type == ColumnType::number) {
        if (max_size != fixed_size(type)) {
            throw std::invalid_argument("invalid number column max_size");
        }
        return number_column(name, nullable, precision, scale);
    }
    if (type == ColumnType::character) {
        if (max_size != fixed_size(type)) {
            throw std::invalid_argument("invalid char column max_size");
        }
        return char_column(name, nullable);
    }
    if (type == ColumnType::string) {
        return string_column(name, nullable, max_size);
    }
    if (type == ColumnType::varstring) {
        return varstring_column(name, nullable, max_size);
    }
    if (type == ColumnType::date) {
        if (max_size != fixed_size(type)) {
            throw std::invalid_argument("invalid date column max_size");
        }
        return date_column(name, nullable);
    }
    if (type == ColumnType::time) {
        if (max_size != fixed_size(type)) {
            throw std::invalid_argument("invalid time column max_size");
        }
        return time_column(name, nullable);
    }
    if (type == ColumnType::datetime) {
        if (max_size != fixed_size(type)) {
            throw std::invalid_argument("invalid datetime column max_size");
        }
        return datetime_column(name, nullable);
    }
    if (type == ColumnType::text) {
        if (max_size != TEXT_MAX_SIZE) {
            throw std::invalid_argument("invalid text column max_size");
        }
        return text_column(name, nullable);
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

/// @brief Getter for number precision
/// @return Total number of digits allowed for NUMBER columns
uint8_t Column::precision() const {
    return precision_;
}

/// @brief Getter for number scale
/// @return Number of digits after the decimal point for NUMBER columns
uint8_t Column::scale() const {
    return scale_;
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
/// @param type The datatype for the column
void Column::set_type(ColumnType type) {
    if (type == ColumnType::null_type) {
        return;
    }

    type_ = type;
    if (!is_variable_type(type_) && type_ != ColumnType::string) {
        max_size_ = fixed_size(type_);
    } else if (type_ == ColumnType::text) {
        max_size_ = TEXT_MAX_SIZE;
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

    if (type_ == ColumnType::string && max_size > STRING_MAX_SIZE) {
        return false;
    }
    if (type_ == ColumnType::varstring && max_size > VARSTRING_MAX_SIZE) {
        return false;
    }
    if (type_ == ColumnType::text && max_size != TEXT_MAX_SIZE) {
        return false;
    }
    if (!is_variable_type(type_) && type_ != ColumnType::string && max_size != fixed_size(type_)) {
        return false;
    }

    max_size_ = max_size;
    return true;
}

/// @brief Setter for NUMBER precision and scale
/// @param precision Total number of digits allowed
/// @param scale Number of digits after the decimal point
/// @return False if format is invalid for this column
bool Column::set_number_format(uint8_t precision, uint8_t scale) {
    if (type_ != ColumnType::number) {
        precision_ = 0;
        scale_ = 0;
        return precision == 0 && scale == 0;
    }

    if (precision == 0 || precision > LIMITS::MAX_NUMBER_PRECISION || scale > precision) {
        return false;
    }

    precision_ = precision;
    scale_ = scale;
    return true;
}

/// @brief Checks if the column is valid according to its type
/// @return True if name and max size are valid for the column type
bool Column::is_valid() const {
    if (!is_valid_name(name_) || max_size_ == 0 || type_ == ColumnType::null_type) {
        return false;
    }

    if (type_ == ColumnType::string) {
        return max_size_ <= STRING_MAX_SIZE;
    }
    if (type_ == ColumnType::varstring) {
        return max_size_ <= VARSTRING_MAX_SIZE;
    }
    if (type_ == ColumnType::text) {
        return max_size_ == TEXT_MAX_SIZE;
    }
    if (type_ == ColumnType::number) {
        return max_size_ == fixed_size(type_) &&
               precision_ > 0 &&
               precision_ <= LIMITS::MAX_NUMBER_PRECISION &&
               scale_ <= precision_;
    }

    return max_size_ == fixed_size(type_);
}

/// @brief Checks if a column name is allowed
/// @param name The column name to check
/// @return True if name starts with lowercase letter or underscore and contains only lowercase letters, digits, or underscore
bool Column::is_valid_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }

    const unsigned char first = static_cast<unsigned char>(name[0]);
    if (!std::islower(first) && name[0] != '_') {
        return false;
    }

    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((!std::islower(c) && !std::isdigit(c)) && ch != '_') {
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
        case ColumnType::integer:
            return "integer";
        case ColumnType::number:
            return "number";
        case ColumnType::character:
            return "char";
        case ColumnType::string:
            return "string";
        case ColumnType::varstring:
            return "varstring";
        case ColumnType::date:
            return "date";
        case ColumnType::time:
            return "time";
        case ColumnType::datetime:
            return "datetime";
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
    if (text == "integer") {
        type = ColumnType::integer;
        return true;
    }
    if (text == "number") {
        type = ColumnType::number;
        return true;
    }
    if (text == "char") {
        type = ColumnType::character;
        return true;
    }
    if (text == "string") {
        type = ColumnType::string;
        return true;
    }
    if (text == "varstring") {
        type = ColumnType::varstring;
        return true;
    }
    if (text == "date") {
        type = ColumnType::date;
        return true;
    }
    if (text == "time") {
        type = ColumnType::time;
        return true;
    }
    if (text == "datetime") {
        type = ColumnType::datetime;
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
        case ColumnType::integer:
            return INTEGER_SIZE;
        case ColumnType::number:
            return NUMBER_SIZE;
        case ColumnType::character:
            return CHAR_SIZE;
        case ColumnType::string:
            return VARIABLE_SIZE;
        case ColumnType::varstring:
            return VARIABLE_SIZE;
        case ColumnType::date:
            return DATE_SIZE;
        case ColumnType::time:
            return TIME_SIZE;
        case ColumnType::datetime:
            return DATETIME_SIZE;
        case ColumnType::text:
            return VARIABLE_SIZE;
    }

    return VARIABLE_SIZE;
}
