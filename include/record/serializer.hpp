#pragma once

#include <cstdint>
#include <vector>

#include "catalog/schema.hpp"
#include "record/row.hpp"

class RecordSerializer {
public:
    static bool serialize(const Schema& schema, const Row& row, std::vector<uint8_t>& record);
    static bool deserialize(const Schema& schema, const std::vector<uint8_t>& record, Row& row);

private:
    static uint16_t null_bitmap_size(uint16_t column_count);
};
