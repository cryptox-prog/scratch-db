#include "record/serializer.hpp"

#include <cstring>

namespace {
    /// @brief Check if type stores its payload after the fixed-width record area
    /// @param type The column type
    /// @return True for variable-size values
    bool is_variable_type(ColumnType type) {
        return type == ColumnType::varstring || type == ColumnType::text;
    }

    /// @brief Get the number of fixed-area bytes used by a column
    /// @param column The column metadata
    /// @return Fixed storage bytes for the column
    uint16_t storage_size(const Column& column) {
        if (is_variable_type(column.type())) {
            return Column::VAR_POINTER_SIZE;
        }
        if (column.type() == ColumnType::string) {
            return column.max_size();
        }
        return Column::fixed_size(column.type());
    }

    /// @brief Write a 16-bit unsigned integer at an existing record offset
    /// @param record The byte vector to write into
    /// @param pos The offset where the value should start
    /// @param value The value to write
    void write_uint16_at(std::vector<uint8_t>& record, std::size_t pos, uint16_t value) {
        record[pos] = static_cast<uint8_t>(value & 0xff);
        record[pos + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    }

    /// @brief Write a 64-bit signed integer at an existing record offset
    /// @param record The byte vector to write into
    /// @param pos The offset where the value should start
    /// @param value The value to write
    void write_int64_at(std::vector<uint8_t>& record, std::size_t pos, int64_t value) {
        const uint64_t raw = static_cast<uint64_t>(value);
        for (uint16_t i = 0; i < Column::INTEGER_SIZE; ++i) {
            record[pos + i] = static_cast<uint8_t>((raw >> (i * 8)) & 0xff);
        }
    }

    /// @brief Write a 32-bit unsigned integer at an existing record offset
    /// @param record The byte vector to write into
    /// @param pos The offset where the value should start
    /// @param value The value to write
    void write_uint32_at(std::vector<uint8_t>& record, std::size_t pos, uint32_t value) {
        for (uint16_t i = 0; i < 4; ++i) {
            record[pos + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
        }
    }

    /// @brief Write a 32-bit signed integer at an existing record offset
    /// @param record The byte vector to write into
    /// @param pos The offset where the value should start
    /// @param value The value to write
    void write_int32_at(std::vector<uint8_t>& record, std::size_t pos, int32_t value) {
        write_uint32_at(record, pos, static_cast<uint32_t>(value));
    }

    /// @brief Read a 16-bit unsigned integer from a record offset
    /// @param record The record bytes
    /// @param pos The offset where the value starts
    /// @param value The output value
    /// @return False if there are not enough bytes left to read
    bool read_uint16_at(const std::vector<uint8_t>& record, std::size_t pos, uint16_t& value) {
        if (pos + 2 > record.size()) {
            return false;
        }

        value = static_cast<uint16_t>(record[pos]) |
                static_cast<uint16_t>(static_cast<uint16_t>(record[pos + 1]) << 8);
        return true;
    }

    /// @brief Read a 64-bit signed integer from a record offset
    /// @param record The record bytes
    /// @param pos The offset where the value starts
    /// @param value The output value
    /// @return False if there are not enough bytes left to read
    bool read_int64_at(const std::vector<uint8_t>& record, std::size_t pos, int64_t& value) {
        if (pos + Column::INTEGER_SIZE > record.size()) {
            return false;
        }

        uint64_t raw = 0;
        for (uint16_t i = 0; i < Column::INTEGER_SIZE; ++i) {
            raw |= static_cast<uint64_t>(record[pos + i]) << (i * 8);
        }
        value = static_cast<int64_t>(raw);
        return true;
    }

    /// @brief Read a 32-bit unsigned integer from a record offset
    /// @param record The record bytes
    /// @param pos The offset where the value starts
    /// @param value The output value
    /// @return False if there are not enough bytes left to read
    bool read_uint32_at(const std::vector<uint8_t>& record, std::size_t pos, uint32_t& value) {
        if (pos + 4 > record.size()) {
            return false;
        }

        value = 0;
        for (uint16_t i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(record[pos + i]) << (i * 8);
        }
        return true;
    }

    /// @brief Read a 32-bit signed integer from a record offset
    /// @param record The record bytes
    /// @param pos The offset where the value starts
    /// @param value The output value
    /// @return False if there are not enough bytes left to read
    bool read_int32_at(const std::vector<uint8_t>& record, std::size_t pos, int32_t& value) {
        uint32_t raw = 0;
        if (!read_uint32_at(record, pos, raw)) {
            return false;
        }
        value = static_cast<int32_t>(raw);
        return true;
    }

    /// @brief Check whether a column's null flag is set in the null bitmap
    /// @param record The record bytes, with the null bitmap at the front
    /// @param column_index The column whose null bit should be checked
    /// @return True if the column is marked null
    bool null_bit_is_set(const std::vector<uint8_t>& record, uint16_t column_index) {
        return (record[column_index / 8] & static_cast<uint8_t>(1u << (column_index % 8))) != 0;
    }

    /// @brief Mark a column as null in the null bitmap
    /// @param record The record bytes, with the null bitmap at the front
    /// @param column_index The column whose null bit should be set
    void set_null_bit(std::vector<uint8_t>& record, uint16_t column_index) {
        record[column_index / 8] |= static_cast<uint8_t>(1u << (column_index % 8));
    }

    /// @brief Build a string and remove fixed-width zero padding from the end
    /// @param record The record bytes
    /// @param pos The fixed-area position where the string begins
    /// @param size The fixed string byte width
    /// @return String without trailing zero padding
    std::string read_padded_string(const std::vector<uint8_t>& record, std::size_t pos, uint16_t size) {
        std::string value(record.begin() + pos, record.begin() + pos + size);
        while (!value.empty() && value.back() == '\0') {
            value.pop_back();
        }
        return value;
    }
}  // namespace

/// @brief Serialize a row into the on-page record byte format
/// @param schema The schema that defines column order and types
/// @param row The row values to serialize
/// @param record The output byte vector, replaced only on success
/// @return False if the schema, row shape, or any value is invalid
bool RecordSerializer::serialize(const Schema& schema, const Row& row, std::vector<uint8_t>& record) {
    if (!schema.is_valid() || row.value_count() != schema.column_count()) {
        return false;
    }

    uint16_t fixed_area_size = 0;
    for (const Column& column : schema.columns()) {
        fixed_area_size = static_cast<uint16_t>(fixed_area_size + storage_size(column));
    }

    std::vector<uint8_t> out(static_cast<std::size_t>(null_bitmap_size(schema.column_count())) + fixed_area_size, 0);
    std::size_t fixed_pos = null_bitmap_size(schema.column_count());

    for (uint16_t i = 0; i < schema.column_count(); ++i) {
        const Column* column = schema.column(i);
        const Value* value = row.value(i);
        if (column == nullptr || value == nullptr || !value->matches_column(*column)) {
            return false;
        }

        const std::size_t column_pos = fixed_pos;
        fixed_pos += storage_size(*column);

        if (value->is_null()) {
            set_null_bit(out, i);
            continue;
        }

        if (column->type() == ColumnType::integer) {
            write_int64_at(out, column_pos, value->integer_data());
        } else if (column->type() == ColumnType::number) {
            write_int64_at(out, column_pos, value->number_data());
        } else if (column->type() == ColumnType::character) {
            out[column_pos] = static_cast<uint8_t>(value->string_data()[0]);
        } else if (column->type() == ColumnType::string) {
            std::memcpy(out.data() + column_pos, value->string_data().data(), value->string_data().size());
        } else if (column->type() == ColumnType::date) {
            write_int32_at(out, column_pos, value->date_data().days_since_epoch());
        } else if (column->type() == ColumnType::time) {
            write_uint32_at(out, column_pos, value->time_data().seconds_since_midnight());
        } else if (column->type() == ColumnType::datetime) {
            write_int64_at(out, column_pos, value->datetime_data().seconds_since_epoch());
        } else if (is_variable_type(column->type())) {
            const std::string& text = value->string_data();
            const uint16_t offset = static_cast<uint16_t>(out.size());
            write_uint16_at(out, column_pos, static_cast<uint16_t>(text.size()));
            write_uint16_at(out, column_pos + 2, offset);
            out.insert(out.end(), text.begin(), text.end());
        } else {
            return false;
        }
    }

    record = out;
    return true;
}

/// @brief Deserialize on-page record bytes into a row
/// @param schema The schema that defines column order and types
/// @param record The record bytes to decode
/// @param row The output row, replaced only on success
/// @return False if the record bytes do not match the schema
bool RecordSerializer::deserialize(const Schema& schema, const std::vector<uint8_t>& record, Row& row) {
    if (!schema.is_valid() || record.size() < null_bitmap_size(schema.column_count())) {
        return false;
    }

    uint16_t fixed_area_size = 0;
    for (const Column& column : schema.columns()) {
        fixed_area_size = static_cast<uint16_t>(fixed_area_size + storage_size(column));
    }
    if (record.size() < static_cast<std::size_t>(null_bitmap_size(schema.column_count())) + fixed_area_size) {
        return false;
    }

    std::size_t fixed_pos = null_bitmap_size(schema.column_count());
    std::vector<Value> values;
    values.reserve(schema.column_count());

    for (uint16_t i = 0; i < schema.column_count(); ++i) {
        const Column* column = schema.column(i);
        if (column == nullptr) {
            return false;
        }

        const std::size_t column_pos = fixed_pos;
        fixed_pos += storage_size(*column);

        if (null_bit_is_set(record, i)) {
            if (!column->nullable()) {
                return false;
            }
            values.push_back(Value::null_value());
            continue;
        }

        if (column->type() == ColumnType::integer) {
            int64_t value = 0;
            if (!read_int64_at(record, column_pos, value)) {
                return false;
            }
            values.push_back(Value::integer_value(value));
        } else if (column->type() == ColumnType::number) {
            int64_t value = 0;
            if (!read_int64_at(record, column_pos, value)) {
                return false;
            }
            values.push_back(Value::number_value(value));
        } else if (column->type() == ColumnType::character) {
            values.push_back(Value::char_value(static_cast<char>(record[column_pos])));
        } else if (column->type() == ColumnType::string) {
            values.push_back(Value::string_value(read_padded_string(record, column_pos, column->max_size())));
        } else if (column->type() == ColumnType::date) {
            int32_t days = 0;
            if (!read_int32_at(record, column_pos, days)) {
                return false;
            }
            values.push_back(Value::date_value(Date(days)));
        } else if (column->type() == ColumnType::time) {
            uint32_t seconds = 0;
            if (!read_uint32_at(record, column_pos, seconds) || seconds >= 86400) {
                return false;
            }
            values.push_back(Value::time_value(Time(seconds)));
        } else if (column->type() == ColumnType::datetime) {
            int64_t seconds = 0;
            if (!read_int64_at(record, column_pos, seconds)) {
                return false;
            }
            values.push_back(Value::datetime_value(DateTime(seconds)));
        } else if (is_variable_type(column->type())) {
            uint16_t size = 0;
            uint16_t offset = 0;
            if (!read_uint16_at(record, column_pos, size) ||
                !read_uint16_at(record, column_pos + 2, offset) ||
                size > column->max_size() ||
                static_cast<std::size_t>(offset) + size > record.size()) {
                return false;
            }
            const std::string text(record.begin() + offset, record.begin() + offset + size);
            values.push_back(column->type() == ColumnType::text ? Value::text_value(text) : Value::varstring_value(text));
        } else {
            return false;
        }
    }

    row = Row(values);
    return true;
}

/// @brief Calculate how many bytes are needed for a null bitmap
/// @param column_count The number of columns represented by the bitmap
/// @return The number of bitmap bytes needed
uint16_t RecordSerializer::null_bitmap_size(uint16_t column_count) {
    return static_cast<uint16_t>((column_count + 7) / 8);
}
