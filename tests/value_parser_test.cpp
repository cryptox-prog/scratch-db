#include "query/value_parser.hpp"
#include "test_utils.hpp"

#include <optional>
#include <vector>

namespace {

Schema make_schema() {
    std::vector<Column> columns;
    columns.push_back(Column::integer_column("id", false));
    columns.push_back(Column::number_column("price", false, 4, 2));
    columns.push_back(Column::date_column("sold_on", false));
    columns.push_back(Column::varstring_column("note", true, 32));
    return Schema("sales", columns);
}

void parse_valid_row() {
    const Schema schema = make_schema();
    const std::optional<Row> row = ValueParser::parse_row(schema, "1, 12.34, '2026-08-30', NULL");

    require(row.has_value(), "valid row rejected");
    require(row->value_count() == 4, "wrong value count");
    require(row->value(0)->integer_data() == 1, "integer parsed incorrectly");
    require(row->value(1)->number_data() == 1234, "number parsed incorrectly");
    require(row->value(3)->is_null(), "null parsed incorrectly");
}

void reject_bad_number_scale() {
    const Schema schema = make_schema();
    const ValueParseResult result = ValueParser::parse_row_with_error(schema, "1, 12.345, '2026-08-30', NULL");
    require(!result.ok(), "bad scale accepted");
    require(result.error->message == "invalid NUMBER value for column price", "bad scale error message wrong");
    require(result.error->token == "12.345", "bad scale token wrong");
}

void reject_unquoted_date() {
    const Schema schema = make_schema();
    const ValueParseResult result = ValueParser::parse_row_with_error(schema, "1, 12.34, 2026-08-30, NULL");
    require(!result.ok(), "unquoted date accepted");
    require(result.error->message == "date value for column sold_on must be quoted", "unquoted date error message wrong");
    require(result.error->token == "2026-08-30", "unquoted date token wrong");
}

void reject_null_not_null_column() {
    const Schema schema = make_schema();
    const ValueParseResult result = ValueParser::parse_row_with_error(schema, "1, NULL, '2026-08-30', NULL");
    require(!result.ok(), "null in not null column accepted");
    require(result.error->message == "NULL is not allowed for column price", "null error message wrong");
    require(result.error->token == "NULL", "null token wrong");
}

void reject_wrong_value_count() {
    const Schema schema = make_schema();
    const ValueParseResult result = ValueParser::parse_row_with_error(schema, "1, 12.34");
    require(!result.ok(), "wrong value count accepted");
    require(result.error->message == "wrong value count: expected 4, got 2", "value count error message wrong");
}

void format_values() {
    const Column price = Column::number_column("price", false, 4, 2);
    const Column note = Column::varstring_column("note", true, 32);

    require(ValueParser::format_value(price, Value::number_value(1200)) == "12.00", "number formatted incorrectly");
    require(ValueParser::format_value(note, Value::null_value()) == "NULL", "null formatted incorrectly");
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"value parser valid row", parse_valid_row});
    tests.push_back({"value parser number scale", reject_bad_number_scale});
    tests.push_back({"value parser quoted date", reject_unquoted_date});
    tests.push_back({"value parser nullability", reject_null_not_null_column});
    tests.push_back({"value parser count", reject_wrong_value_count});
    tests.push_back({"value parser format", format_values});
    return run_tests(tests);
}
