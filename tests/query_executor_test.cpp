#include "query/query_executor.hpp"
#include "test_utils.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

std::filesystem::path test_root(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("scratch_db_" + name);
}

void execute_insert_and_select() {
    const std::filesystem::path root = test_root("query_executor_insert_select");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    QueryResult result = executor.execute("CREATE DATABASE shop");
    require(result.ok(), "create database failed");
    require(result.metadata.message == "database created", "create database message missing");

    result = executor.execute("CREATE TABLE items (id INTEGER NOT NULL, price NUMBER(4, 2) NOT NULL, name VARSTRING(16) NULL)");
    require(result.ok(), "create table failed");
    require(result.metadata.message == "table created", "create table message missing");

    result = executor.execute("INSERT INTO items VALUES (1, 12.34, 'pen')");
    require(result.ok(), "insert failed");
    require(result.metadata.row_count == 1, "insert row count wrong");

    result = executor.execute("SELECT * FROM items");
    require(result.ok(), "select failed");
    require(result.columns.size() == 3, "select column count wrong");
    require(result.columns[0].name == "id", "id column missing");
    require(result.columns[1].name == "price", "price column missing");
    require(result.rows.size() == 1, "select row count wrong");
    require(result.rows[0][0] == "1", "select id wrong");
    require(result.rows[0][1] == "12.34", "select price wrong");
    require(result.rows[0][2] == "pen", "select name wrong");

    std::filesystem::remove_all(root);
}

void execute_exit_returns_false() {
    QueryExecutor executor(test_root("query_executor_exit"));
    require(executor.execute("EXIT").should_exit, "exit did not stop executor");
}

void invalid_query_returns_error() {
    QueryExecutor executor(test_root("query_executor_invalid"));
    QueryResult result = executor.execute("select * from items");
    require(!result.ok(), "invalid query accepted");
    require(result.error->message == "keyword must be uppercase", "invalid query message wrong");
    require(result.error->token == "select", "invalid query token missing");
    require(result.error->position == 0, "invalid query position wrong");
}

void duplicate_database_returns_specific_error() {
    const std::filesystem::path root = test_root("query_executor_duplicate_database");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    QueryResult result = executor.execute("CREATE DATABASE db");
    require(result.ok(), "initial create database failed");

    result = executor.execute("CREATE DATABASE db");
    require(!result.ok(), "duplicate database accepted");
    require(result.error->message == "database already exists", "duplicate database message wrong");
    require(result.error->token == "db", "duplicate database token wrong");

    std::filesystem::remove_all(root);
}

void duplicate_table_returns_specific_error() {
    const std::filesystem::path root = test_root("query_executor_duplicate_table");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    QueryResult result = executor.execute("CREATE TABLE student (id INTEGER NOT NULL)");
    require(result.ok(), "initial create table failed");

    result = executor.execute("CREATE TABLE student (id INTEGER NOT NULL)");
    require(!result.ok(), "duplicate table accepted");
    require(result.error->message == "table already exists", "duplicate table message wrong");
    require(result.error->token == "student", "duplicate table token wrong");

    std::filesystem::remove_all(root);
}

void select_where_filters_rows() {
    const std::filesystem::path root = test_root("query_executor_select_where");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, price NUMBER(4, 2) NOT NULL, name VARSTRING(16) NOT NULL, sold_on DATE NOT NULL)").ok(), "create table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 10.00, 'pen', '2026-08-30')").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 15.50, 'book', '2026-09-01')").ok(), "insert 2 failed");

    QueryResult result = executor.execute("SELECT * FROM items WHERE price > 10.00");
    require(result.ok(), "select price where failed");
    require(result.rows.size() == 1, "select price where row count wrong");
    require(result.rows[0][0] == "2", "select price where wrong row");

    result = executor.execute("SELECT * FROM items WHERE sold_on <= '2026-08-30'");
    require(result.ok(), "select date where failed");
    require(result.rows.size() == 1, "select date where row count wrong");
    require(result.rows[0][2] == "pen", "select date where wrong row");

    result = executor.execute("SELECT * FROM items WHERE name != 'pen'");
    require(result.ok(), "select string where failed");
    require(result.rows.size() == 1, "select string where row count wrong");
    require(result.rows[0][2] == "book", "select string where wrong row");

    std::filesystem::remove_all(root);
}

void delete_where_removes_matching_rows() {
    const std::filesystem::path root = test_root("query_executor_delete_where");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 'pen')").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 'book')").ok(), "insert 2 failed");

    QueryResult result = executor.execute("DELETE FROM items WHERE id >= 2");
    require(result.ok(), "delete where failed");
    require(result.metadata.row_count == 1, "delete where count wrong");

    result = executor.execute("SELECT * FROM items");
    require(result.ok(), "select after delete failed");
    require(result.rows.size() == 1, "delete did not remove one row");
    require(result.rows[0][0] == "1", "wrong row remained after delete");

    std::filesystem::remove_all(root);
}

void update_where_changes_matching_rows() {
    const std::filesystem::path root = test_root("query_executor_update_where");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 'pen')").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 'book')").ok(), "insert 2 failed");

    QueryResult result = executor.execute("UPDATE items SET VALUES (3, 'pencil') WHERE name = 'pen'");
    require(result.ok(), "update where failed");
    require(result.metadata.row_count == 1, "update where count wrong");

    result = executor.execute("SELECT * FROM items WHERE id = 3");
    require(result.ok(), "select after update failed");
    require(result.rows.size() == 1, "updated row missing");
    require(result.rows[0][1] == "pencil", "updated value wrong");

    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"executor insert select", execute_insert_and_select});
    tests.push_back({"executor exit", execute_exit_returns_false});
    tests.push_back({"executor invalid query", invalid_query_returns_error});
    tests.push_back({"executor duplicate database", duplicate_database_returns_specific_error});
    tests.push_back({"executor duplicate table", duplicate_table_returns_specific_error});
    tests.push_back({"executor select where", select_where_filters_rows});
    tests.push_back({"executor delete where", delete_where_removes_matching_rows});
    tests.push_back({"executor update where", update_where_changes_matching_rows});
    return run_tests(tests);
}
