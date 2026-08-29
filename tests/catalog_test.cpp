#include "catalog/catalog.hpp"
#include "common/constants.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::path("/tmp") / ("scratch_db_catalog_" + name);
}

template <typename Fn>
bool throws_invalid_argument(Fn fn) {
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    }

    return false;
}

void column_type_round_trip() {
    ColumnType type = ColumnType::text;
    require(Column::type_to_string(ColumnType::int32) == "int32", "bad int32 text");
    require(Column::type_to_string(ColumnType::text) == "text", "bad text text");
    require(Column::type_to_string(ColumnType::null_type) == "null", "bad null text");
    require(Column::type_from_string("int32", type) && type == ColumnType::int32, "bad int32 parse");
    require(Column::type_from_string("text", type) && type == ColumnType::text, "bad text parse");
    require(Column::type_from_string("null", type) && type == ColumnType::null_type, "bad null parse");
    require(!Column::type_from_string("float", type), "invalid type parsed");
}

void column_domain_sizes() {
    Column id = Column::int32_column("id", false);
    require(id.type() == ColumnType::int32, "int32 factory used wrong type");
    require(id.max_size() == Column::INT32_SIZE, "int32 size is not fixed size");
    require(Column::fixed_size(ColumnType::int32) == Column::INT32_SIZE, "bad int32 fixed size");
    require(Column::fixed_size(ColumnType::text) == Column::VARIABLE_SIZE, "text should be variable size");

    Column name = Column::text_column("name", true, 128);
    require(name.type() == ColumnType::text, "text factory used wrong type");
    require(name.max_size() == 128, "text size was not preserved");
    require(Column::text_column("bio", true, Column::TEXT_MAX_SIZE).max_size() == Column::TEXT_MAX_SIZE, "text max rejected");

    require(throws_invalid_argument([]() {
        Column::from_catalog("id", ColumnType::int32, false, 8);
    }), "catalog accepted bad int32 size");

    require(throws_invalid_argument([]() {
        Column::from_catalog("nothing", ColumnType::null_type, true, 0);
    }), "catalog accepted null column type");

    require(throws_invalid_argument([]() {
        Column::text_column("bio", true, static_cast<uint16_t>(Column::TEXT_MAX_SIZE + 1));
    }), "text above max accepted");

    require(throws_invalid_argument([]() {
        Column::from_catalog("bio", ColumnType::text, true, static_cast<uint16_t>(Column::TEXT_MAX_SIZE + 1));
    }), "catalog accepted text above max");
}

void column_validation() {
    require(Column::is_valid_name("id"), "simple name rejected");
    require(Column::is_valid_name("_internal_1"), "underscore name rejected");
    require(!Column::is_valid_name("first name"), "space accepted");
    require(!Column::is_valid_name("9id"), "digit-first accepted");
    require(!Column::is_valid_name("user-id"), "special char accepted");

    Column column = Column::int32_column("age", false);
    require(column.set_name("age"), "valid setter name rejected");
    require(!column.set_name("bad name"), "invalid setter name accepted");
    require(column.name() == "age", "invalid setter changed name");
    require(!column.set_max_size(0), "zero max size accepted");
    require(!column.set_max_size(8), "bad int32 max size accepted");
    require(column.set_max_size(Column::INT32_SIZE), "valid max size rejected");
    require(column.is_valid(), "valid column rejected");

    require(throws_invalid_argument([]() {
        Column::text_column("bad name", true, 20);
    }), "constructor accepted bad name");

    require(throws_invalid_argument([]() {
        Column::text_column("name", true, 0);
    }), "constructor accepted zero max_size");
}

void schema_lookup_and_validation() {
    Schema schema("student", {
        Column::int32_column("id", false),
        Column::text_column("name", false, 128),
    });

    require(schema.is_valid(), "schema should be valid");
    require(schema.column_count() == 2, "bad column count");
    require(schema.column(0) != nullptr && schema.column(0)->name() == "id", "bad column by index");
    require(schema.find_column("name") != nullptr, "column lookup failed");
    require(schema.column_index("missing") == -1, "missing column found");

    require(Schema::is_valid_table_name("student"), "valid table name rejected");
    require(!Schema::is_valid_table_name("student table"), "space in table name accepted");
    require(!Schema::is_valid_table_name("9student"), "digit-first table name accepted");

    require(throws_invalid_argument([]() {
        Schema("bad", {
            Column::int32_column("id", false),
            Column::text_column("id", true, 10),
        });
    }), "duplicate column accepted");

    require(throws_invalid_argument([]() {
        Schema("bad table", {
            Column::int32_column("id", false),
        });
    }), "bad table name accepted");

    require(throws_invalid_argument([]() {
        Column::text_column("first name", true, 20);
    }), "bad column name accepted");
}

void schema_column_limit() {
    std::vector<Column> columns;
    columns.reserve(static_cast<std::size_t>(LIMITS::MAX_COLUMNS) + 1);

    for (uint16_t i = 0; i <= LIMITS::MAX_COLUMNS; ++i) {
        columns.push_back(Column::int32_column("c_" + std::to_string(i), true));
    }

    require(throws_invalid_argument([&columns]() {
        Schema("too_wide", columns);
    }), "schema accepted too many columns");

    columns.pop_back();
    Schema schema("wide", columns);
    require(schema.column_count() == LIMITS::MAX_COLUMNS, "schema rejected max column count");
}

void catalog_create_and_load() {
    const std::filesystem::path root = temp_path("create_and_load");
    std::filesystem::remove_all(root);

    Catalog catalog(root);
    require(catalog.create_database("school"), "create database failed");
    require(catalog.database_exists("school"), "database missing");
    require(!catalog.create_database("bad db"), "bad database name accepted");

    Schema schema("student", {
        Column::int32_column("id", false),
        Column::text_column("name", false, 128),
    });

    require(catalog.is_valid_new_table_name("school", "student"), "valid new table name rejected");
    require(!catalog.is_valid_new_table_name("school", "bad table"), "bad new table name accepted");
    require(catalog.create_table("school", schema), "create table failed");
    require(!catalog.is_valid_new_table_name("school", "student"), "existing table name accepted");
    require(!catalog.create_table("school", schema), "duplicate table accepted");
    require(catalog.table_exists("school", "student"), "table missing");
    require(std::filesystem::is_regular_file(catalog.table_file_path("school", "student")), "table file missing");

    std::optional<Schema> loaded = catalog.load_schema("school", "student");
    require(loaded.has_value(), "load schema failed");
    require(loaded->table_name() == "student", "table name mismatch");
    require(loaded->column_count() == 2, "loaded column count mismatch");
    require(loaded->find_column("name") != nullptr, "loaded column missing");

    std::filesystem::remove_all(root);
}

void catalog_lists_names() {
    const std::filesystem::path root = temp_path("lists");
    std::filesystem::remove_all(root);

    Catalog catalog(root);
    catalog.create_database("b_db");
    catalog.create_database("a_db");

    const std::vector<std::string> databases = catalog.list_databases();
    require(databases.size() == 2, "bad database count");
    require(databases[0] == "a_db" && databases[1] == "b_db", "databases not sorted");

    Schema schema("people", {
        Column::int32_column("id", false),
    });
    require(catalog.create_table("a_db", schema), "create table failed");

    const std::vector<std::string> tables = catalog.list_tables("a_db");
    require(tables.size() == 1 && tables[0] == "people", "bad table list");

    std::filesystem::remove_all(root);
}

}  // namespace

void add_catalog_tests(std::vector<TestCase>& tests) {
    tests.push_back({"column types", column_type_round_trip});
    tests.push_back({"column sizes", column_domain_sizes});
    tests.push_back({"column validation", column_validation});
    tests.push_back({"schema lookup", schema_lookup_and_validation});
    tests.push_back({"schema column limit", schema_column_limit});
    tests.push_back({"catalog create/load", catalog_create_and_load});
    tests.push_back({"catalog lists", catalog_lists_names});
}

int main() {
    std::vector<TestCase> tests;
    add_catalog_tests(tests);
    return run_tests(tests);
}
