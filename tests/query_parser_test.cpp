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
    require(delete_query->condition.has_value(), "delete condition missing");
    require(delete_query->condition->column_name == "id", "bad delete condition column");
    require(delete_query->condition->op == QueryOperator::equal, "bad delete condition operator");
    require(delete_query->condition->value_text == "12", "bad delete condition value");

    const std::optional<ParsedQuery> update_query = QueryParser::parse(
        "UPDATE items SET VALUES (1, 12.34) WHERE price >= 10.00"
    );
    require(update_query.has_value(), "update did not parse");
    require(update_query->values_text == "1, 12.34", "bad update values");
    require(update_query->condition.has_value(), "update condition missing");
    require(update_query->condition->column_name == "price", "bad update condition column");
    require(update_query->condition->op == QueryOperator::greater_equal, "bad update condition operator");
    require(update_query->condition->value_text == "10.00", "bad update condition value");
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
    require(query->condition.has_value(), "select condition missing");
    require(query->condition->column_name == "sold_on", "select condition column wrong");
    require(query->condition->op == QueryOperator::less_equal, "select condition operator wrong");
    require(query->condition->value_text == "'2026-08-30'", "select condition value wrong");
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
    tests.push_back({"parser lowercase error", report_lowercase_keyword});
    tests.push_back({"parser parenthesis error", report_missing_parenthesis});
    tests.push_back({"parser column error", report_bad_column_definition});
    tests.push_back({"parser create keyword error", report_bad_create_keyword});
    tests.push_back({"parser select where", parse_select_where});
    tests.push_back({"parser string size error", report_missing_string_size});
    tests.push_back({"parser varstring size error", report_missing_varstring_size});
    tests.push_back({"parser number format error", report_missing_number_format});
    return run_tests(tests);
}
