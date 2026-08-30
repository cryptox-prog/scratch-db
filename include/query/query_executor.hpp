#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "database/database.hpp"
#include "query/query_parser.hpp"

struct QueryResultColumn {
    std::string name;
    std::string type;
};

struct QueryResultMetadata {
    uint64_t row_count = 0;
    std::string message;
};

struct QueryError {
    std::string message;
    std::string token;
    std::string source;
    std::size_t position = 0;
};

struct QueryResult {
    std::vector<QueryResultColumn> columns;
    std::vector<std::vector<std::string>> rows;
    QueryResultMetadata metadata;
    std::optional<QueryError> error;
    bool should_exit = false;

    bool ok() const;
};

class QueryExecutor {
public:
    explicit QueryExecutor(std::filesystem::path data_root);

    const std::string& current_database() const;
    QueryResult execute(const std::string& command);

private:
    Database* database();
    void select_database(const std::string& database_name);
    QueryResult execute_parsed(const ParsedQuery& query);
    QueryResult help() const;
    QueryResult create_database(const std::string& database_name);
    QueryResult use_database(const std::string& database_name);
    QueryResult show_databases() const;
    QueryResult show_tables();
    QueryResult create_table(const std::string& table_name, const std::vector<Column>& columns);
    QueryResult describe_table(const std::string& table_name);
    QueryResult insert_row(const std::string& table_name, const std::string& values_text);
    QueryResult select_all(const std::string& table_name, const std::optional<QueryCondition>& condition);
    QueryResult delete_row(const std::string& table_name, const std::optional<QueryCondition>& condition);
    QueryResult update_row(const std::string& table_name, const std::string& values_text, const std::optional<QueryCondition>& condition);

    std::filesystem::path data_root_;
    std::string current_command_;
    std::string current_database_;
    std::unique_ptr<Database> database_;
};
