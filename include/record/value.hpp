#pragma once

#include <cstdint>
#include <string>

#include "catalog/column.hpp"
#include "db_types/date_time.hpp"

class Value {
public:
    static Value null_value();
    static Value integer_value(int64_t value);
    static Value number_value(int64_t scaled_value);
    static Value char_value(char value);
    static Value string_value(const std::string& value);
    static Value varstring_value(const std::string& value);
    static Value date_value(Date value);
    static Value time_value(Time value);
    static Value datetime_value(DateTime value);
    static Value text_value(const std::string& value);

    bool is_null() const;
    ColumnType type() const;
    int64_t integer_data() const;
    int64_t number_data() const;
    const std::string& string_data() const;
    Date date_data() const;
    Time time_data() const;
    DateTime datetime_data() const;

    bool matches_column(const Column& column) const;

private:
    Value() = default;

    bool is_null_ = true;
    ColumnType type_ = ColumnType::null_type;
    int64_t integer_data_ = 0;
    int64_t number_data_ = 0;
    std::string string_data_;
    Date date_data_ = Date(0);
    Time time_data_ = Time(0);
    DateTime datetime_data_ = DateTime(0);
};
