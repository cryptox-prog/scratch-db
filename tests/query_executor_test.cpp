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

    result = executor.execute("CREATE TABLE item_names (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)");
    require(result.ok(), "create item_names failed");
    result = executor.execute("INSERT INTO item_names (name, id) VALUES ('book', 2)");
    require(result.ok(), "insert with explicit column order failed");
    require(result.metadata.row_count == 1, "insert column-order row count wrong");

    result = executor.execute("SELECT * FROM item_names WHERE id = 2");
    require(result.ok(), "select after column-order insert failed");
    require(result.rows.size() == 1, "insert column-order row missing");
    require(result.rows[0][0] == "2", "column-order id wrong");
    require(result.rows[0][1] == "book", "column-order name wrong");

    std::filesystem::remove_all(root);
}

void execute_multi_row_insert() {
    const std::filesystem::path root = test_root("query_executor_multi_row_insert");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE shop").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create table failed");

    QueryResult result = executor.execute("INSERT INTO items VALUES (1, 'pen'), (2, 'book')");
    require(result.ok(), "multi-row insert failed");
    require(result.metadata.row_count == 2, "multi-row insert count wrong");

    result = executor.execute("SELECT * FROM items ORDER BY id");
    require(result.ok(), "select after multi-row insert failed");
    require(result.rows.size() == 2, "multi-row insert row count wrong");
    require(result.rows[0][0] == "1", "first inserted row id wrong");
    require(result.rows[0][1] == "pen", "first inserted row name wrong");
    require(result.rows[1][0] == "2", "second inserted row id wrong");
    require(result.rows[1][1] == "book", "second inserted row name wrong");

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

    QueryResult result = executor.execute("UPDATE items SET name = 'pencil' WHERE id = 1");
    require(result.ok(), "assignment update where failed");
    require(result.metadata.row_count == 1, "assignment update where count wrong");

    result = executor.execute("SELECT * FROM items WHERE id = 1");
    require(result.ok(), "select after assignment update failed");
    require(result.rows.size() == 1, "updated row missing");
    require(result.rows[0][1] == "pencil", "updated value wrong");

    result = executor.execute("UPDATE items SET name = 'eraser', id = 3 WHERE name = 'book'");
    require(result.ok(), "multi-assignment update failed");
    require(result.metadata.row_count == 1, "multi-assignment update count wrong");

    result = executor.execute("SELECT * FROM items WHERE id = 3");
    require(result.ok(), "select after multi-assignment update failed");
    require(result.rows.size() == 1, "multi-assignment updated row missing");
    require(result.rows[0][1] == "eraser", "multi-assignment name wrong");

    std::filesystem::remove_all(root);
}

void insert_value_error_points_to_literal() {
    const std::filesystem::path root = test_root("query_executor_insert_value_error");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, price NUMBER(4, 2) NOT NULL)").ok(), "create table failed");

    QueryResult result = executor.execute("INSERT INTO items VALUES (1, 12.345)");
    require(!result.ok(), "bad insert value accepted");
    require(result.error->message == "invalid NUMBER value for column price", "bad insert value message wrong");
    require(result.error->token == "12.345", "bad insert value token wrong");
    require(result.error->position == 29, "bad insert value position wrong");

    std::filesystem::remove_all(root);
}

void where_value_error_points_to_literal() {
    const std::filesystem::path root = test_root("query_executor_where_value_error");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, sold_on DATE NOT NULL)").ok(), "create table failed");
    require(executor.execute("INSERT INTO items VALUES (1, '2026-08-30')").ok(), "insert failed");

    QueryResult result = executor.execute("SELECT * FROM items WHERE sold_on = 2026-08-30");
    require(!result.ok(), "bad where value accepted");
    require(result.error->message == "date value for column sold_on must be quoted", "bad where value message wrong");
    require(result.error->token == "2026-08-30", "bad where value token wrong");

    std::filesystem::remove_all(root);
}

void where_and_or_filters_rows() {
    const std::filesystem::path root = test_root("query_executor_where_and_or");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, price NUMBER(4, 2) NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 5.00, 'pen')").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 15.00, 'book')").ok(), "insert 2 failed");
    require(executor.execute("INSERT INTO items VALUES (3, 20.00, 'pen')").ok(), "insert 3 failed");

    QueryResult result = executor.execute("SELECT * FROM items WHERE price > 10.00 AND name != 'pen'");
    require(result.ok(), "and where failed");
    require(result.rows.size() == 1, "and where row count wrong");
    require(result.rows[0][0] == "2", "and where wrong row");

    result = executor.execute("SELECT * FROM items WHERE id = 1 OR price > 10.00 AND name != 'pen'");
    require(result.ok(), "or where failed");
    require(result.rows.size() == 2, "or precedence row count wrong");
    require(result.rows[0][0] == "1", "or precedence first row wrong");
    require(result.rows[1][0] == "2", "or precedence second row wrong");

    result = executor.execute("SELECT * FROM items WHERE (id = 1 OR price > 10.00) AND name != 'pen'");
    require(result.ok(), "grouped where failed");
    require(result.rows.size() == 1, "grouped where row count wrong");
    require(result.rows[0][0] == "2", "grouped where wrong row");

    std::filesystem::remove_all(root);
}

void chained_join_executes() {
    const std::filesystem::path root = test_root("query_executor_chained_join");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE a (id INTEGER NOT NULL)").ok(), "create table a failed");
    require(executor.execute("CREATE TABLE b (id INTEGER NOT NULL, aid INTEGER NOT NULL)").ok(), "create table b failed");
    require(executor.execute("CREATE TABLE c (id INTEGER NOT NULL, bid INTEGER NOT NULL)").ok(), "create table c failed");
    require(executor.execute("INSERT INTO a VALUES (1)").ok(), "insert a failed");
    require(executor.execute("INSERT INTO b VALUES (10, 1)").ok(), "insert b failed");
    require(executor.execute("INSERT INTO c VALUES (100, 10)").ok(), "insert c failed");

    QueryResult result = executor.execute("SELECT * FROM a JOIN b ON a.id = b.aid JOIN c ON b.id = c.bid");
    require(result.ok(), "chained join failed");
    require(result.rows.size() == 1, "chained join row count wrong");
    require(result.columns.size() == 5, "chained join column count wrong");
    require(result.columns[0].name == "id", "chained join left column wrong");
    require(result.columns[1].name == "id_1", "chained join middle id column wrong");
    require(result.columns[2].name == "aid", "chained join middle aid column wrong");
    require(result.columns[3].name == "id_2", "chained join right id column wrong");
    require(result.columns[4].name == "bid", "chained join right bid column wrong");
    require(result.rows[0][0] == "1", "chained join left value wrong");
    require(result.rows[0][1] == "10", "chained join middle value wrong");
    require(result.rows[0][2] == "1", "chained join middle foreign key wrong");
    require(result.rows[0][3] == "100", "chained join right value wrong");
    require(result.rows[0][4] == "10", "chained join right foreign key wrong");

    std::filesystem::remove_all(root);
}

void compound_selects_union_and_intersect() {
    const std::filesystem::path root = test_root("query_executor_compound_selects");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE left_items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create left table failed");
    require(executor.execute("CREATE TABLE right_items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create right table failed");
    require(executor.execute("INSERT INTO left_items VALUES (1, 'pen')").ok(), "left insert 1 failed");
    require(executor.execute("INSERT INTO left_items VALUES (2, 'book')").ok(), "left insert 2 failed");
    require(executor.execute("INSERT INTO right_items VALUES (2, 'book')").ok(), "right insert 1 failed");
    require(executor.execute("INSERT INTO right_items VALUES (3, 'pencil')").ok(), "right insert 2 failed");

    QueryResult result = executor.execute("SELECT * FROM left_items UNION SELECT * FROM right_items");
    require(result.ok(), "union failed");
    require(result.rows.size() == 3, "union row count wrong");

    result = executor.execute("SELECT * FROM left_items UNION ALL SELECT * FROM right_items");
    require(result.ok(), "union all failed");
    require(result.rows.size() == 4, "union all row count wrong");

    result = executor.execute("SELECT * FROM left_items INTERSECT SELECT * FROM right_items");
    require(result.ok(), "intersect failed");
    require(result.rows.size() == 1, "intersect row count wrong");
    require(result.rows[0][0] == "2", "intersect row wrong");

    std::filesystem::remove_all(root);
}

void aggregate_and_scalar_subquery_executes() {
    const std::filesystem::path root = test_root("query_executor_scalar_subquery");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE current_items (id INTEGER NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create current table failed");
    require(executor.execute("CREATE TABLE old_items (id INTEGER NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create old table failed");
    require(executor.execute("INSERT INTO current_items VALUES (1, 9.00)").ok(), "current insert 1 failed");
    require(executor.execute("INSERT INTO current_items VALUES (2, 12.50)").ok(), "current insert 2 failed");
    require(executor.execute("INSERT INTO current_items VALUES (3, 20.00)").ok(), "current insert 3 failed");
    require(executor.execute("INSERT INTO old_items VALUES (1, 10.00)").ok(), "old insert 1 failed");
    require(executor.execute("INSERT INTO old_items VALUES (2, 15.00)").ok(), "old insert 2 failed");

    QueryResult max_result = executor.execute("SELECT MAX(price) FROM old_items");
    require(max_result.ok(), "max aggregate failed");
    require(max_result.columns.size() == 1, "max aggregate column count wrong");
    require(max_result.rows.size() == 1, "max aggregate row count wrong");
    require(max_result.rows[0][0] == "15.00", "max aggregate value wrong");

    QueryResult result = executor.execute("SELECT * FROM current_items WHERE price > (SELECT MAX(price) FROM old_items)");
    require(result.ok(), "scalar subquery failed");
    require(result.rows.size() == 1, "scalar subquery row count wrong");
    require(result.rows[0][0] == "3", "scalar subquery selected wrong row");

    std::filesystem::remove_all(root);
}

void avg_number_uses_fixed_decimal_scale() {
    const std::filesystem::path root = test_root("query_executor_avg_number");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE prices (id INTEGER NOT NULL, amount NUMBER(6, 2) NOT NULL)").ok(), "create prices table failed");
    require(executor.execute("INSERT INTO prices VALUES (1, 10.00)").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO prices VALUES (2, 15.00)").ok(), "insert 2 failed");
    require(executor.execute("INSERT INTO prices VALUES (3, 17.50)").ok(), "insert 3 failed");

    QueryResult result = executor.execute("SELECT AVG(amount) FROM prices");
    require(result.ok(), "avg aggregate failed");
    require(result.columns.size() == 1, "avg aggregate column count wrong");
    require(result.rows.size() == 1, "avg aggregate row count wrong");
    require(result.rows[0][0] == "14.17", "avg aggregate value wrong");

    std::filesystem::remove_all(root);
}

void select_column_aliases_and_order_by_alias() {
    const std::filesystem::path root = test_root("query_executor_column_alias");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create items table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 10.00)").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 15.00)").ok(), "insert 2 failed");
    require(executor.execute("INSERT INTO items VALUES (3, 5.00)").ok(), "insert 3 failed");

    QueryResult result = executor.execute("SELECT price AS unit_price FROM items ORDER BY unit_price DESC");
    require(result.ok(), "order by alias failed");
    require(result.columns.size() == 1, "alias column count wrong");
    require(result.columns[0].name == "unit_price", "alias column name wrong");
    require(result.rows.size() == 3, "alias row count wrong");
    require(result.rows[0][0] == "15.00", "alias highest price wrong");
    require(result.rows[2][0] == "5.00", "alias lowest price wrong");

    std::filesystem::remove_all(root);
}

void subquery_predicates_execute() {
    const std::filesystem::path root = test_root("query_executor_subquery_predicates");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create items table failed");
    require(executor.execute("CREATE TABLE old_items (id INTEGER NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create old items table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 5.00)").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 15.00)").ok(), "insert 2 failed");
    require(executor.execute("INSERT INTO items VALUES (3, 25.00)").ok(), "insert 3 failed");
    require(executor.execute("INSERT INTO old_items VALUES (2, 10.00)").ok(), "insert 4 failed");
    require(executor.execute("INSERT INTO old_items VALUES (4, 20.00)").ok(), "insert 5 failed");

    QueryResult result = executor.execute("SELECT * FROM items WHERE id NOT IN (SELECT id FROM old_items)");
    require(result.ok(), "not in subquery failed");
    require(result.rows.size() == 2, "not in row count wrong");
    require(result.rows[0][0] == "1", "not in first row wrong");
    require(result.rows[1][0] == "3", "not in second row wrong");

    result = executor.execute("SELECT * FROM items WHERE price > ANY (SELECT price FROM old_items)");
    require(result.ok(), "any subquery failed");
    require(result.rows.size() == 2, "any row count wrong");
    require(result.rows[0][0] == "2", "any first row wrong");
    require(result.rows[1][0] == "3", "any second row wrong");

    std::filesystem::remove_all(root);
}

void group_by_aggregates_rows() {
    const std::filesystem::path root = test_root("query_executor_group_by");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE sales (category VARSTRING(16) NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create sales table failed");
    require(executor.execute("INSERT INTO sales VALUES ('book', 10.00)").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO sales VALUES ('book', 15.50)").ok(), "insert 2 failed");
    require(executor.execute("INSERT INTO sales VALUES ('pen', 2.00)").ok(), "insert 3 failed");

    QueryResult result = executor.execute("SELECT category, MAX(price), COUNT(*) FROM sales GROUP BY category");
    require(result.ok(), "group by failed");
    require(result.columns.size() == 3, "group by column count wrong");
    require(result.rows.size() == 2, "group by row count wrong");
    require(result.rows[0][0] == "book", "first group name wrong");
    require(result.rows[0][1] == "15.50", "first group max wrong");
    require(result.rows[0][2] == "2", "first group count wrong");
    require(result.rows[1][0] == "pen", "second group name wrong");
    require(result.rows[1][1] == "2.00", "second group max wrong");
    require(result.rows[1][2] == "1", "second group count wrong");

    result = executor.execute("SELECT category, price, MAX(price) FROM sales GROUP BY category");
    require(!result.ok(), "non-grouped selected column accepted");
    require(result.error->message == "selected column must appear in GROUP BY", "wrong non-grouped error");
    require(result.error->token == "price", "wrong non-grouped token");

    std::filesystem::remove_all(root);
}

void in_and_exists_subqueries_execute() {
    const std::filesystem::path root = test_root("query_executor_in_exists_subquery");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create items table failed");
    require(executor.execute("CREATE TABLE picked (id INTEGER NOT NULL)").ok(), "create picked table failed");
    require(executor.execute("CREATE TABLE empty_items (id INTEGER NOT NULL)").ok(), "create empty table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 'pen')").ok(), "insert item 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 'book')").ok(), "insert item 2 failed");
    require(executor.execute("INSERT INTO items VALUES (3, 'bag')").ok(), "insert item 3 failed");
    require(executor.execute("INSERT INTO picked VALUES (1)").ok(), "insert picked 1 failed");
    require(executor.execute("INSERT INTO picked VALUES (3)").ok(), "insert picked 3 failed");

    QueryResult result = executor.execute("SELECT * FROM items WHERE id IN (SELECT id FROM picked)");
    require(result.ok(), "in subquery failed");
    require(result.rows.size() == 2, "in subquery row count wrong");
    require(result.rows[0][0] == "1", "in subquery first row wrong");
    require(result.rows[1][0] == "3", "in subquery second row wrong");

    result = executor.execute("SELECT * FROM items WHERE EXISTS (SELECT * FROM picked)");
    require(result.ok(), "exists subquery failed");
    require(result.rows.size() == 3, "exists subquery row count wrong");

    result = executor.execute("SELECT * FROM items WHERE NOT EXISTS (SELECT * FROM empty_items)");
    require(result.ok(), "not exists subquery failed");
    require(result.rows.size() == 3, "not exists subquery row count wrong");

    std::filesystem::remove_all(root);
}

void derived_table_executes() {
    const std::filesystem::path root = test_root("query_executor_derived_table");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create items table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 'pen', 2.50)").ok(), "insert item 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 'book', 12.00)").ok(), "insert item 2 failed");

    QueryResult result = executor.execute("SELECT x.id FROM (SELECT id FROM items) AS x");
    require(result.ok(), "derived table select failed");
    require(result.columns.size() == 1, "derived table column count wrong");
    require(result.rows.size() == 2, "derived table row count wrong");
    require(result.rows[0][0] == "1", "derived table first row wrong");
    require(result.rows[1][0] == "2", "derived table second row wrong");

    result = executor.execute("SELECT x.id FROM (SELECT id, price FROM items) AS x WHERE x.price > 10.00");
    require(result.ok(), "derived table where failed");
    require(result.rows.size() == 1, "derived table where row count wrong");
    require(result.rows[0][0] == "2", "derived table where row wrong");

    std::filesystem::remove_all(root);
}

void with_queries_execute() {
    const std::filesystem::path root = test_root("query_executor_with");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create items table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 2.50)").ok(), "insert item 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 12.00)").ok(), "insert item 2 failed");

    QueryResult result = executor.execute("WITH picked AS (SELECT id, price FROM items WHERE price > 10.00) SELECT picked.id FROM picked");
    require(result.ok(), "with select failed");
    require(result.rows.size() == 1, "with select row count wrong");
    require(result.rows[0][0] == "2", "with select row wrong");

    result = executor.execute(
        "WITH base AS (SELECT id, price FROM items), picked AS (SELECT id FROM base WHERE price > 10.00) SELECT * FROM picked"
    );
    require(result.ok(), "multi with select failed");
    require(result.rows.size() == 1, "multi with row count wrong");
    require(result.rows[0][0] == "2", "multi with row wrong");

    std::filesystem::remove_all(root);
}

void order_by_sorts_results() {
    const std::filesystem::path root = test_root("query_executor_order_by");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, category VARSTRING(16) NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create items table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 'book', 10.00)").ok(), "insert item 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 'pen', 2.00)").ok(), "insert item 2 failed");
    require(executor.execute("INSERT INTO items VALUES (3, 'book', 15.50)").ok(), "insert item 3 failed");

    QueryResult result = executor.execute("SELECT id, price FROM items ORDER BY price DESC");
    require(result.ok(), "order by failed");
    require(result.rows.size() == 3, "order by row count wrong");
    require(result.rows[0][0] == "3", "order by first row wrong");
    require(result.rows[1][0] == "1", "order by second row wrong");
    require(result.rows[2][0] == "2", "order by third row wrong");

    result = executor.execute("SELECT category, MAX(price) FROM items GROUP BY category ORDER BY category ASC");
    require(result.ok(), "group order by failed");
    require(result.rows.size() == 2, "group order by row count wrong");
    require(result.rows[0][0] == "book", "group order by first row wrong");
    require(result.rows[1][0] == "pen", "group order by second row wrong");

    std::filesystem::remove_all(root);
}

void having_and_limit_filter_results() {
    const std::filesystem::path root = test_root("query_executor_having_limit");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, category VARSTRING(16) NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create items table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 'book', 10.00)").ok(), "insert item 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 'book', 15.00)").ok(), "insert item 2 failed");
    require(executor.execute("INSERT INTO items VALUES (3, 'pen', 2.00)").ok(), "insert item 3 failed");
    require(executor.execute("INSERT INTO items VALUES (4, 'bag', 20.00)").ok(), "insert item 4 failed");
    require(executor.execute("INSERT INTO items VALUES (5, 'bag', 25.00)").ok(), "insert item 5 failed");

    QueryResult result = executor.execute("SELECT category, COUNT(*) FROM items GROUP BY category HAVING COUNT(*) > 1 ORDER BY category ASC LIMIT 1");
    require(result.ok(), "having limit failed");
    require(result.rows.size() == 1, "having limit row count wrong");
    require(result.rows[0][0] == "bag", "having limit row wrong");
    require(result.rows[0][1] == "2", "having count wrong");

    result = executor.execute("SELECT id FROM items ORDER BY price DESC LIMIT 2");
    require(result.ok(), "limit failed");
    require(result.rows.size() == 2, "limit row count wrong");
    require(result.rows[0][0] == "5", "limit first row wrong");
    require(result.rows[1][0] == "4", "limit second row wrong");

    std::filesystem::remove_all(root);
}

void correlated_subqueries_execute() {
    const std::filesystem::path root = test_root("query_executor_correlated_subquery");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create items table failed");
    require(executor.execute("CREATE TABLE picked (id INTEGER NOT NULL)").ok(), "create picked table failed");
    require(executor.execute("CREATE TABLE prices (item_id INTEGER NOT NULL, price NUMBER(5, 2) NOT NULL)").ok(), "create prices table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 'pen')").ok(), "insert item 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 'book')").ok(), "insert item 2 failed");
    require(executor.execute("INSERT INTO picked VALUES (2)").ok(), "insert picked failed");
    require(executor.execute("INSERT INTO prices VALUES (1, 9.00)").ok(), "insert price 1 failed");
    require(executor.execute("INSERT INTO prices VALUES (2, 20.00)").ok(), "insert price 2 failed");

    QueryResult result = executor.execute("SELECT * FROM items WHERE EXISTS (SELECT * FROM picked WHERE picked.id = items.id)");
    require(result.ok(), "correlated exists failed");
    require(result.rows.size() == 1, "correlated exists row count wrong");
    require(result.rows[0][0] == "2", "correlated exists row wrong");

    result = executor.execute("SELECT * FROM items WHERE id = (SELECT MAX(item_id) FROM prices WHERE prices.item_id = items.id)");
    require(result.ok(), "correlated scalar aggregate failed");
    require(result.rows.size() == 2, "correlated scalar row count wrong");
    require(result.rows[0][0] == "1", "correlated scalar first row wrong");
    require(result.rows[1][0] == "2", "correlated scalar second row wrong");

    std::filesystem::remove_all(root);
}

void cartesian_and_join_queries_run() {
    const std::filesystem::path root = test_root("query_executor_join");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE left_items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create left table failed");
    require(executor.execute("CREATE TABLE right_items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create right table failed");
    require(executor.execute("INSERT INTO left_items VALUES (1, 'pen')").ok(), "left insert 1 failed");
    require(executor.execute("INSERT INTO left_items VALUES (2, 'book')").ok(), "left insert 2 failed");
    require(executor.execute("INSERT INTO right_items VALUES (1, 'pen')").ok(), "right insert 1 failed");
    require(executor.execute("INSERT INTO right_items VALUES (2, 'book')").ok(), "right insert 2 failed");

    QueryResult result = executor.execute("SELECT * FROM left_items, right_items");
    require(result.ok(), "cartesian product failed");
    require(result.rows.size() == 4, "cartesian row count wrong");

    result = executor.execute("SELECT * FROM left_items JOIN right_items ON id = id");
    require(result.ok(), "join failed");
    require(result.rows.size() == 2, "join row count wrong");

    result = executor.execute("SELECT * FROM left_items LEFT JOIN right_items ON left_items.id = right_items.id");
    require(result.ok(), "left join failed");
    require(result.rows.size() == 2, "left join row count wrong");

    result = executor.execute("SELECT * FROM left_items NATURAL JOIN right_items");
    require(result.ok(), "natural join failed");
    require(result.rows.size() == 2, "natural join row count wrong");

    result = executor.execute("SELECT left_items.id FROM left_items AS left_items");
    require(result.ok(), "qualified select failed");
    require(result.rows.size() == 2, "qualified select row count wrong");

    result = executor.execute("SELECT right_items.id FROM left_items JOIN right_items ON left_items.id = right_items.id");
    require(result.ok(), "qualified join select failed");
    require(result.columns.size() == 1, "qualified join select column count wrong");
    require(result.rows.size() == 2, "qualified join select row count wrong");
    require(result.rows[0][0] == "1", "qualified join select first row wrong");
    require(result.rows[1][0] == "2", "qualified join select second row wrong");

    result = executor.execute("SELECT id FROM left_items JOIN right_items ON left_items.id = right_items.id");
    require(!result.ok(), "ambiguous join select accepted");
    require(result.error->message == "ambiguous selected column", "wrong ambiguous selected column error");
    require(result.error->token == "id", "wrong ambiguous selected column token");

    std::filesystem::remove_all(root);
}

void select_table_alias_runs() {
    const std::filesystem::path root = test_root("query_executor_select_alias");
    std::filesystem::remove_all(root);

    QueryExecutor executor(root);
    require(executor.execute("CREATE DATABASE db").ok(), "create database failed");
    require(executor.execute("CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").ok(), "create table failed");
    require(executor.execute("INSERT INTO items VALUES (1, 'pen')").ok(), "insert 1 failed");
    require(executor.execute("INSERT INTO items VALUES (2, 'book')").ok(), "insert 2 failed");

    QueryResult result = executor.execute("SELECT * FROM items AS i WHERE id = 2");
    require(result.ok(), "select alias failed");
    require(result.rows.size() == 1, "alias row count wrong");
    require(result.rows[0][1] == "book", "alias row wrong");

    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"executor insert select", execute_insert_and_select});
    tests.push_back({"executor multi row insert", execute_multi_row_insert});
    tests.push_back({"executor exit", execute_exit_returns_false});
    tests.push_back({"executor invalid query", invalid_query_returns_error});
    tests.push_back({"executor duplicate database", duplicate_database_returns_specific_error});
    tests.push_back({"executor duplicate table", duplicate_table_returns_specific_error});
    tests.push_back({"executor select where", select_where_filters_rows});
    tests.push_back({"executor delete where", delete_where_removes_matching_rows});
    tests.push_back({"executor update where", update_where_changes_matching_rows});
    tests.push_back({"executor insert value error", insert_value_error_points_to_literal});
    tests.push_back({"executor where value error", where_value_error_points_to_literal});
    tests.push_back({"executor where and or", where_and_or_filters_rows});
    tests.push_back({"executor compound selects", compound_selects_union_and_intersect});
    tests.push_back({"executor aggregate and scalar subquery", aggregate_and_scalar_subquery_executes});
    tests.push_back({"executor avg number fixed scale", avg_number_uses_fixed_decimal_scale});
    tests.push_back({"executor column alias", select_column_aliases_and_order_by_alias});
    tests.push_back({"executor group by", group_by_aggregates_rows});
    tests.push_back({"executor in and exists subqueries", in_and_exists_subqueries_execute});
    tests.push_back({"executor derived table", derived_table_executes});
    tests.push_back({"executor with queries", with_queries_execute});
    tests.push_back({"executor order by", order_by_sorts_results});
    tests.push_back({"executor having and limit", having_and_limit_filter_results});
    tests.push_back({"executor correlated subqueries", correlated_subqueries_execute});
    tests.push_back({"executor chained join", chained_join_executes});
    tests.push_back({"executor join queries", cartesian_and_join_queries_run});
    tests.push_back({"executor table alias", select_table_alias_runs});
    return run_tests(tests);
}
