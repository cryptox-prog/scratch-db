#pragma once

#include <cstdint>
#include <string>

#include "catalog/column.hpp"

class Value {
public:
    static Value null_value();
    static Value int32_value(int32_t value);
    static Value text_value(const std::string& value);

    bool is_null() const;
    ColumnType type() const;
    int32_t int32_data() const;
    const std::string& text_data() const;

    bool matches_column(const Column& column) const;

private:
    Value() = default;

    bool is_null_ = true;
    ColumnType type_ = ColumnType::null_type;
    int32_t int32_data_ = 0;
    std::string text_data_;
};
