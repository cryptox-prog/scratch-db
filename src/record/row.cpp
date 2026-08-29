#include "record/row.hpp"

#include <utility>

/// @brief Create a row from values in schema order
/// @param values Values that belong to the row
Row::Row(std::vector<Value> values) : values_(std::move(values)) {}

/// @brief Get all values in the row
/// @return Constant vector of row values
const std::vector<Value>& Row::values() const {
    return values_;
}

/// @brief Get the number of values in the row
/// @return Value count
uint16_t Row::value_count() const {
    return static_cast<uint16_t>(values_.size());
}

/// @brief Get a value by index
/// @param value_index Index of the value in the row
/// @return Pointer to the value, or nullptr if index is invalid
const Value* Row::value(uint16_t value_index) const {
    if (value_index >= values_.size()) {
        return nullptr;
    }

    return &values_[value_index];
}
