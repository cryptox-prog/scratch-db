#pragma once

#include <iosfwd>

#include "query/query_executor.hpp"

class TablePrinter {
public:
    static void print(std::ostream& out, const QueryResult& result);
};
