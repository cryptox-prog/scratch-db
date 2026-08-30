#pragma once

#include <optional>
#include <string>

#include "catalog/column.hpp"
#include "catalog/schema.hpp"
#include "record/row.hpp"
#include "record/value.hpp"

struct ValueParseError {
    std::string message;
    std::string token;
    std::size_t position = 0;
};

struct ValueParseResult {
    std::optional<Value> value;
    std::optional<Row> row;
    std::optional<ValueParseError> error;

    bool ok() const;
};

class ValueParser {
public:
    static std::optional<Value> parse_value(const Column& column, const std::string& text);
    static ValueParseResult parse_value_with_error(const Column& column, const std::string& text, std::size_t base_position = 0);
    static std::optional<Row> parse_row(const Schema& schema, const std::string& values_text);
    static ValueParseResult parse_row_with_error(const Schema& schema, const std::string& values_text, std::size_t base_position = 0);
    static std::string format_value(const Column& column, const Value& value);
};
