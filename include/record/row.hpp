#pragma once

#include <cstdint>
#include <vector>

#include "record/value.hpp"

class Row {
public:
    Row() = default;
    explicit Row(std::vector<Value> values);

    const std::vector<Value>& values() const;
    uint16_t value_count() const;
    const Value* value(uint16_t value_index) const;

private:
    std::vector<Value> values_;
};
