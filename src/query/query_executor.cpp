#include "query/query_executor.hpp"

#include <exception>
#include <optional>
#include <utility>

#include "catalog/catalog.hpp"
#include "query/value_parser.hpp"

namespace {
    QueryResult message_result(const std::string& message) {
        QueryResult result;
        result.metadata.message = message;
        return result;
    }

    std::size_t token_position(const std::string& command, const std::string& token) {
        const std::size_t position = command.find(token);
        return position == std::string::npos ? 0 : position;
    }

    QueryResult error_result(const std::string& message, const std::string& token, const std::string& source = "", std::size_t position = 0) {
        QueryResult result;
        result.error = QueryError{message, token, source, position};
        return result;
    }

    std::vector<QueryResultColumn> schema_columns() {
        return {
            {"column", "string"},
            {"type", "string"},
            {"nullable", "string"},
            {"max_size", "integer"},
            {"precision", "integer"},
            {"scale", "integer"},
        };
    }

    std::vector<std::string> schema_row(const Column& column) {
        return {
            column.name(),
            Column::type_to_string(column.type()),
            column.nullable() ? "yes" : "no",
            std::to_string(column.max_size()),
            std::to_string(column.precision()),
            std::to_string(column.scale()),
        };
    }

    std::vector<QueryResultColumn> table_columns(const Schema& schema) {
        std::vector<QueryResultColumn> columns;
        for (const Column& column : schema.columns()) {
            columns.push_back({column.name(), Column::type_to_string(column.type())});
        }
        return columns;
    }

    std::vector<std::string> table_row(const Schema& schema, const TableRow& table_row) {
        std::vector<std::string> row;
        for (uint16_t i = 0; i < table_row.row.value_count(); ++i) {
            const Column* column = schema.column(i);
            const Value* value = table_row.row.value(i);
            row.push_back(column != nullptr && value != nullptr ? ValueParser::format_value(*column, *value) : "");
        }
        return row;
    }

    int compare_values(const Value& left, const Value& right) {
        if (left.is_null() || right.is_null()) {
            return left.is_null() == right.is_null() ? 0 : (left.is_null() ? -1 : 1);
        }

        if (left.type() == ColumnType::integer) {
            if (left.integer_data() == right.integer_data()) {
                return 0;
            }
            return left.integer_data() < right.integer_data() ? -1 : 1;
        }
        if (left.type() == ColumnType::number) {
            if (left.number_data() == right.number_data()) {
                return 0;
            }
            return left.number_data() < right.number_data() ? -1 : 1;
        }
        if (left.type() == ColumnType::date) {
            if (left.date_data().days_since_epoch() == right.date_data().days_since_epoch()) {
                return 0;
            }
            return left.date_data().days_since_epoch() < right.date_data().days_since_epoch() ? -1 : 1;
        }
        if (left.type() == ColumnType::time) {
            if (left.time_data().seconds_since_midnight() == right.time_data().seconds_since_midnight()) {
                return 0;
            }
            return left.time_data().seconds_since_midnight() < right.time_data().seconds_since_midnight() ? -1 : 1;
        }
        if (left.type() == ColumnType::datetime) {
            if (left.datetime_data().seconds_since_epoch() == right.datetime_data().seconds_since_epoch()) {
                return 0;
            }
            return left.datetime_data().seconds_since_epoch() < right.datetime_data().seconds_since_epoch() ? -1 : 1;
        }

        if (left.string_data() == right.string_data()) {
            return 0;
        }
        return left.string_data() < right.string_data() ? -1 : 1;
    }

    bool compare_with_operator(int comparison, QueryOperator op) {
        if (op == QueryOperator::equal) {
            return comparison == 0;
        }
        if (op == QueryOperator::not_equal) {
            return comparison != 0;
        }
        if (op == QueryOperator::greater) {
            return comparison > 0;
        }
        if (op == QueryOperator::less) {
            return comparison < 0;
        }
        if (op == QueryOperator::greater_equal) {
            return comparison >= 0;
        }
        if (op == QueryOperator::less_equal) {
            return comparison <= 0;
        }
        return false;
    }

    bool row_matches(const Schema& schema, const TableRow& table_row, const QueryCondition& condition, QueryResult& error) {
        const int column_index = schema.column_index(condition.column_name);
        if (column_index < 0) {
            error = error_result("unknown column", condition.column_name, "", condition.column_position);
            return false;
        }

        const Column* column = schema.column(static_cast<uint16_t>(column_index));
        const Value* stored_value = table_row.row.value(static_cast<uint16_t>(column_index));
        if (column == nullptr || stored_value == nullptr) {
            error = error_result("invalid column", condition.column_name, "", condition.column_position);
            return false;
        }

        std::optional<Value> condition_value = ValueParser::parse_value(*column, condition.value_text);
        if (!condition_value.has_value() || !condition_value->matches_column(*column)) {
            error = error_result("invalid WHERE value", condition.value_text, "", condition.value_position);
            return false;
        }

        error = QueryResult{};
        return compare_with_operator(compare_values(*stored_value, *condition_value), condition.op);
    }

    bool row_matches_condition(const Schema& schema, const TableRow& table_row, const std::optional<QueryCondition>& condition, QueryResult& error) {
        if (!condition.has_value()) {
            return true;
        }
        return row_matches(schema, table_row, *condition, error);
    }
}

bool QueryResult::ok() const {
    return !error.has_value();
}

QueryExecutor::QueryExecutor(std::filesystem::path data_root)
    : data_root_(std::move(data_root)) {}

const std::string& QueryExecutor::current_database() const {
    return current_database_;
}

QueryResult QueryExecutor::execute(const std::string& command) {
    current_command_ = command;
    const ParseResult parsed = QueryParser::parse_with_error(command);
    if (!parsed.ok()) {
        const ParseError& error = *parsed.error;
        return error_result(error.message, error.token, command, error.position);
    }
    return execute_parsed(*parsed.query);
}

Database* QueryExecutor::database() {
    return database_.get();
}

void QueryExecutor::select_database(const std::string& database_name) {
    current_database_ = database_name;
    database_ = std::make_unique<Database>(data_root_, current_database_);
}

QueryResult QueryExecutor::execute_parsed(const ParsedQuery& query) {
    switch (query.type) {
        case QueryType::exit: {
            QueryResult result;
            result.should_exit = true;
            return result;
        }
        case QueryType::help:
            return help();
        case QueryType::show_databases:
            return show_databases();
        case QueryType::show_tables:
            return show_tables();
        case QueryType::create_database:
            return create_database(query.database_name);
        case QueryType::use_database:
            return use_database(query.database_name);
        case QueryType::create_table:
            return create_table(query.table_name, query.columns);
        case QueryType::describe_table:
            return describe_table(query.table_name);
        case QueryType::insert_row:
            return insert_row(query.table_name, query.values_text);
        case QueryType::select_all:
            return select_all(query.table_name, query.condition);
        case QueryType::delete_row:
            return delete_row(query.table_name, query.condition);
        case QueryType::update_row:
            return update_row(query.table_name, query.values_text, query.condition);
        case QueryType::unknown:
            return error_result("unknown command", "", current_command_);
    }

    return error_result("unknown command", "", current_command_);
}

QueryResult QueryExecutor::help() const {
    QueryResult result;
    result.columns = {{"command", "string"}};
    result.rows = {
        {"CREATE DATABASE name;"},
        {"USE name;"},
        {"SHOW DATABASES;"},
        {"SHOW TABLES;"},
        {"CREATE TABLE name (id INTEGER NOT NULL, name VARSTRING(128) NULL);"},
        {"DESCRIBE table;"},
        {"INSERT INTO table VALUES (1, 'alice', NULL);"},
        {"SELECT * FROM table;"},
        {"DELETE FROM table WHERE id = 1;"},
        {"UPDATE table SET VALUES (1, 'alice', NULL) WHERE id = 1;"},
        {"EXIT;"},
    };
    result.metadata.row_count = result.rows.size();
    return result;
}

QueryResult QueryExecutor::create_database(const std::string& database_name) {
    select_database(database_name);
    if (database_->database_exists()) {
        return error_result("database already exists", database_name, current_command_, token_position(current_command_, database_name));
    }
    if (!database_->create_database()) {
        return error_result("could not create database", database_name, current_command_, token_position(current_command_, database_name));
    }
    return message_result("database created");
}

QueryResult QueryExecutor::use_database(const std::string& database_name) {
    Database candidate(data_root_, database_name);
    if (!candidate.database_exists()) {
        return error_result("unknown database", database_name, current_command_, token_position(current_command_, database_name));
    }

    select_database(database_name);
    return message_result("database changed");
}

QueryResult QueryExecutor::show_databases() const {
    QueryResult result;
    result.columns = {{"database", "string"}};

    Catalog catalog(data_root_);
    for (const std::string& name : catalog.list_databases()) {
        result.rows.push_back({name});
    }
    result.metadata.row_count = result.rows.size();
    return result;
}

QueryResult QueryExecutor::show_tables() {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", "", current_command_);
    }

    QueryResult result;
    result.columns = {{"table", "string"}};
    for (const std::string& name : db->list_tables()) {
        result.rows.push_back({name});
    }
    result.metadata.row_count = result.rows.size();
    return result;
}

QueryResult QueryExecutor::create_table(const std::string& table_name, const std::vector<Column>& columns) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    try {
        if (db->table_exists(table_name)) {
            return error_result("table already exists", table_name, current_command_, token_position(current_command_, table_name));
        }
        if (!db->create_table(Schema(table_name, columns))) {
            return error_result("could not create table", table_name, current_command_, token_position(current_command_, table_name));
        }
    } catch (const std::exception& error) {
        return error_result(error.what(), table_name, current_command_, token_position(current_command_, table_name));
    }

    return message_result("table created");
}

QueryResult QueryExecutor::describe_table(const std::string& table_name) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result;
    result.columns = schema_columns();
    for (const Column& column : schema->columns()) {
        result.rows.push_back(schema_row(column));
    }
    result.metadata.row_count = result.rows.size();
    return result;
}

QueryResult QueryExecutor::insert_row(const std::string& table_name, const std::string& values_text) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Row> row = ValueParser::parse_row(*schema, values_text);
    if (!row.has_value()) {
        return error_result("invalid values", values_text, current_command_, token_position(current_command_, values_text));
    }

    std::optional<RecordId> record_id = db->insert_row(table_name, *row);
    if (!record_id.has_value()) {
        return error_result("insert failed", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result = message_result("row inserted");
    result.metadata.row_count = 1;
    return result;
}

QueryResult QueryExecutor::select_all(const std::string& table_name, const std::optional<QueryCondition>& condition) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result;
    result.columns = table_columns(*schema);
    QueryResult condition_error;
    const bool completed = db->scan_rows(table_name, [&schema, &condition, &condition_error, &result](const TableRow& row) {
        const bool matches = row_matches_condition(*schema, row, condition, condition_error);
        if (!condition_error.ok()) {
            return false;
        }
        if (matches) {
            result.rows.push_back(table_row(*schema, row));
        }
        return true;
    });

    if (!completed) {
        if (!condition_error.ok()) {
            condition_error.error->source = current_command_;
            return condition_error;
        }
        return error_result("scan failed", table_name, current_command_, token_position(current_command_, table_name));
    }

    result.metadata.row_count = result.rows.size();
    return result;
}

QueryResult QueryExecutor::delete_row(const std::string& table_name, const std::optional<QueryCondition>& condition) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult condition_error;
    uint64_t deleted = 0;
    const bool completed = db->scan_rows(table_name, [db, &schema, &table_name, &condition, &condition_error, &deleted](const TableRow& row) {
        const bool matches = row_matches_condition(*schema, row, condition, condition_error);
        if (!condition_error.ok()) {
            return false;
        }
        if (matches && db->delete_row(table_name, row.record_id)) {
            ++deleted;
        }
        return true;
    });

    if (!completed) {
        if (!condition_error.ok()) {
            condition_error.error->source = current_command_;
            return condition_error;
        }
        return error_result("scan failed", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result = message_result("row deleted");
    result.metadata.row_count = deleted;
    return result;
}

QueryResult QueryExecutor::update_row(const std::string& table_name, const std::string& values_text, const std::optional<QueryCondition>& condition) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Row> row = ValueParser::parse_row(*schema, values_text);
    if (!row.has_value()) {
        return error_result("invalid values", values_text, current_command_, token_position(current_command_, values_text));
    }

    QueryResult condition_error;
    uint64_t updated = 0;
    const bool completed = db->scan_rows(table_name, [db, &schema, &table_name, &condition, &condition_error, &row, &updated](const TableRow& table_row) {
        const bool matches = row_matches_condition(*schema, table_row, condition, condition_error);
        if (!condition_error.ok()) {
            return false;
        }

        RecordId record_id = table_row.record_id;
        if (matches && db->update_row(table_name, record_id, *row)) {
            ++updated;
        }
        return true;
    });

    if (!completed) {
        if (!condition_error.ok()) {
            condition_error.error->source = current_command_;
            return condition_error;
        }
        return error_result("scan failed", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result = message_result("row updated");
    result.metadata.row_count = updated;
    return result;
}
