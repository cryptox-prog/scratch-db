#pragma once

#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include "catalog/schema.hpp"

enum class QueryType {
    exit,
    help,
    show_databases,
    show_tables,
    create_database,
    use_database,
    create_table,
    alter_table,
    describe_table,
    insert_row,
    select_all,
    delete_row,
    update_row,
    begin_transaction,
    commit_transaction,
    rollback_transaction,
    unknown,
};

enum class QueryCompoundOperator {
    union_op,
    intersect_op,
};

enum class QuerySetQuantifier {
    some,
    all,
};

enum class QueryOperator {
    equal,
    not_equal,
    greater,
    less,
    greater_equal,
    less_equal,
};

enum class QueryJoinType {
    cross,
    inner,
    left,
    right,
    natural,
};

enum class QueryAggregateFunction {
    none,
    max,
    min,
    avg,
    sum,
    count,
};

struct SelectedColumn {
    std::string table_alias;
    std::string column_name;
    std::string alias;
    QueryAggregateFunction aggregate = QueryAggregateFunction::none;
    std::size_t position = 0;
};

struct OrderByColumn {
    SelectedColumn column;
    bool descending = false;
};

struct ParsedQuery;

struct QueryCommonTableExpression {
    std::string name;
    std::unique_ptr<ParsedQuery> query;

    QueryCommonTableExpression() = default;
    QueryCommonTableExpression(const QueryCommonTableExpression& other);
    QueryCommonTableExpression& operator=(const QueryCommonTableExpression& other);
    QueryCommonTableExpression(QueryCommonTableExpression&& other) noexcept = default;
    QueryCommonTableExpression& operator=(QueryCommonTableExpression&& other) noexcept = default;
};

enum class QueryExpressionType {
    column_reference,
    aggregate,
    literal,
    scalar_subquery,
};

struct QueryExpression {
    QueryExpressionType type = QueryExpressionType::literal;
    std::string table_alias;
    std::string column_name;
    QueryAggregateFunction aggregate = QueryAggregateFunction::none;
    std::string literal_text;
    std::unique_ptr<ParsedQuery> subquery;
    std::size_t position = 0;

    QueryExpression() = default;
    QueryExpression(const QueryExpression& other);
    QueryExpression& operator=(const QueryExpression& other);
    QueryExpression(QueryExpression&& other) noexcept = default;
    QueryExpression& operator=(QueryExpression&& other) noexcept = default;
};

struct QueryCondition {
    QueryExpression left_expression;
    QueryOperator op = QueryOperator::equal;
    QueryExpression right_expression;
    std::vector<std::string> right_values;
    bool exists_result = false;
};

enum class QueryConditionNodeType {
    comparison,
    in_subquery,
    any_subquery,
    all_subquery,
    exists_subquery,
    not_node,
    and_node,
    or_node,
};

struct QueryConditionNode {
    QueryConditionNodeType type = QueryConditionNodeType::comparison;
    QueryCondition condition;
    std::unique_ptr<QueryConditionNode> left;
    std::unique_ptr<QueryConditionNode> right;

    QueryConditionNode() = default;
    QueryConditionNode(const QueryConditionNode& other);
    QueryConditionNode& operator=(const QueryConditionNode& other);
    QueryConditionNode(QueryConditionNode&& other) noexcept = default;
    QueryConditionNode& operator=(QueryConditionNode&& other) noexcept = default;
};

struct QueryJoin {
    QueryJoinType type = QueryJoinType::cross;
    std::string table_name;
    std::string table_alias;
    std::unique_ptr<QueryConditionNode> condition;
    std::unique_ptr<QueryJoin> next_join;

    QueryJoin() = default;
    QueryJoin(const QueryJoin& other);
    QueryJoin& operator=(const QueryJoin& other);
    QueryJoin(QueryJoin&& other) noexcept = default;
    QueryJoin& operator=(QueryJoin&& other) noexcept = default;
};

struct ParsedQuery {
    QueryType type = QueryType::unknown;
    std::string database_name;
    std::string table_name;
    std::string table_alias;
    std::unique_ptr<ParsedQuery> derived_table;
    std::vector<Column> columns;
    std::vector<ConstraintDefinition> constraints;
    std::optional<uint64_t> drop_constraint_id;
    std::vector<std::string> insert_columns;
    std::string values_text;
    std::vector<std::string> insert_value_rows;
    std::vector<std::pair<std::string, std::string>> update_assignments;
    bool select_all = true;
    std::vector<SelectedColumn> selected_columns;
    std::vector<SelectedColumn> group_by_columns;
    std::vector<OrderByColumn> order_by_columns;
    std::unique_ptr<QueryConditionNode> condition;
    std::unique_ptr<QueryConditionNode> having_condition;
    std::unique_ptr<QueryJoin> join;
    std::vector<QueryCommonTableExpression> common_table_expressions;
    std::optional<QueryCompoundOperator> compound_operator;
    QuerySetQuantifier compound_quantifier = QuerySetQuantifier::some;
    std::unique_ptr<ParsedQuery> compound_query;
    std::optional<uint64_t> limit_count;

    ParsedQuery() = default;
    ParsedQuery(const ParsedQuery& other);
    ParsedQuery& operator=(const ParsedQuery& other);
    ParsedQuery(ParsedQuery&& other) noexcept = default;
    ParsedQuery& operator=(ParsedQuery&& other) noexcept = default;
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
