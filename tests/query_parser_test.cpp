#include "query/query_parser.hpp"
#include "test_utils.hpp"

#include <optional>
#include <vector>

namespace {

void parse_create_table() {
    const std::optional<ParsedQuery> query = QueryParser::parse(
        "CREATE TABLE items (id INTEGER NOT NULL, price NUMBER(4, 2) NOT NULL, name VARSTRING(32) NULL)"
    );

    require(query.has_value(), "create table did not parse");
    require(query->type == QueryType::create_table, "wrong query type");
    require(query->table_name == "items", "wrong table name");
    require(query->columns.size() == 3, "wrong column count");
    require(query->columns[1].type() == ColumnType::number, "number column type missing");
    require(query->columns[1].precision() == 4 && query->columns[1].scale() == 2, "number format missing");
}

void reject_lowercase_keywords() {
    require(!QueryParser::parse("select * from items").has_value(), "lowercase query accepted");
}

void parse_record_id_queries() {
    const std::optional<ParsedQuery> delete_query = QueryParser::parse(
        "DELETE FROM items WHERE id = 12"
    );
    require(delete_query.has_value(), "delete did not parse");
    require(delete_query->condition != nullptr, "delete condition missing");
    require(delete_query->condition->condition.left_expression.column_name == "id", "bad delete condition column");
    require(delete_query->condition->condition.op == QueryOperator::equal, "bad delete condition operator");
    require(delete_query->condition->condition.right_expression.literal_text == "12", "bad delete condition value");

    const std::optional<ParsedQuery> update_query = QueryParser::parse(
        "UPDATE items SET VALUES (1, 12.34) WHERE price >= 10.00"
    );
    require(update_query.has_value(), "update did not parse");
    require(update_query->values_text == "1, 12.34", "bad update values");
    require(update_query->condition != nullptr, "update condition missing");
    require(update_query->condition->condition.left_expression.column_name == "price", "bad update condition column");
    require(update_query->condition->condition.op == QueryOperator::greater_equal, "bad update condition operator");
    require(update_query->condition->condition.right_expression.literal_text == "10.00", "bad update condition value");

    const std::optional<ParsedQuery> assignment_query = QueryParser::parse(
        "UPDATE items SET price = 12.50, name = 'book' WHERE id = 1"
    );
    require(assignment_query.has_value(), "assignment update did not parse");
    require(assignment_query->update_assignments.size() == 2, "assignment count wrong");
    require(assignment_query->update_assignments[0].first == "price", "first assignment column wrong");
    require(assignment_query->update_assignments[0].second == "12.50", "first assignment value wrong");
    require(assignment_query->update_assignments[1].first == "name", "second assignment column wrong");
    require(assignment_query->update_assignments[1].second == "'book'", "second assignment value wrong");
}

void parse_insert_column_list() {
    const std::optional<ParsedQuery> query = QueryParser::parse(
        "INSERT INTO items (id, name) VALUES (1, 'pen')"
    );
    require(query.has_value(), "insert with column list did not parse");
    require(query->type == QueryType::insert_row, "wrong query type");
    require(query->table_name == "items", "table name wrong");
    require(query->insert_columns.size() == 2, "column list size wrong");
    require(query->insert_columns[0] == "id", "first column wrong");
    require(query->insert_columns[1] == "name", "second column wrong");
    require(query->values_text == "1, 'pen'", "insert values wrong");
}

void parse_multi_row_insert() {
    const std::optional<ParsedQuery> query = QueryParser::parse(
        "INSERT INTO items VALUES (1, 'pen'), (2, 'book')"
    );
    require(query.has_value(), "multi-row insert did not parse");
    require(query->type == QueryType::insert_row, "wrong query type");
    require(query->insert_value_rows.size() == 2, "row list count wrong");
    require(query->insert_value_rows[0] == "1, 'pen'", "first row values wrong");
    require(query->insert_value_rows[1] == "2, 'book'", "second row values wrong");
}

void report_lowercase_keyword() {
    const ParseResult result = QueryParser::parse_with_error("select * FROM items");
    require(!result.ok(), "bad query accepted");
    require(result.error->message == "keyword must be uppercase", "wrong lowercase error");
    require(result.error->token == "select", "wrong lowercase token");
    require(result.error->position == 0, "wrong lowercase position");
}

void report_missing_parenthesis() {
    const ParseResult result = QueryParser::parse_with_error("CREATE TABLE items (id INTEGER");
    require(!result.ok(), "bad parenthesis query accepted");
    require(result.error->message == "missing closing parenthesis", "wrong parenthesis error");
    require(result.error->token == "(", "wrong parenthesis token");
}

void report_bad_column_definition() {
    const ParseResult result = QueryParser::parse_with_error("CREATE TABLE items (BadName INTEGER)");
    require(!result.ok(), "bad column query accepted");
    require(result.error->message == "column name must be lowercase identifier", "wrong column error");
    require(result.error->token == "BadName", "wrong column token");
}

void report_bad_create_keyword() {
    const ParseResult result = QueryParser::parse_with_error("CREATE DATABSE db1");
    require(!result.ok(), "bad create query accepted");
    require(result.error->message == "expected DATABASE or TABLE", "wrong create keyword error");
    require(result.error->token == "DATABSE", "wrong create keyword token");
    require(result.error->position == 7, "wrong create keyword position");
}

void parse_select_where() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT * FROM items WHERE sold_on <= '2026-08-30'");
    require(query.has_value(), "select where did not parse");
    require(query->type == QueryType::select_all, "wrong select where type");
    require(query->condition != nullptr, "select condition missing");
    require(query->condition->condition.left_expression.column_name == "sold_on", "select condition column wrong");
    require(query->condition->condition.op == QueryOperator::less_equal, "select condition operator wrong");
    require(query->condition->condition.right_expression.literal_text == "'2026-08-30'", "select condition value wrong");
}

void parse_where_and_or() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT * FROM items WHERE id = 1 OR price >= 10.00 AND name != 'pen'");
    require(query.has_value(), "and/or where did not parse");
    require(query->condition != nullptr, "and/or condition missing");
    require(query->condition->type == QueryConditionNodeType::or_node, "or should be root");
    require(query->condition->left->condition.left_expression.column_name == "id", "left condition wrong");
    require(query->condition->right->type == QueryConditionNodeType::and_node, "and should bind tighter");
}

void parse_grouped_where_conditions() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT * FROM items WHERE (id = 1 OR price >= 10.00) AND name != 'pen'");
    require(query.has_value(), "grouped and/or where did not parse");
    require(query->condition != nullptr, "grouped condition missing");
    require(query->condition->type == QueryConditionNodeType::and_node, "and should be root after grouping");
    require(query->condition->left->type == QueryConditionNodeType::or_node, "grouped OR should be left child");
    require(query->condition->right->condition.left_expression.column_name == "name", "right condition wrong");
}

void parse_compound_selects() {
    const std::optional<ParsedQuery> query = QueryParser::parse(
        "SELECT * FROM a WHERE id = 1 UNION ALL SELECT * FROM b INTERSECT SOME SELECT * FROM c WHERE name = 'x'"
    );
    require(query.has_value(), "compound select did not parse");
    require(query->compound_operator == QueryCompoundOperator::union_op, "union operator missing");
    require(query->compound_quantifier == QuerySetQuantifier::all, "union all missing");
    require(query->compound_query != nullptr, "second select missing");
    require(query->compound_query->table_name == "b", "second select table wrong");
    require(query->compound_query->compound_operator == QueryCompoundOperator::intersect_op, "intersect operator missing");
    require(query->compound_query->compound_quantifier == QuerySetQuantifier::some, "intersect some missing");
    require(query->compound_query->compound_query != nullptr, "third select missing");
    require(query->compound_query->compound_query->table_name == "c", "third select table wrong");
}

void parse_select_table_alias() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT * FROM items AS i WHERE id = 1");
    require(query.has_value(), "select table alias did not parse");
    require(query->table_name == "items", "table alias should keep actual table name");
    require(query->table_alias == "i", "table alias missing");
    require(query->condition != nullptr, "alias query condition missing");
}

void parse_join_variants() {
    const std::optional<ParsedQuery> cartesian = QueryParser::parse("SELECT * FROM a, b");
    require(cartesian.has_value(), "cartesian product did not parse");
    require(cartesian->join != nullptr, "cartesian join missing");
    require(cartesian->join->type == QueryJoinType::cross, "cross join type wrong");
    require(cartesian->join->table_name == "b", "cross join target wrong");

    const std::optional<ParsedQuery> inner = QueryParser::parse("SELECT * FROM a JOIN b ON a.id = b.id");
    require(inner.has_value(), "join did not parse");
    require(inner->join != nullptr, "join clause missing");
    require(inner->join->type == QueryJoinType::inner, "join type wrong");
    require(inner->join->condition != nullptr, "join condition missing");

    const std::optional<ParsedQuery> left = QueryParser::parse("SELECT * FROM a LEFT JOIN b ON a.id = b.id");
    require(left.has_value(), "left join did not parse");
    require(left->join != nullptr, "left join clause missing");
    require(left->join->type == QueryJoinType::left, "left join type wrong");

    const std::optional<ParsedQuery> natural = QueryParser::parse("SELECT * FROM a NATURAL JOIN b");
    require(natural.has_value(), "natural join did not parse");
    require(natural->join != nullptr, "natural join clause missing");
    require(natural->join->type == QueryJoinType::natural, "natural join type wrong");

    const std::optional<ParsedQuery> chained = QueryParser::parse("SELECT * FROM a JOIN b ON a.id = b.id JOIN c ON b.id = c.id");
    require(chained.has_value(), "chained join did not parse");
    require(chained->join != nullptr, "chained join missing");
    require(chained->join->next_join != nullptr, "chained join chain missing");
    require(chained->join->next_join->table_name == "c", "chained join target wrong");
}

void parse_qualified_select_column() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT a.id FROM items AS a");
    require(query.has_value(), "qualified select did not parse");
    require(!query->select_all, "qualified select should not be wildcard");
    require(query->selected_columns.size() == 1, "selected column count wrong");
    require(query->selected_columns[0].table_alias == "a", "qualified alias missing");
    require(query->selected_columns[0].column_name == "id", "qualified column name wrong");
}

void parse_column_alias() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT price AS unit_price, MAX(price) AS max_price FROM items ORDER BY unit_price DESC");
    require(query.has_value(), "column alias select did not parse");
    require(query->selected_columns.size() == 2, "selected column count wrong");
    require(query->selected_columns[0].alias == "unit_price", "first alias wrong");
    require(query->selected_columns[1].alias == "max_price", "second alias wrong");
    require(query->order_by_columns.size() == 1, "order by alias count wrong");
    require(query->order_by_columns[0].column.column_name == "unit_price", "order by alias wrong");
}

void parse_aggregate_and_scalar_subquery() {
    const std::optional<ParsedQuery> aggregate = QueryParser::parse("SELECT MAX(price) FROM items");
    require(aggregate.has_value(), "aggregate select did not parse");
    require(!aggregate->select_all, "aggregate select should not be wildcard");
    require(aggregate->selected_columns.size() == 1, "aggregate select count wrong");
    require(aggregate->selected_columns[0].aggregate == QueryAggregateFunction::max, "aggregate function wrong");
    require(aggregate->selected_columns[0].column_name == "price", "aggregate column wrong");

    const std::optional<ParsedQuery> subquery = QueryParser::parse("SELECT * FROM items WHERE price > (SELECT MAX(price) FROM old_items)");
    require(subquery.has_value(), "scalar subquery did not parse");
    require(subquery->condition != nullptr, "scalar subquery condition missing");
    require(subquery->condition->condition.right_expression.subquery != nullptr, "scalar subquery missing from condition");
    require(subquery->condition->condition.right_expression.subquery->selected_columns[0].aggregate == QueryAggregateFunction::max, "scalar aggregate wrong");
}

void parse_group_by() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT category, MAX(price) FROM items GROUP BY category");
    require(query.has_value(), "group by did not parse");
    require(query->group_by_columns.size() == 1, "group by column count wrong");
    require(query->group_by_columns[0].column_name == "category", "group by column wrong");
    require(query->selected_columns.size() == 2, "group select column count wrong");
    require(query->selected_columns[1].aggregate == QueryAggregateFunction::max, "group aggregate wrong");
}

void parse_in_and_exists_subqueries() {
    const std::optional<ParsedQuery> in_query = QueryParser::parse("SELECT * FROM items WHERE id IN (SELECT id FROM old_items)");
    require(in_query.has_value(), "in subquery did not parse");
    require(in_query->condition != nullptr, "in condition missing");
    require(in_query->condition->type == QueryConditionNodeType::in_subquery, "in condition type wrong");
    require(in_query->condition->condition.left_expression.column_name == "id", "in left column wrong");
    require(in_query->condition->condition.right_expression.subquery != nullptr, "in subquery missing");

    const std::optional<ParsedQuery> not_in_query = QueryParser::parse("SELECT * FROM items WHERE id NOT IN (SELECT id FROM old_items)");
    require(not_in_query.has_value(), "not in subquery did not parse");
    require(not_in_query->condition != nullptr, "not in condition missing");
    require(not_in_query->condition->type == QueryConditionNodeType::not_node, "not in should be wrapped in not node");
    require(not_in_query->condition->left->type == QueryConditionNodeType::in_subquery, "not in child should be in subquery");

    const std::optional<ParsedQuery> any_query = QueryParser::parse("SELECT * FROM items WHERE price > ANY (SELECT price FROM old_items)");
    require(any_query.has_value(), "any subquery did not parse");
    require(any_query->condition != nullptr, "any condition missing");
    require(any_query->condition->type == QueryConditionNodeType::any_subquery, "any condition type wrong");
    require(any_query->condition->condition.op == QueryOperator::greater, "any operator wrong");

    const std::optional<ParsedQuery> exists_query = QueryParser::parse("SELECT * FROM items WHERE NOT EXISTS (SELECT * FROM old_items)");
    require(exists_query.has_value(), "exists subquery did not parse");
    require(exists_query->condition != nullptr, "exists condition missing");
    require(exists_query->condition->type == QueryConditionNodeType::not_node, "not condition type wrong");
    require(exists_query->condition->left->type == QueryConditionNodeType::exists_subquery, "exists condition type wrong");
    require(exists_query->condition->left->condition.right_expression.subquery != nullptr, "exists subquery missing");
}

void parse_derived_table() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT x.id FROM (SELECT id FROM items) AS x");
    require(query.has_value(), "derived table did not parse");
    require(query->derived_table != nullptr, "derived query missing");
    require(query->table_alias == "x", "derived alias wrong");
    require(query->derived_table->table_name == "items", "derived source table wrong");
}

void parse_with_queries() {
    const std::optional<ParsedQuery> query = QueryParser::parse("WITH picked AS (SELECT id FROM items) SELECT * FROM picked");
    require(query.has_value(), "with query did not parse");
    require(query->common_table_expressions.size() == 1, "with cte count wrong");
    require(query->common_table_expressions[0].name == "picked", "with cte name wrong");
    require(query->common_table_expressions[0].query != nullptr, "with cte query missing");
    require(query->table_name == "picked", "with main table wrong");

    const std::optional<ParsedQuery> multi = QueryParser::parse(
        "WITH base AS (SELECT id FROM items), picked AS (SELECT id FROM base) SELECT * FROM picked"
    );
    require(multi.has_value(), "multi with query did not parse");
    require(multi->common_table_expressions.size() == 2, "multi with cte count wrong");
    require(multi->common_table_expressions[1].name == "picked", "multi with second cte wrong");
}

void parse_order_by() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT id, price FROM items ORDER BY price DESC, id ASC");
    require(query.has_value(), "order by did not parse");
    require(query->order_by_columns.size() == 2, "order by column count wrong");
    require(query->order_by_columns[0].column.column_name == "price", "first order column wrong");
    require(query->order_by_columns[0].descending, "first order direction wrong");
    require(query->order_by_columns[1].column.column_name == "id", "second order column wrong");
    require(!query->order_by_columns[1].descending, "second order direction wrong");
}

void parse_having_and_limit() {
    const std::optional<ParsedQuery> query = QueryParser::parse("SELECT category, COUNT(*) FROM items GROUP BY category HAVING COUNT(*) > 1 ORDER BY category DESC LIMIT 2");
    require(query.has_value(), "having limit query did not parse");
    require(query->having_condition != nullptr, "having condition missing");
    require(query->having_condition->condition.left_expression.type == QueryExpressionType::aggregate, "having aggregate missing");
    require(query->having_condition->condition.left_expression.aggregate == QueryAggregateFunction::count, "having aggregate wrong");
    require(query->order_by_columns.size() == 1, "having order count wrong");
    require(query->limit_count.has_value() && *query->limit_count == 2, "limit count wrong");
}

void reject_invalid_table_aliases() {
    require(!QueryParser::parse("SELECT * FROM items AS Item").has_value(), "uppercase alias accepted");
    require(!QueryParser::parse("SELECT * FROM items AS 2items").has_value(), "digit-leading alias accepted");
    require(!QueryParser::parse("SELECT * FROM items AS item-name").has_value(), "special-character alias accepted");
}

void report_missing_string_size() {
    const ParseResult result = QueryParser::parse_with_error("CREATE TABLE tbk (id INTEGER, name STRING)");
    require(!result.ok(), "bare string query accepted");
    require(result.error->message == "STRING requires a size, use STRING(n)", "wrong string size error");
    require(result.error->token == "STRING", "wrong string size token");
}

void report_missing_varstring_size() {
    const ParseResult result = QueryParser::parse_with_error("CREATE TABLE tbk (id INTEGER, name VARSTRING)");
    require(!result.ok(), "bare varstring query accepted");
    require(result.error->message == "VARSTRING requires a size, use VARSTRING(n)", "wrong varstring size error");
    require(result.error->token == "VARSTRING", "wrong varstring size token");
}

void parse_constraint_syntax() {
    const std::optional<ParsedQuery> query = QueryParser::parse(
        "CREATE TABLE items (id INTEGER NOT NULL PRIMARY KEY, category_id INTEGER NOT NULL REFERENCES categories(id), price NUMBER(4, 2) NOT NULL CHECK (price > 0), name VARSTRING(16) UNIQUE)"
    );
    require(query.has_value(), "constraint create table did not parse");
    require(query->constraints.size() == 4, "constraint count wrong");
    require(query->constraints[0].kind == "primary_key", "primary key kind wrong");
    require(query->constraints[0].columns[0] == "id", "primary key column wrong");
    require(query->constraints[1].kind == "foreign_key", "foreign key kind wrong");
    require(query->constraints[2].kind == "check", "check kind wrong");
    require(query->constraints[2].args[0] == "price", "check left column wrong");
    require(query->constraints[3].kind == "unique", "unique kind wrong");

    const std::optional<ParsedQuery> stacked = QueryParser::parse(
        "CREATE TABLE children (id INTEGER PRIMARY KEY, parent_id INTEGER UNIQUE REFERENCES parents(id) CHECK (parent_id > 0))"
    );
    require(stacked.has_value(), "stacked column constraints did not parse");
    require(stacked->constraints.size() == 4, "stacked column constraint count wrong");
    require(stacked->constraints[1].kind == "unique", "stacked unique kind wrong");
    require(stacked->constraints[2].kind == "foreign_key", "stacked foreign key kind wrong");
    require(stacked->constraints[3].kind == "check", "stacked check kind wrong");
}

void report_missing_number_format() {
    const ParseResult result = QueryParser::parse_with_error("CREATE TABLE tbk (id INTEGER, price NUMBER)");
    require(!result.ok(), "bare number query accepted");
    require(result.error->message == "NUMBER requires precision and scale, use NUMBER(p, s)", "wrong number format error");
    require(result.error->token == "NUMBER", "wrong number format token");
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"parser create table", parse_create_table});
    tests.push_back({"parser case strict", reject_lowercase_keywords});
    tests.push_back({"parser record ids", parse_record_id_queries});
    tests.push_back({"parser insert column list", parse_insert_column_list});
    tests.push_back({"parser multi row insert", parse_multi_row_insert});
    tests.push_back({"parser lowercase error", report_lowercase_keyword});
    tests.push_back({"parser parenthesis error", report_missing_parenthesis});
    tests.push_back({"parser column error", report_bad_column_definition});
    tests.push_back({"parser create keyword error", report_bad_create_keyword});
    tests.push_back({"parser select where", parse_select_where});
    tests.push_back({"parser where and or", parse_where_and_or});
    tests.push_back({"parser grouped where", parse_grouped_where_conditions});
    tests.push_back({"parser compound selects", parse_compound_selects});
    tests.push_back({"parser table alias", parse_select_table_alias});
    tests.push_back({"parser join variants", parse_join_variants});
    tests.push_back({"parser qualified select", parse_qualified_select_column});
    tests.push_back({"parser column alias", parse_column_alias});
    tests.push_back({"parser aggregate and scalar subquery", parse_aggregate_and_scalar_subquery});
    tests.push_back({"parser group by", parse_group_by});
    tests.push_back({"parser in and exists subqueries", parse_in_and_exists_subqueries});
    tests.push_back({"parser derived table", parse_derived_table});
    tests.push_back({"parser with queries", parse_with_queries});
    tests.push_back({"parser order by", parse_order_by});
    tests.push_back({"parser having and limit", parse_having_and_limit});
    tests.push_back({"parser invalid table alias", reject_invalid_table_aliases});
    tests.push_back({"parser string size error", report_missing_string_size});
    tests.push_back({"parser varstring size error", report_missing_varstring_size});
    tests.push_back({"parser constraint syntax", parse_constraint_syntax});
    tests.push_back({"parser number format error", report_missing_number_format});
    return run_tests(tests);
}
