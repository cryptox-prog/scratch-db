#include "cli/table_printer.hpp"

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace {
    std::vector<std::size_t> column_widths(const QueryResult& result) {
        std::vector<std::size_t> widths;
        widths.reserve(result.columns.size());
        for (const QueryResultColumn& column : result.columns) {
            widths.push_back(column.name.size());
        }

        for (const std::vector<std::string>& row : result.rows) {
            for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
                widths[i] = std::max(widths[i], row[i].size());
            }
        }

        return widths;
    }

    void print_border(std::ostream& out, const std::vector<std::size_t>& widths) {
        out << "+";
        for (std::size_t width : widths) {
            out << std::string(width + 2, '-') << "+";
        }
        out << "\n";
    }

    void print_cells(std::ostream& out, const std::vector<std::string>& cells, const std::vector<std::size_t>& widths) {
        out << "|";
        for (std::size_t i = 0; i < widths.size(); ++i) {
            const std::string value = i < cells.size() ? cells[i] : "";
            out << " " << std::left << std::setw(static_cast<int>(widths[i])) << value << " |";
        }
        out << "\n";
    }
}

void TablePrinter::print(std::ostream& out, const QueryResult& result) {
    if (result.rows.empty()) {
        out << result.metadata.row_count << " row(s)\n";
        return;
    }

    const std::vector<std::size_t> widths = column_widths(result);

    print_border(out, widths);

    std::vector<std::string> header;
    header.reserve(result.columns.size());
    for (const QueryResultColumn& column : result.columns) {
        header.push_back(column.name);
    }
    print_cells(out, header, widths);

    print_border(out, widths);
    for (const std::vector<std::string>& row : result.rows) {
        print_cells(out, row, widths);
    }
    print_border(out, widths);

    out << result.metadata.row_count << " row(s)\n";
}
