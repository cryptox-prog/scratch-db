#include "cli/table_printer.hpp"
#include "test_utils.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace {

void print_bordered_table() {
    QueryResult result;
    result.columns = {
        {"id", "integer"},
        {"name", "text"},
    };
    result.rows = {
        {"1", "alice"},
        {"22", "bob"},
    };
    result.metadata.row_count = 2;

    std::ostringstream out;
    TablePrinter::print(out, result);

    const std::string expected =
        "+----+-------+\n"
        "| id | name  |\n"
        "+----+-------+\n"
        "| 1  | alice |\n"
        "| 22 | bob   |\n"
        "+----+-------+\n"
        "2 row(s)\n";

    require(out.str() == expected, "bordered table output wrong");
}

void skip_empty_table() {
    QueryResult result;
    result.columns = {
        {"database", "string"},
    };
    result.metadata.row_count = 0;

    std::ostringstream out;
    TablePrinter::print(out, result);

    require(out.str() == "0 row(s)\n", "empty table output wrong");
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"table printer bordered", print_bordered_table});
    tests.push_back({"table printer empty", skip_empty_table});
    return run_tests(tests);
}
