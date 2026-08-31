#include "database/database.hpp"
#include "storage/wal_manager.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
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

Schema course_schema() {
    return Schema("course", {
        Column::integer_column("id", false),
        Column::varstring_column("title", false, 128),
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

void modifying_statements_use_wal_transactions() {
    const std::filesystem::path root = temp_path("statement_wal");
    std::filesystem::remove_all(root);

    {
        Database database(root, "school");
        require(database.create_database(), "create database failed");
        require(database.create_table(student_schema()), "create table failed");
        require(database.insert_row("student", Row({
            Value::integer_value(1),
            Value::varstring_value("alice"),
            Value::null_value(),
        })).has_value(), "insert row failed");
    }

    WalManager wal(root / "school" / "database.wal");
    require(wal.last_lsn() == 6, "statement transaction log count wrong");

    std::filesystem::remove_all(root);
}

void explicit_transaction_commit_persists_changes() {
    const std::filesystem::path root = temp_path("transaction_commit");
    std::filesystem::remove_all(root);

    Database database(root, "school");
    require(database.create_database(), "create database failed");
    require(database.create_table(student_schema()), "create table failed");

    require(database.begin_transaction(), "begin failed");
    std::optional<RecordId> record_id = database.insert_row("student", Row({
        Value::integer_value(1),
        Value::varstring_value("alice"),
        Value::null_value(),
    }));
    require(record_id.has_value(), "transaction insert failed");
    require(database.commit_transaction(), "commit failed");

    std::optional<Row> row = database.read_row("student", *record_id);
    require(row.has_value(), "committed row missing");
    require(row->value(1)->string_data() == "alice", "committed value wrong");

    std::filesystem::remove_all(root);
}

void explicit_transaction_rollback_undoes_changes() {
    const std::filesystem::path root = temp_path("transaction_rollback");
    std::filesystem::remove_all(root);

    Database database(root, "school");
    require(database.create_database(), "create database failed");
    require(database.create_table(student_schema()), "create table failed");

    require(database.begin_transaction(), "begin failed");
    std::optional<RecordId> record_id = database.insert_row("student", Row({
        Value::integer_value(2),
        Value::varstring_value("bob"),
        Value::null_value(),
    }));
    require(record_id.has_value(), "transaction insert failed");
    require(database.rollback_transaction(), "rollback failed");

    require(!database.read_row("student", *record_id).has_value(), "rolled back row survived");

    std::filesystem::remove_all(root);
}

void transaction_read_lock_blocks_writer_until_commit() {
    const std::filesystem::path root = temp_path("transaction_read_lock");
    std::filesystem::remove_all(root);

    Database setup(root, "school");
    require(setup.create_database(), "create database failed");
    require(setup.create_table(student_schema()), "create table failed");

    Database reader(root, "school");
    Database writer(root, "school");
    require(reader.begin_transaction(), "reader begin failed");
    require(reader.scan_rows("student").empty(), "reader scan failed");

    std::atomic<bool> writer_done = false;
    std::thread writer_thread([&]() {
        writer.insert_row("student", Row({
            Value::integer_value(3),
            Value::varstring_value("carol"),
            Value::null_value(),
        }));
        writer_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(!writer_done.load(), "writer was not blocked by transaction read lock");
    require(reader.commit_transaction(), "reader commit failed");
    writer_thread.join();
    require(writer_done.load(), "writer did not finish after commit");

    std::filesystem::remove_all(root);
}

void transaction_read_read_proceeds_together() {
    const std::filesystem::path root = temp_path("transaction_read_read");
    std::filesystem::remove_all(root);

    Database setup(root, "school");
    require(setup.create_database(), "create database failed");
    require(setup.create_table(student_schema()), "create table failed");

    Database first(root, "school");
    Database second(root, "school");
    require(first.begin_transaction(), "first begin failed");
    require(second.begin_transaction(), "second begin failed");

    std::atomic<bool> first_done = false;
    std::atomic<bool> second_done = false;
    std::thread first_thread([&]() {
        first.scan_rows("student");
        first_done = true;
    });
    std::thread second_thread([&]() {
        second.scan_rows("student");
        second_done = true;
    });

    first_thread.join();
    second_thread.join();
    require(first_done.load() && second_done.load(), "read/read did not proceed together");
    require(first.commit_transaction(), "first commit failed");
    require(second.commit_transaction(), "second commit failed");

    std::filesystem::remove_all(root);
}

void transaction_write_write_serializes() {
    const std::filesystem::path root = temp_path("transaction_write_write");
    std::filesystem::remove_all(root);

    Database setup(root, "school");
    require(setup.create_database(), "create database failed");
    require(setup.create_table(student_schema()), "create table failed");

    Database first(root, "school");
    Database second(root, "school");
    require(first.begin_transaction(), "first begin failed");
    require(first.insert_row("student", Row({
        Value::integer_value(1),
        Value::varstring_value("alice"),
        Value::null_value(),
    })).has_value(), "first insert failed");

    std::atomic<bool> second_done = false;
    std::thread second_thread([&]() {
        second.insert_row("student", Row({
            Value::integer_value(2),
            Value::varstring_value("bob"),
            Value::null_value(),
        }));
        second_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(!second_done.load(), "second writer was not serialized");
    require(first.commit_transaction(), "first commit failed");
    second_thread.join();
    require(second_done.load(), "second writer did not finish after first commit");

    Database check(root, "school");
    require(check.scan_rows("student").size() == 2, "serialized writers lost rows");

    std::filesystem::remove_all(root);
}

void transaction_read_cannot_see_uncommitted_insert_update_delete() {
    const std::filesystem::path root = temp_path("transaction_no_dirty_read");
    std::filesystem::remove_all(root);

    Database setup(root, "school");
    require(setup.create_database(), "create database failed");
    require(setup.create_table(student_schema()), "create table failed");
    std::optional<RecordId> record_id = setup.insert_row("student", Row({
        Value::integer_value(1),
        Value::varstring_value("alice"),
        Value::null_value(),
    }));
    require(record_id.has_value(), "setup insert failed");

    Database writer(root, "school");
    Database reader(root, "school");
    require(writer.begin_transaction(), "insert begin failed");
    require(writer.insert_row("student", Row({
        Value::integer_value(2),
        Value::varstring_value("bob"),
        Value::null_value(),
    })).has_value(), "uncommitted insert failed");

    std::atomic<bool> reader_done = false;
    std::thread insert_reader([&]() {
        reader.scan_rows("student");
        reader_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(!reader_done.load(), "reader saw or passed uncommitted insert");
    require(writer.rollback_transaction(), "insert rollback failed");
    insert_reader.join();
    require(reader.scan_rows("student").size() == 1, "rolled back insert changed committed view");

    require(writer.begin_transaction(), "update begin failed");
    RecordId updated_id = *record_id;
    require(writer.update_row("student", updated_id, Row({
        Value::integer_value(1),
        Value::varstring_value("updated"),
        Value::null_value(),
    })), "uncommitted update failed");
    reader_done = false;
    std::thread update_reader([&]() {
        reader.read_row("student", *record_id);
        reader_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(!reader_done.load(), "reader saw or passed uncommitted update");
    require(writer.rollback_transaction(), "update rollback failed");
    update_reader.join();
    std::optional<Row> row = reader.read_row("student", *record_id);
    require(row.has_value() && row->value(1)->string_data() == "alice", "rolled back update changed committed row");

    require(writer.begin_transaction(), "delete begin failed");
    require(writer.delete_row("student", *record_id), "uncommitted delete failed");
    reader_done = false;
    std::thread delete_reader([&]() {
        reader.read_row("student", *record_id);
        reader_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(!reader_done.load(), "reader saw or passed uncommitted delete");
    require(writer.rollback_transaction(), "delete rollback failed");
    delete_reader.join();
    require(reader.read_row("student", *record_id).has_value(), "rolled back delete removed committed row");

    std::filesystem::remove_all(root);
}

void rollback_does_not_affect_committed_work() {
    const std::filesystem::path root = temp_path("transaction_rollback_keeps_committed");
    std::filesystem::remove_all(root);

    Database database(root, "school");
    require(database.create_database(), "create database failed");
    require(database.create_table(student_schema()), "create table failed");
    std::optional<RecordId> committed = database.insert_row("student", Row({
        Value::integer_value(1),
        Value::varstring_value("alice"),
        Value::null_value(),
    }));
    require(committed.has_value(), "committed insert failed");

    require(database.begin_transaction(), "begin failed");
    require(database.insert_row("student", Row({
        Value::integer_value(2),
        Value::varstring_value("bob"),
        Value::null_value(),
    })).has_value(), "transaction insert failed");
    require(database.rollback_transaction(), "rollback failed");

    std::vector<TableRow> rows = database.scan_rows("student");
    require(rows.size() == 1, "rollback affected committed row count");
    require(rows[0].row.value(1)->string_data() == "alice", "rollback affected committed value");

    std::filesystem::remove_all(root);
}

void recovery_undoes_uncommitted_explicit_transaction() {
    const std::filesystem::path root = temp_path("transaction_crash_recovery");
    std::filesystem::remove_all(root);
    RecordId committed_id;

    {
        Database database(root, "school");
        require(database.create_database(), "create database failed");
        require(database.create_table(student_schema()), "create table failed");
        std::optional<RecordId> committed = database.insert_row("student", Row({
            Value::integer_value(1),
            Value::varstring_value("alice"),
            Value::null_value(),
        }));
        require(committed.has_value(), "committed insert failed");
        committed_id = *committed;

        require(database.begin_transaction(), "begin failed");
        require(database.insert_row("student", Row({
            Value::integer_value(2),
            Value::varstring_value("bob"),
            Value::null_value(),
        })).has_value(), "uncommitted insert failed");
    }

    Database recovered(root, "school");
    std::vector<TableRow> rows = recovered.scan_rows("student");
    require(rows.size() == 1, "recovery did not undo uncommitted transaction");
    require(rows[0].record_id.page_id == committed_id.page_id && rows[0].record_id.slot_id == committed_id.slot_id, "recovery lost committed row");
    require(rows[0].row.value(1)->string_data() == "alice", "recovery changed committed row");

    std::filesystem::remove_all(root);
}

void deadlock_schedule_times_out_cleanly() {
    const std::filesystem::path root = temp_path("transaction_deadlock_timeout");
    std::filesystem::remove_all(root);

    Database setup(root, "school");
    require(setup.create_database(), "create database failed");
    require(setup.create_table(student_schema()), "create student failed");
    require(setup.create_table(course_schema()), "create course failed");

    Database first(root, "school");
    Database second(root, "school");
    require(first.begin_transaction(), "first begin failed");
    require(second.begin_transaction(), "second begin failed");
    require(first.insert_row("student", Row({
        Value::integer_value(1),
        Value::varstring_value("alice"),
        Value::null_value(),
    })).has_value(), "first initial insert failed");
    require(second.insert_row("course", Row({
        Value::integer_value(1),
        Value::varstring_value("db"),
    })).has_value(), "second initial insert failed");

    std::atomic<bool> first_done = false;
    std::atomic<bool> second_done = false;
    std::atomic<bool> first_succeeded = false;
    std::atomic<bool> second_succeeded = false;

    std::thread first_thread([&]() {
        first_succeeded = first.insert_row("course", Row({
            Value::integer_value(2),
            Value::varstring_value("systems"),
        })).has_value();
        first_done = true;
    });
    std::thread second_thread([&]() {
        second_succeeded = second.insert_row("student", Row({
            Value::integer_value(2),
            Value::varstring_value("bob"),
            Value::null_value(),
        })).has_value();
        second_done = true;
    });

    first_thread.join();
    second_thread.join();
    require(first_done.load() && second_done.load(), "deadlock schedule did not return");
    require(!first_succeeded.load() || !second_succeeded.load(), "deadlock schedule did not force a timeout");
    first.rollback_transaction();
    second.rollback_transaction();

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
    tests.push_back({"database statement wal", modifying_statements_use_wal_transactions});
    tests.push_back({"database transaction commit", explicit_transaction_commit_persists_changes});
    tests.push_back({"database transaction rollback", explicit_transaction_rollback_undoes_changes});
    tests.push_back({"database transaction locks", transaction_read_lock_blocks_writer_until_commit});
    tests.push_back({"database read/read transactions", transaction_read_read_proceeds_together});
    tests.push_back({"database write/write transactions", transaction_write_write_serializes});
    tests.push_back({"database no dirty reads", transaction_read_cannot_see_uncommitted_insert_update_delete});
    tests.push_back({"database rollback isolation", rollback_does_not_affect_committed_work});
    tests.push_back({"database recovery undo tx", recovery_undoes_uncommitted_explicit_transaction});
    tests.push_back({"database deadlock timeout", deadlock_schedule_times_out_cleanly});

    return run_tests(tests);
}
