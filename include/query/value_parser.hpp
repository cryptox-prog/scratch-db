#pragma once

#include <optional>
#include <string>

#include "catalog/column.hpp"
#include "catalog/schema.hpp"
#include "record/row.hpp"
#include "record/value.hpp"

class ValueParser {
public:
    static std::optional<Value> parse_value(const Column& column, const std::string& text);
    static std::optional<Row> parse_row(const Schema& schema, const std::string& values_text);
    static std::string format_value(const Column& column, const Value& value);
};
