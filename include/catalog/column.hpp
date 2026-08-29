#pragma once

#include <cstdint>
#include <string>

enum class ColumnType : uint8_t {
    null_type,
    int32,
    text,
};

class Column {
public:
    static constexpr uint16_t VARIABLE_SIZE = 0;
    static constexpr uint16_t INT32_SIZE = 4;
    static constexpr uint16_t TEXT_MAX_SIZE = 1024;

    static Column int32_column(const std::string& name, bool nullable);
    static Column text_column(const std::string& name, bool nullable, uint16_t max_size);
    static Column from_catalog(
        const std::string& name,
        ColumnType type,
        bool nullable,
        uint16_t max_size
    );

    const std::string& name() const;
    ColumnType type() const;
    bool nullable() const;
    uint16_t max_size() const;

    bool set_name(const std::string& name);
    void set_type(ColumnType type);
    void set_nullable(bool nullable);
    bool set_max_size(uint16_t max_size);

    bool is_valid() const;

    static std::string type_to_string(ColumnType type);
    static bool type_from_string(const std::string& text, ColumnType& type);
    static uint16_t fixed_size(ColumnType type);
    static bool is_valid_name(const std::string& name);

private:
    Column(std::string name, ColumnType type, bool nullable, uint16_t max_size);

    std::string name_;
    ColumnType type_ = ColumnType::int32;
    bool nullable_ = false;
    uint16_t max_size_ = INT32_SIZE;
};
