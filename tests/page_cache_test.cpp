#include "storage/page_cache.hpp"
#include "test_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> bytes(uint8_t seed, std::size_t count) {
    std::vector<uint8_t> data(count);
    for (std::size_t i = 0; i < count; ++i) {
        data[i] = static_cast<uint8_t>(seed + i);
    }
    return data;
}

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::path("/tmp") / ("scratch_db_" + name + ".tbl");
}

Page page_with_record(uint8_t seed) {
    Page page;
    uint16_t slot_id = 0;
    require(page.insert(bytes(seed, 32), slot_id), "page setup insert failed");
    return page;
}

void write_pages(const std::filesystem::path& path, const std::vector<Page>& pages) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "could not create table file");
    for (const Page& page : pages) {
        out.write(reinterpret_cast<const char*>(page.data()), BYTE_SIZES::PAGE_SIZE);
    }
    require(static_cast<bool>(out), "could not write table file");
}

bool first_record_matches(Page& page, uint8_t seed) {
    std::vector<uint8_t> out;
    return page.read(0, out) && out == bytes(seed, 32);
}

void cache_hit_and_miss() {
    const std::filesystem::path path = temp_path("page_cache_hit_miss");
    std::remove(path.c_str());
    write_pages(path, {page_with_record(1)});

    PageCache cache(2);
    Page* first = cache.fetch_page(path, 0);
    require(first != nullptr, "first fetch failed");
    require(cache.miss_count() == 1, "miss count wrong");
    require(cache.unpin_page(path, 0, false), "first unpin failed");

    Page* second = cache.fetch_page(path, 0);
    require(second != nullptr, "second fetch failed");
    require(cache.hit_count() == 1, "hit count wrong");
    require(cache.unpin_page(path, 0, false), "second unpin failed");

    std::remove(path.c_str());
}

void dirty_page_flushes() {
    const std::filesystem::path path = temp_path("page_cache_flush");
    std::remove(path.c_str());
    write_pages(path, {page_with_record(1)});

    {
        PageCache cache(2);
        Page* page = cache.fetch_page(path, 0);
        require(page != nullptr, "fetch failed");
        require(page->update(0, bytes(9, 32)), "page update failed");
        require(cache.unpin_page(path, 0, true), "dirty unpin failed");
        require(cache.flush_page(path, 0), "flush failed");
    }

    PageCache verifier(1);
    Page* page = verifier.fetch_page(path, 0);
    require(page != nullptr && first_record_matches(*page, 9), "flushed page not on disk");
    verifier.unpin_page(path, 0, false);

    std::remove(path.c_str());
}

void pinned_pages_are_not_evicted() {
    const std::filesystem::path path = temp_path("page_cache_pinned");
    std::remove(path.c_str());
    write_pages(path, {page_with_record(1), page_with_record(2), page_with_record(3)});

    PageCache cache(2);
    require(cache.fetch_page(path, 0) != nullptr, "fetch pinned page failed");
    require(cache.fetch_page(path, 1) != nullptr, "fetch second page failed");
    require(cache.unpin_page(path, 1, false), "unpin second page failed");

    require(cache.fetch_page(path, 2) != nullptr, "fetch third page failed");
    require(cache.contains_page(path, 0), "pinned page was evicted");
    require(!cache.contains_page(path, 1), "unpinned LRU page was not evicted");

    cache.unpin_page(path, 0, false);
    cache.unpin_page(path, 2, false);
    std::remove(path.c_str());
}

void eviction_writes_dirty_page() {
    const std::filesystem::path path = temp_path("page_cache_dirty_evict");
    std::remove(path.c_str());
    write_pages(path, {page_with_record(1), page_with_record(2)});

    {
        PageCache cache(1);
        Page* first = cache.fetch_page(path, 0);
        require(first != nullptr, "fetch first failed");
        require(first->update(0, bytes(7, 32)), "dirty update failed");
        require(cache.unpin_page(path, 0, true), "dirty unpin failed");

        Page* second = cache.fetch_page(path, 1);
        require(second != nullptr, "fetch second failed");
        require(!cache.contains_page(path, 0), "dirty page was not evicted");
        require(cache.unpin_page(path, 1, false), "second unpin failed");
    }

    PageCache verifier(1);
    Page* page = verifier.fetch_page(path, 0);
    require(page != nullptr && first_record_matches(*page, 7), "dirty evicted page was not written");
    verifier.unpin_page(path, 0, false);

    std::remove(path.c_str());
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"cache hit miss", cache_hit_and_miss});
    tests.push_back({"cache dirty flush", dirty_page_flushes});
    tests.push_back({"cache pinned eviction", pinned_pages_are_not_evicted});
    tests.push_back({"cache dirty eviction", eviction_writes_dirty_page});
    return run_tests(tests);
}
