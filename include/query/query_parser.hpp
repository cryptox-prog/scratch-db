#pragma once

#include <optional>
#include <string>
#include <vector>

#include "catalog/column.hpp"

enum class QueryType {
    exit,
    help,
    show_databases,
    show_tables,
    create_database,
    use_database,
    create_table,
    describe_table,
    insert_row,
    select_all,
    delete_row,
    update_row,
    unknown,
};

enum class QueryOperator {
    equal,
    not_equal,
    greater,
    less,
    greater_equal,
    less_equal,
};

struct QueryCondition {
    std::string column_name;
    QueryOperator op = QueryOperator::equal;
    std::string value_text;
    std::size_t column_position = 0;
    std::size_t value_position = 0;
};

struct ParsedQuery {
    QueryType type = QueryType::unknown;
    std::string database_name;
    std::string table_name;
    std::vector<Column> columns;
    std::string values_text;
    std::optional<QueryCondition> condition;
};

struct ParseError {
    std::string message;
    std::string token;
    std::size_t position = 0;
};

struct ParseResult {
    std::optional<ParsedQuery> query;
    std::optional<ParseError> error;

    bool ok() const;
};

class QueryParser {
public:
    static std::optional<ParsedQuery> parse(const std::string& command);
    static ParseResult parse_with_error(const std::string& command);
};
