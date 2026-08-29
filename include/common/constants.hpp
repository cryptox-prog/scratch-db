#pragma once

#include <cstdint>

namespace BYTE_SIZES {
    constexpr uint16_t PAGE_SIZE = 4096;
    constexpr uint16_t PAGE_HEADER_SIZE = 4;
    constexpr uint16_t SLOT_SIZE = 4;
    constexpr uint16_t MAX_SLOTS = (PAGE_SIZE - PAGE_HEADER_SIZE) / SLOT_SIZE;

    constexpr uint16_t SLOT_COUNT = 2;
    constexpr uint16_t OFFSET = 2;
    constexpr uint16_t LENGTH = 2;
}

namespace LIMITS {
    constexpr uint16_t MAX_COLUMNS = 991;
}
