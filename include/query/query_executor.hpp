#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
    std::map<std::string, std::size_t> column_positions;
    std::set<std::string> ambiguous_columns;
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
    QueryResult execute_join(const ParsedQuery& query);
    QueryResult execute_select(const ParsedQuery& query);
    QueryResult execute_row_source(const ParsedQuery& query);
    QueryResult execute_select_chain(const ParsedQuery& query);
    QueryResult resolve_subqueries(QueryConditionNode* condition);
    QueryResult resolve_subqueries(QueryConditionNode* condition, const QueryResult* outer_result, const std::vector<std::string>* outer_row);
    QueryResult scalar_subquery_value(const ParsedQuery& query, std::string& value_text);
    QueryResult scalar_subquery_value(const ParsedQuery& query, const QueryResult* outer_result, const std::vector<std::string>* outer_row, std::string& value_text);
    QueryResult execute_select_chain(const ParsedQuery& query, const QueryResult* outer_result, const std::vector<std::string>* outer_row);
    QueryResult help() const;
    QueryResult create_database(const std::string& database_name);
    QueryResult use_database(const std::string& database_name);
    QueryResult show_databases() const;
    QueryResult show_tables();
    QueryResult show_indexes(const std::string& table_name);
    QueryResult create_table(const std::string& table_name, const std::vector<Column>& columns, const std::vector<ConstraintDefinition>& constraints = {}, bool if_not_exists = false, TableStorageMode storage_mode = TableStorageMode::disk);
    QueryResult create_index(const std::string& table_name, const std::string& index_name, const std::string& column_name, bool unique);
    QueryResult drop_database(const std::string& database_name);
    QueryResult drop_table(const std::string& table_name);
    QueryResult drop_index(const std::string& table_name, const std::string& index_name);
    QueryResult alter_table(const ParsedQuery& query);
    QueryResult describe_table(const std::string& table_name);
    QueryResult insert_row(const std::string& table_name, const std::string& values_text);
    QueryResult insert_row(const std::string& table_name, const std::vector<std::string>& insert_columns, const std::string& values_text);
    QueryResult insert_row(const std::string& table_name, const std::vector<std::string>& insert_columns, const std::vector<std::string>& values_rows);
    QueryResult insert_select(const std::string& table_name, const std::vector<std::string>& insert_columns, const ParsedQuery& select_query);
    QueryResult select_all(const std::string& table_name, const std::string& table_alias, const QueryConditionNode* condition);
    QueryResult delete_row(const std::string& table_name, const QueryConditionNode* condition);
    QueryResult update_row(const std::string& table_name, const std::string& values_text, const QueryConditionNode* condition);
    QueryResult update_row(const std::string& table_name, const std::vector<std::pair<std::string, std::string>>& assignments, const QueryConditionNode* condition);
    QueryResult begin_transaction();
    QueryResult commit_transaction();
    QueryResult rollback_transaction();

    std::filesystem::path data_root_;
    std::string current_command_;
    std::string current_database_;
    std::unique_ptr<Database> database_;
};
