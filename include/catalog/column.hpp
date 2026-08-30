#pragma once

#include <cstdint>
#include <string>

enum class ColumnType : uint8_t {
    null_type,
    integer,
    number,
    character,
    string,
    varstring,
    date,
    time,
    datetime,
    text,
};

class Column {
public:
    static constexpr uint16_t VARIABLE_SIZE = 0;
    static constexpr uint16_t INTEGER_SIZE = 8;
    static constexpr uint16_t NUMBER_SIZE = 8;
    static constexpr uint16_t CHAR_SIZE = 1;
    static constexpr uint16_t DATE_SIZE = 4;
    static constexpr uint16_t TIME_SIZE = 4;
    static constexpr uint16_t DATETIME_SIZE = 8;
    static constexpr uint16_t VAR_POINTER_SIZE = 4;
    static constexpr uint16_t STRING_MAX_SIZE = 1024;
    static constexpr uint16_t VARSTRING_MAX_SIZE = 1024;
    static constexpr uint16_t TEXT_MAX_SIZE = 2048;

    static Column integer_column(const std::string& name, bool nullable);
    static Column number_column(const std::string& name, bool nullable, uint8_t precision, uint8_t scale);
    static Column char_column(const std::string& name, bool nullable);
    static Column string_column(const std::string& name, bool nullable, uint16_t size);
    static Column varstring_column(const std::string& name, bool nullable, uint16_t max_size);
    static Column date_column(const std::string& name, bool nullable);
    static Column time_column(const std::string& name, bool nullable);
    static Column datetime_column(const std::string& name, bool nullable);
    static Column text_column(const std::string& name, bool nullable);
    static Column from_catalog(
        const std::string& name,
        ColumnType type,
        bool nullable,
        uint16_t max_size
    );
    static Column from_catalog(
        const std::string& name,
        ColumnType type,
        bool nullable,
        uint16_t max_size,
        uint8_t precision,
        uint8_t scale
    );

    const std::string& name() const;
    ColumnType type() const;
    bool nullable() const;
    uint16_t max_size() const;
    uint8_t precision() const;
    uint8_t scale() const;

    bool set_name(const std::string& name);
    void set_type(ColumnType type);
    void set_nullable(bool nullable);
    bool set_max_size(uint16_t max_size);
    bool set_number_format(uint8_t precision, uint8_t scale);

    bool is_valid() const;

    static std::string type_to_string(ColumnType type);
    static bool type_from_string(const std::string& text, ColumnType& type);
    static uint16_t fixed_size(ColumnType type);
    static bool is_valid_name(const std::string& name);

private:
    Column(std::string name, ColumnType type, bool nullable, uint16_t max_size, uint8_t precision = 0, uint8_t scale = 0);

    std::string name_;
    ColumnType type_ = ColumnType::integer;
    bool nullable_ = false;
    uint16_t max_size_ = INTEGER_SIZE;
    uint8_t precision_ = 0;
    uint8_t scale_ = 0;
};
