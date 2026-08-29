#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "common/types.hpp"
#include "storage/page.hpp"

class TableFile {
public:
    explicit TableFile(const std::filesystem::path& path);

    uint32_t page_count() const;

    bool read_page(uint32_t page_id, Page& page) const;
    bool write_page(uint32_t page_id, const Page& page);

    RecordId insert_record(const std::vector<uint8_t>& record);
    bool read_record(RecordId record_id, std::vector<uint8_t>& record) const;
    bool delete_record(RecordId record_id);
    bool update_record(RecordId& record_id, const std::vector<uint8_t>& record);

private:
    bool append_page(const Page& page);

    std::filesystem::path path_;
};
