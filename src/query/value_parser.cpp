#include "query/value_parser.hpp"

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
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

    std::vector<std::string> split_csv(const std::string& text) {
        std::vector<std::string> parts;
        std::string current;
        bool in_string = false;
        uint16_t paren_depth = 0;

        for (char ch : text) {
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
                parts.push_back(trim(current));
                current.clear();
            } else {
                current.push_back(ch);
            }
        }

        if (!current.empty() || !text.empty()) {
            parts.push_back(trim(current));
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
}

std::optional<Value> ValueParser::parse_value(const Column& column, const std::string& text) {
    const std::string cleaned = trim(text);
    if (cleaned == "NULL") {
        return Value::null_value();
    }

    if (column.type() == ColumnType::integer) {
        int64_t value = 0;
        if (!parse_integer(cleaned, value)) {
            return std::nullopt;
        }
        return Value::integer_value(value);
    }

    if (column.type() == ColumnType::number) {
        std::optional<int64_t> value = parse_number(cleaned, column.precision(), column.scale());
        if (!value.has_value()) {
            return std::nullopt;
        }
        return Value::number_value(*value);
    }

    if (column.type() == ColumnType::character ||
        column.type() == ColumnType::string ||
        column.type() == ColumnType::varstring ||
        column.type() == ColumnType::date ||
        column.type() == ColumnType::time ||
        column.type() == ColumnType::datetime ||
        column.type() == ColumnType::text) {
        if (cleaned.size() < 2 || cleaned.front() != '\'' || cleaned.back() != '\'') {
            return std::nullopt;
        }
        const std::string value = cleaned.substr(1, cleaned.size() - 2);
        if (column.type() == ColumnType::character) {
            if (value.size() != 1) {
                return std::nullopt;
            }
            return Value::char_value(value[0]);
        }
        if (column.type() == ColumnType::string) {
            return Value::string_value(value);
        }
        if (column.type() == ColumnType::varstring) {
            return Value::varstring_value(value);
        }
        if (column.type() == ColumnType::date) {
            std::optional<Date> date = Date::from_string(value);
            if (!date.has_value()) {
                return std::nullopt;
            }
            return Value::date_value(*date);
        }
        if (column.type() == ColumnType::time) {
            std::optional<Time> time = Time::from_string(value);
            if (!time.has_value()) {
                return std::nullopt;
            }
            return Value::time_value(*time);
        }
        if (column.type() == ColumnType::datetime) {
            std::optional<DateTime> datetime = DateTime::from_string(value);
            if (!datetime.has_value()) {
                return std::nullopt;
            }
            return Value::datetime_value(*datetime);
        }
        return Value::text_value(value);
    }

    return std::nullopt;
}

std::optional<Row> ValueParser::parse_row(const Schema& schema, const std::string& values_text) {
    const std::vector<std::string> parts = split_csv(values_text);
    if (parts.size() != schema.column_count()) {
        return std::nullopt;
    }

    std::vector<Value> values;
    values.reserve(parts.size());
    for (uint16_t i = 0; i < schema.column_count(); ++i) {
        const Column* column = schema.column(i);
        if (column == nullptr) {
            return std::nullopt;
        }

        std::optional<Value> value = parse_value(*column, parts[i]);
        if (!value.has_value() || !value->matches_column(*column)) {
            return std::nullopt;
        }
        values.push_back(*value);
    }

    return Row(values);
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
