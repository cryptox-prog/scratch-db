#include "query/value_parser.hpp"

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
    struct TextPart {
        std::string text;
        std::size_t position = 0;
    };

    ValueParseResult value_result(const Value& value) {
        ValueParseResult result;
        result.value = value;
        return result;
    }

    ValueParseResult row_result(const Row& row) {
        ValueParseResult result;
        result.row = row;
        return result;
    }

    ValueParseResult value_error(const std::string& message, const std::string& token, std::size_t position) {
        ValueParseResult result;
        result.error = ValueParseError{message, token, position};
        return result;
    }

    std::string trim(const std::string& text) {
        std::size_t start = 0;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
            ++start;
        }

        std::size_t end = text.size();
        while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
            --end;
        }

        return text.substr(start, end - start);
    }

    std::vector<TextPart> split_csv_with_positions(const std::string& text, std::size_t base_position) {
        std::vector<TextPart> parts;
        std::string current;
        std::size_t current_position = base_position;
        bool in_string = false;
        uint16_t paren_depth = 0;

        for (std::size_t i = 0; i < text.size(); ++i) {
            const char ch = text[i];
            if (ch == '\'') {
                in_string = !in_string;
                current.push_back(ch);
            } else if (ch == '(' && !in_string) {
                ++paren_depth;
                current.push_back(ch);
            } else if (ch == ')' && !in_string && paren_depth > 0) {
                --paren_depth;
                current.push_back(ch);
            } else if (ch == ',' && !in_string && paren_depth == 0) {
                std::size_t local_start = 0;
                while (local_start < current.size() && std::isspace(static_cast<unsigned char>(current[local_start]))) {
                    ++local_start;
                }
                parts.push_back({trim(current), current_position + local_start});
                current.clear();
                current_position = base_position + i + 1;
            } else {
                current.push_back(ch);
            }
        }

        if (!current.empty() || !text.empty()) {
            std::size_t local_start = 0;
            while (local_start < current.size() && std::isspace(static_cast<unsigned char>(current[local_start]))) {
                ++local_start;
            }
            parts.push_back({trim(current), current_position + local_start});
        }

        return parts;
    }

    bool parse_integer(const std::string& text, int64_t& value) {
        try {
            std::size_t used = 0;
            const long long parsed = std::stoll(text, &used);
            if (used != text.size()) {
                return false;
            }
            value = static_cast<int64_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }

    int64_t power_of_10(uint8_t exponent) {
        int64_t value = 1;
        for (uint8_t i = 0; i < exponent; ++i) {
            value *= 10;
        }
        return value;
    }

    std::optional<int64_t> parse_number(const std::string& text, uint8_t precision, uint8_t scale) {
        if (text.empty()) {
            return std::nullopt;
        }

        std::size_t pos = 0;
        bool negative = false;
        if (text[pos] == '-') {
            negative = true;
            ++pos;
        }
        if (pos == text.size()) {
            return std::nullopt;
        }

        std::string whole;
        std::string fraction;
        bool after_decimal = false;
        for (; pos < text.size(); ++pos) {
            const char ch = text[pos];
            if (ch == '.') {
                if (after_decimal) {
                    return std::nullopt;
                }
                after_decimal = true;
                continue;
            }
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
            if (after_decimal) {
                fraction.push_back(ch);
            } else {
                whole.push_back(ch);
            }
        }

        while (whole.size() > 1 && whole.front() == '0') {
            whole.erase(whole.begin());
        }

        if (whole.empty()) {
            whole = "0";
        }
        if (fraction.size() > scale) {
            return std::nullopt;
        }

        const std::size_t whole_digits = whole == "0" ? 0 : whole.size();
        if (whole_digits + fraction.size() > precision || whole_digits > precision - scale) {
            return std::nullopt;
        }

        while (fraction.size() < scale) {
            fraction.push_back('0');
        }

        int64_t scaled = 0;
        for (char ch : whole + fraction) {
            scaled = scaled * 10 + (ch - '0');
        }
        return negative ? -scaled : scaled;
    }

    std::string format_number(int64_t scaled_value, uint8_t scale) {
        const bool negative = scaled_value < 0;
        const uint64_t value = negative ?
            static_cast<uint64_t>(-(scaled_value + 1)) + 1 :
            static_cast<uint64_t>(scaled_value);
        const uint64_t factor = static_cast<uint64_t>(power_of_10(scale));
        const uint64_t whole = scale == 0 ? value : value / factor;
        const uint64_t fraction = scale == 0 ? 0 : value % factor;

        std::ostringstream out;
        if (negative) {
            out << "-";
        }
        out << whole;
        if (scale > 0) {
            out << "." << std::setw(scale) << std::setfill('0') << fraction;
        }
        return out.str();
    }

    std::string type_name(const Column& column) {
        return Column::type_to_string(column.type());
    }
}

std::optional<Value> ValueParser::parse_value(const Column& column, const std::string& text) {
    ValueParseResult result = parse_value_with_error(column, text);
    return result.value;
}

ValueParseResult ValueParser::parse_value_with_error(const Column& column, const std::string& text, std::size_t base_position) {
    const std::string cleaned = trim(text);
    std::size_t local_start = 0;
    while (local_start < text.size() && std::isspace(static_cast<unsigned char>(text[local_start]))) {
        ++local_start;
    }
    const std::size_t position = base_position + local_start;

    if (cleaned == "NULL") {
        if (!column.nullable()) {
            return value_error("NULL is not allowed for column " + column.name(), cleaned, position);
        }
        return value_result(Value::null_value());
    }

    if (column.type() == ColumnType::integer) {
        int64_t value = 0;
        if (!parse_integer(cleaned, value)) {
            return value_error("invalid INTEGER value for column " + column.name(), cleaned, position);
        }
        return value_result(Value::integer_value(value));
    }

    if (column.type() == ColumnType::number) {
        std::optional<int64_t> value = parse_number(cleaned, column.precision(), column.scale());
        if (!value.has_value()) {
            return value_error("invalid NUMBER value for column " + column.name(), cleaned, position);
        }
        return value_result(Value::number_value(*value));
    }

    if (column.type() == ColumnType::character ||
        column.type() == ColumnType::string ||
        column.type() == ColumnType::varstring ||
        column.type() == ColumnType::date ||
        column.type() == ColumnType::time ||
        column.type() == ColumnType::datetime ||
        column.type() == ColumnType::text) {
        if (cleaned.size() < 2 || cleaned.front() != '\'' || cleaned.back() != '\'') {
            return value_error(type_name(column) + " value for column " + column.name() + " must be quoted", cleaned, position);
        }
        const std::string value = cleaned.substr(1, cleaned.size() - 2);
        if (column.type() == ColumnType::character) {
            if (value.size() != 1) {
                return value_error("CHAR value for column " + column.name() + " must be exactly one character", cleaned, position);
            }
            return value_result(Value::char_value(value[0]));
        }
        if (column.type() == ColumnType::string) {
            Value parsed = Value::string_value(value);
            if (!parsed.matches_column(column)) {
                return value_error("STRING value for column " + column.name() + " exceeds max size " + std::to_string(column.max_size()), cleaned, position);
            }
            return value_result(parsed);
        }
        if (column.type() == ColumnType::varstring) {
            Value parsed = Value::varstring_value(value);
            if (!parsed.matches_column(column)) {
                return value_error("VARSTRING value for column " + column.name() + " exceeds max size " + std::to_string(column.max_size()), cleaned, position);
            }
            return value_result(parsed);
        }
        if (column.type() == ColumnType::date) {
            std::optional<Date> date = Date::from_string(value);
            if (!date.has_value()) {
                return value_error("invalid DATE value for column " + column.name(), cleaned, position);
            }
            return value_result(Value::date_value(*date));
        }
        if (column.type() == ColumnType::time) {
            std::optional<Time> time = Time::from_string(value);
            if (!time.has_value()) {
                return value_error("invalid TIME value for column " + column.name(), cleaned, position);
            }
            return value_result(Value::time_value(*time));
        }
        if (column.type() == ColumnType::datetime) {
            std::optional<DateTime> datetime = DateTime::from_string(value);
            if (!datetime.has_value()) {
                return value_error("invalid DATETIME value for column " + column.name(), cleaned, position);
            }
            return value_result(Value::datetime_value(*datetime));
        }
        Value parsed = Value::text_value(value);
        if (!parsed.matches_column(column)) {
            return value_error("TEXT value for column " + column.name() + " exceeds max size " + std::to_string(column.max_size()), cleaned, position);
        }
        return value_result(parsed);
    }

    return value_error("unsupported value type for column " + column.name(), cleaned, position);
}

std::optional<Row> ValueParser::parse_row(const Schema& schema, const std::string& values_text) {
    ValueParseResult result = parse_row_with_error(schema, values_text);
    return result.row;
}

ValueParseResult ValueParser::parse_row_with_error(const Schema& schema, const std::string& values_text, std::size_t base_position) {
    const std::vector<TextPart> parts = split_csv_with_positions(values_text, base_position);
    if (parts.size() != schema.column_count()) {
        return value_error(
            "wrong value count: expected " + std::to_string(schema.column_count()) + ", got " + std::to_string(parts.size()),
            values_text,
            base_position
        );
    }

    std::vector<Value> values;
    values.reserve(parts.size());
    for (uint16_t i = 0; i < schema.column_count(); ++i) {
        const Column* column = schema.column(i);
        if (column == nullptr) {
            return value_error("invalid schema column", "", base_position);
        }

        ValueParseResult value = parse_value_with_error(*column, parts[i].text, parts[i].position);
        if (!value.ok() || !value.value.has_value()) {
            return value;
        }
        if (!value.value->matches_column(*column)) {
            return value_error("value does not match column " + column->name(), parts[i].text, parts[i].position);
        }
        values.push_back(*value.value);
    }

    return row_result(Row(values));
}

bool ValueParseResult::ok() const {
    return !error.has_value();
}

std::string ValueParser::format_value(const Column& column, const Value& value) {
    if (value.is_null()) {
        return "NULL";
    }
    if (value.type() == ColumnType::integer) {
        return std::to_string(value.integer_data());
    }
    if (value.type() == ColumnType::number) {
        return format_number(value.number_data(), column.scale());
    }
    if (value.type() == ColumnType::date) {
        return value.date_data().to_string();
    }
    if (value.type() == ColumnType::time) {
        return value.time_data().to_string();
    }
    if (value.type() == ColumnType::datetime) {
        return value.datetime_data().to_string();
    }
    return value.string_data();
}
