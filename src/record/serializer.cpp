#include "record/serializer.hpp"

namespace {
    /// @brief Append a 16-bit unsigned integer to a record in little-endian order
    /// @param record The byte vector to append to
    /// @param value The value to write
    void write_uint16(std::vector<uint8_t>& record, uint16_t value) {
        record.push_back(static_cast<uint8_t>(value & 0xff));
        record.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    }

    /// @brief Append a 32-bit signed integer to a record in little-endian order
    /// @param record The byte vector to append to
    /// @param value The value to write
    void write_int32(std::vector<uint8_t>& record, int32_t value) {
        const uint32_t raw = static_cast<uint32_t>(value);
        record.push_back(static_cast<uint8_t>(raw & 0xff));
        record.push_back(static_cast<uint8_t>((raw >> 8) & 0xff));
        record.push_back(static_cast<uint8_t>((raw >> 16) & 0xff));
        record.push_back(static_cast<uint8_t>((raw >> 24) & 0xff));
    }

    /// @brief Read a 16-bit unsigned integer from a record in little-endian order
    /// @param record The record bytes to read from
    /// @param pos The current read position, advanced by two bytes on success
    /// @param value The output value
    /// @return False if there are not enough bytes left to read
    bool read_uint16(const std::vector<uint8_t>& record, std::size_t& pos, uint16_t& value) {
        if (pos + 2 > record.size()) {
            return false;
        }

        value = static_cast<uint16_t>(record[pos]) |
                static_cast<uint16_t>(static_cast<uint16_t>(record[pos + 1]) << 8);
        pos += 2;
        return true;
    }

    /// @brief Read a 32-bit signed integer from a record in little-endian order
    /// @param record The record bytes to read from
    /// @param pos The current read position, advanced by four bytes on success
    /// @param value The output value
    /// @return False if there are not enough bytes left to read
    bool read_int32(const std::vector<uint8_t>& record, std::size_t& pos, int32_t& value) {
        if (pos + 4 > record.size()) {
            return false;
        }

        const uint32_t raw =
            static_cast<uint32_t>(record[pos]) |
            (static_cast<uint32_t>(record[pos + 1]) << 8) |
            (static_cast<uint32_t>(record[pos + 2]) << 16) |
            (static_cast<uint32_t>(record[pos + 3]) << 24);
        value = static_cast<int32_t>(raw);
        pos += 4;
        return true;
    }

    /// @brief Check whether a column's null flag is set in the null bitmap
    /// @param record The record bytes, with the null bitmap at the front
    /// @param column_index The column whose null bit should be checked
    /// @return True if the column is marked null
    bool null_bit_is_set(const std::vector<uint8_t>& record, uint16_t column_index) {
        // division findes byte index, modulo finds bit index
        return (record[column_index / 8] & static_cast<uint8_t>(1u << (column_index % 8))) != 0;
    }

    /// @brief Mark a column as null in the null bitmap
    /// @param record The record bytes, with the null bitmap at the front
    /// @param column_index The column whose null bit should be set
    void set_null_bit(std::vector<uint8_t>& record, uint16_t column_index) {
        record[column_index / 8] |= static_cast<uint8_t>(1u << (column_index % 8));
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

    std::vector<uint8_t> out(null_bitmap_size(schema.column_count()), 0);

    for (uint16_t i = 0; i < schema.column_count(); ++i) {
        const Column* column = schema.column(i);
        const Value* value = row.value(i);
        if (column == nullptr || value == nullptr || !value->matches_column(*column)) {
            return false;
        }

        if (value->is_null()) {
            set_null_bit(out, i);
            continue;
        }

        if (column->type() == ColumnType::int32) {
            write_int32(out, value->int32_data());
        } else if (column->type() == ColumnType::text) {
            const std::string& text = value->text_data();
            write_uint16(out, static_cast<uint16_t>(text.size()));
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

    std::size_t pos = null_bitmap_size(schema.column_count());
    std::vector<Value> values;
    values.reserve(schema.column_count());

    for (uint16_t i = 0; i < schema.column_count(); ++i) {
        const Column* column = schema.column(i);
        if (column == nullptr) {
            return false;
        }

        if (null_bit_is_set(record, i)) {
            if (!column->nullable()) {
                return false;
            }
            values.push_back(Value::null_value());
            continue;
        }

        if (column->type() == ColumnType::int32) {
            int32_t value = 0;
            if (!read_int32(record, pos, value)) {
                return false;
            }
            values.push_back(Value::int32_value(value));
        } else if (column->type() == ColumnType::text) {
            uint16_t size = 0;
            if (!read_uint16(record, pos, size) || pos + size > record.size() || size > column->max_size()) {
                return false;
            }

            values.push_back(Value::text_value(std::string(record.begin() + pos, record.begin() + pos + size)));
            pos += size;
        } else {
            return false;
        }
    }

    if (pos != record.size()) {
        return false;
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
