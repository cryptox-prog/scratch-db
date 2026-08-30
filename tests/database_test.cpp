#include "database/database.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::path("/tmp") / ("scratch_db_database_" + name);
}

Schema student_schema() {
    return Schema("student", {
        Column::integer_column("id", false),
        Column::varstring_column("name", false, 128),
        Column::varstring_column("nickname", true, 64),
    });
}

void create_database_and_table() {
    const std::filesystem::path root = temp_path("create");
    std::filesystem::remove_all(root);

    Database database(root, "school");
    require(!database.database_exists(), "database existed before create");
    require(database.create_database(), "create database failed");
    require(database.database_exists(), "database missing after create");
    require(database.create_table(student_schema()), "create table failed");
    require(database.table_exists("student"), "table missing after create");

    std::optional<Schema> schema = database.load_schema("student");
    require(schema.has_value(), "schema load failed");
    require(schema->column_count() == 3, "bad schema column count");

    std::filesystem::remove_all(root);
}

void insert_and_read_row() {
    const std::filesystem::path root = temp_path("insert_read");
    std::filesystem::remove_all(root);

    Database database(root, "school");
    require(database.create_database(), "create database failed");
    require(database.create_table(student_schema()), "create table failed");

    std::optional<RecordId> record_id = database.insert_row("student", Row({
        Value::integer_value(1),
        Value::varstring_value("alice"),
        Value::null_value(),
    }));
    require(record_id.has_value(), "insert row failed");

    std::optional<Row> row = database.read_row("student", *record_id);
    require(row.has_value(), "read row failed");
    require(row->value(0)->integer_data() == 1, "bad id value");
    require(row->value(1)->string_data() == "alice", "bad name value");
    require(row->value(2)->is_null(), "bad nullable value");

    std::filesystem::remove_all(root);
}

void update_and_delete_row() {
    const std::filesystem::path root = temp_path("update_delete");
    std::filesystem::remove_all(root);

    Database database(root, "school");
    require(database.create_database(), "create database failed");
    require(database.create_table(student_schema()), "create table failed");

    std::optional<RecordId> inserted = database.insert_row("student", Row({
        Value::integer_value(2),
        Value::varstring_value("bob"),
        Value::varstring_value("bobby"),
    }));
    require(inserted.has_value(), "insert row failed");

    RecordId record_id = *inserted;
    require(database.update_row("student", record_id, Row({
        Value::integer_value(2),
        Value::varstring_value("robert"),
        Value::null_value(),
    })), "update row failed");

    std::optional<Row> row = database.read_row("student", record_id);
    require(row.has_value(), "read updated row failed");
    require(row->value(1)->string_data() == "robert", "updated value missing");
    require(row->value(2)->is_null(), "updated null missing");

    require(database.delete_row("student", record_id), "delete row failed");
    require(!database.read_row("student", record_id).has_value(), "deleted row was readable");

    std::filesystem::remove_all(root);
}

void scan_rows() {
    const std::filesystem::path root = temp_path("scan");
    std::filesystem::remove_all(root);

    Database database(root, "school");
    require(database.create_database(), "create database failed");
    require(database.create_table(student_schema()), "create table failed");

    std::optional<RecordId> first = database.insert_row("student", Row({
        Value::integer_value(1),
        Value::varstring_value("alice"),
        Value::null_value(),
    }));
    std::optional<RecordId> second = database.insert_row("student", Row({
        Value::integer_value(2),
        Value::varstring_value("bob"),
        Value::varstring_value("bobby"),
    }));
    require(first.has_value() && second.has_value(), "insert rows failed");

    const std::vector<TableRow> rows = database.scan_rows("student");
    require(rows.size() == 2, "scan returned wrong row count");
    require(rows[0].record_id.page_id == first->page_id && rows[0].record_id.slot_id == first->slot_id, "first scan id wrong");
    require(rows[0].row.value(1)->string_data() == "alice", "first scan row wrong");
    require(rows[1].record_id.page_id == second->page_id && rows[1].record_id.slot_id == second->slot_id, "second scan id wrong");
    require(rows[1].row.value(1)->string_data() == "bob", "second scan row wrong");

    std::filesystem::remove_all(root);
}

void reject_bad_row() {
    const std::filesystem::path root = temp_path("bad_row");
    std::filesystem::remove_all(root);

    Database database(root, "school");
    require(database.create_database(), "create database failed");
    require(database.create_table(student_schema()), "create table failed");

    require(!database.insert_row("student", Row({
        Value::varstring_value("wrong"),
        Value::varstring_value("alice"),
        Value::null_value(),
    })).has_value(), "bad row inserted");

    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"database create", create_database_and_table});
    tests.push_back({"database insert/read", insert_and_read_row});
    tests.push_back({"database update/delete", update_and_delete_row});
    tests.push_back({"database scan", scan_rows});
    tests.push_back({"database bad row", reject_bad_row});

    return run_tests(tests);
}
