#include "query/query_parser.hpp"

#include <cctype>
#include <cstdint>
#include <regex>

#include "common/constants.hpp"

namespace {
    struct TextPart {
        std::string text;
        std::size_t position = 0;
    };

    struct Token {
        std::string text;
        std::size_t position = 0;
    };

    ParseResult parsed_result(const ParsedQuery& query) {
        ParseResult result;
        result.query = query;
        return result;
    }

    ParseResult parse_error(const std::string& message, const std::string& token, std::size_t position) {
        ParseResult result;
        result.error = ParseError{message, token, position};
        return result;
    }

    std::string trim(const std::string& text) {
        std::size_t start = 0;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
            ++start;
        }

        std::size_t end = text.size();
        while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
            --end;
        }

        return text.substr(start, end - start);
    }

    std::size_t first_non_space(const std::string& text) {
        std::size_t position = 0;
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) {
            ++position;
        }
        return position;
    }

    std::string first_word(const std::string& text, std::size_t& position) {
        position = first_non_space(text);
        std::size_t end = position;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '(' && text[end] != ')') {
            ++end;
        }
        return text.substr(position, end - position);
    }

    bool has_lowercase(const std::string& text) {
        for (char ch : text) {
            if (std::islower(static_cast<unsigned char>(ch))) {
                return true;
            }
        }
        return false;
    }

    std::vector<Token> tokenize(const std::string& command) {
        std::vector<Token> tokens;
        std::size_t i = 0;
        while (i < command.size()) {
            while (i < command.size() && std::isspace(static_cast<unsigned char>(command[i]))) {
                ++i;
            }
            if (i == command.size()) {
                break;
            }

            const std::size_t start = i;
            if (command[i] == '(' || command[i] == ')' || command[i] == ',' || command[i] == '=' || command[i] == '*') {
                tokens.push_back({command.substr(i, 1), i});
                ++i;
                continue;
            }

            if (command[i] == '\'') {
                ++i;
                while (i < command.size() && command[i] != '\'') {
                    ++i;
                }
                if (i < command.size()) {
                    ++i;
                }
                tokens.push_back({command.substr(start, i - start), start});
                continue;
            }

            while (i < command.size() &&
                   !std::isspace(static_cast<unsigned char>(command[i])) &&
                   command[i] != '(' &&
                   command[i] != ')' &&
                   command[i] != ',' &&
                   command[i] != '=' &&
                   command[i] != '*') {
                ++i;
            }
            tokens.push_back({command.substr(start, i - start), start});
        }
        return tokens;
    }

    ParseResult expected_error(const std::string& expected, const std::vector<Token>& tokens, std::size_t index, const std::string& command) {
        if (index < tokens.size()) {
            return parse_error("expected " + expected, tokens[index].text, tokens[index].position);
        }
        return parse_error("expected " + expected, "", command.size());
    }

    std::optional<ParseResult> diagnose_keyword_sequence(const std::string& command, const std::vector<Token>& tokens) {
        if (tokens.empty()) {
            return std::nullopt;
        }

        if (tokens[0].text == "CREATE") {
            if (tokens.size() < 2 || (tokens[1].text != "DATABASE" && tokens[1].text != "TABLE")) {
                return expected_error("DATABASE or TABLE", tokens, 1, command);
            }
        } else if (tokens[0].text == "SHOW") {
            if (tokens.size() < 2 || (tokens[1].text != "DATABASES" && tokens[1].text != "TABLES")) {
                return expected_error("DATABASES or TABLES", tokens, 1, command);
            }
        } else if (tokens[0].text == "INSERT") {
            if (tokens.size() < 2 || tokens[1].text != "INTO") {
                return expected_error("INTO", tokens, 1, command);
            }
            if (tokens.size() < 4 || tokens[3].text != "VALUES") {
                return expected_error("VALUES", tokens, 3, command);
            }
        } else if (tokens[0].text == "SELECT") {
            if (tokens.size() < 2 || tokens[1].text != "*") {
                return expected_error("*", tokens, 1, command);
            }
            if (tokens.size() < 3 || tokens[2].text != "FROM") {
                return expected_error("FROM", tokens, 2, command);
            }
        } else if (tokens[0].text == "DELETE") {
            if (tokens.size() < 2 || tokens[1].text != "FROM") {
                return expected_error("FROM", tokens, 1, command);
            }
            if (tokens.size() < 4 || tokens[3].text != "WHERE") {
                return expected_error("WHERE", tokens, 3, command);
            }
        } else if (tokens[0].text == "UPDATE") {
            if (tokens.size() < 3 || tokens[2].text != "SET") {
                return expected_error("SET", tokens, 2, command);
            }
        }

        return std::nullopt;
    }

    std::optional<ParseResult> diagnose_lowercase_keyword(const std::vector<Token>& tokens) {
        const std::vector<std::string> keywords = {
            "exit", "quit", "help", "show", "databases", "tables", "create", "database", "table",
            "use", "describe", "desc", "insert", "into", "values", "select", "from", "delete",
            "where", "and", "update", "set", "null", "not"
        };

        for (const Token& token : tokens) {
            for (const std::string& keyword : keywords) {
                if (token.text == keyword) {
                    return parse_error("keyword must be uppercase", token.text, token.position);
                }
            }
        }
        return std::nullopt;
    }

    std::optional<ParseError> delimiter_error(const std::string& command) {
        bool in_string = false;
        std::vector<std::size_t> parens;
        for (std::size_t i = 0; i < command.size(); ++i) {
            const char ch = command[i];
            if (ch == '\'') {
                in_string = !in_string;
            } else if (ch == '(' && !in_string) {
                parens.push_back(i);
            } else if (ch == ')' && !in_string) {
                if (parens.empty()) {
                    return ParseError{"unexpected closing parenthesis", ")", i};
                }
                parens.pop_back();
            }
        }

        if (in_string) {
            return ParseError{"unterminated string literal", "'", command.rfind('\'')};
        }
        if (!parens.empty()) {
            return ParseError{"missing closing parenthesis", "(", parens.back()};
        }
        return std::nullopt;
    }

    std::vector<std::string> split_csv(const std::string& text) {
        std::vector<std::string> parts;
        std::string current;
        bool in_string = false;
        uint16_t paren_depth = 0;

        for (char ch : text) {
            if (ch == '\'') {
                in_string = !in_string;
                current.push_back(ch);
            } else if (ch == '(' && !in_string) {
                ++paren_depth;
                current.push_back(ch);
            } else if (ch == ')' && !in_string && paren_depth > 0) {
                --paren_depth;
                current.push_back(ch);
            } else if (ch == ',' && !in_string && paren_depth == 0) {
                parts.push_back(trim(current));
                current.clear();
            } else {
                current.push_back(ch);
            }
        }

        if (!current.empty() || !text.empty()) {
            parts.push_back(trim(current));
        }

        return parts;
    }

    std::vector<TextPart> split_csv_with_positions(const std::string& text, std::size_t base_position) {
        std::vector<TextPart> parts;
        std::string current;
        std::size_t current_position = base_position;
        bool in_string = false;
        uint16_t paren_depth = 0;

        for (std::size_t i = 0; i < text.size(); ++i) {
            const char ch = text[i];
            if (ch == '\'') {
                in_string = !in_string;
                current.push_back(ch);
            } else if (ch == '(' && !in_string) {
                ++paren_depth;
                current.push_back(ch);
            } else if (ch == ')' && !in_string && paren_depth > 0) {
                --paren_depth;
                current.push_back(ch);
            } else if (ch == ',' && !in_string && paren_depth == 0) {
                const std::size_t local_start = first_non_space(current);
                parts.push_back({trim(current), current_position + local_start});
                current.clear();
                current_position = base_position + i + 1;
            } else {
                current.push_back(ch);
            }
        }

        if (!current.empty() || !text.empty()) {
            const std::size_t local_start = first_non_space(current);
            parts.push_back({trim(current), current_position + local_start});
        }

        return parts;
    }

    bool parse_uint16(const std::string& text, uint16_t& value) {
        try {
            std::size_t used = 0;
            const unsigned long parsed = std::stoul(text, &used);
            if (used != text.size() || parsed > UINT16_MAX) {
                return false;
            }
            value = static_cast<uint16_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::optional<QueryOperator> parse_operator(const std::string& text) {
        if (text == "=") {
            return QueryOperator::equal;
        }
        if (text == "!=") {
            return QueryOperator::not_equal;
        }
        if (text == ">") {
            return QueryOperator::greater;
        }
        if (text == "<") {
            return QueryOperator::less;
        }
        if (text == ">=") {
            return QueryOperator::greater_equal;
        }
        if (text == "<=") {
            return QueryOperator::less_equal;
        }
        return std::nullopt;
    }

    std::optional<QueryCondition> parse_condition(
        const std::string& column_name,
        const std::string& op_text,
        const std::string& value_text,
        const std::string& command
    ) {
        std::optional<QueryOperator> op = parse_operator(op_text);
        if (!op.has_value()) {
            return std::nullopt;
        }

        QueryCondition condition;
        condition.column_name = column_name;
        condition.op = *op;
        condition.value_text = trim(value_text);
        condition.column_position = command.find(column_name);
        condition.value_position = command.find(condition.value_text, condition.column_position);
        if (condition.column_position == std::string::npos || condition.value_position == std::string::npos) {
            return std::nullopt;
        }
        return condition;
    }

    std::optional<Column> parse_column_definition(const std::string& definition) {
        const std::regex column_regex(R"(^\s*([a-z_][a-z0-9_]*)\s+(INTEGER|NUMBER\s*\(\s*[0-9]+\s*,\s*[0-9]+\s*\)|CHAR|STRING\s*\(\s*[0-9]+\s*\)|VARSTRING\s*\(\s*[0-9]+\s*\)|DATE|TIME|DATETIME|TEXT)\s*(NULL|NOT\s+NULL)?\s*$)");
        const std::regex number_regex(R"(NUMBER\s*\(\s*([0-9]+)\s*,\s*([0-9]+)\s*\))");
        const std::regex string_regex(R"(STRING\s*\(\s*([0-9]+)\s*\))");
        const std::regex varstring_regex(R"(VARSTRING\s*\(\s*([0-9]+)\s*\))");
        std::smatch match;
        if (!std::regex_match(definition, match, column_regex)) {
            return std::nullopt;
        }

        const std::string name = match[1].str();
        const std::string type_text = match[2].str();
        const std::string null_text = match[3].str();
        const bool nullable = !std::regex_match(null_text, std::regex(R"(NOT\s+NULL)"));

        try {
            if (type_text == "INTEGER") {
                return Column::integer_column(name, nullable);
            }
            if (type_text == "CHAR") {
                return Column::char_column(name, nullable);
            }
            if (type_text == "DATE") {
                return Column::date_column(name, nullable);
            }
            if (type_text == "TIME") {
                return Column::time_column(name, nullable);
            }
            if (type_text == "DATETIME") {
                return Column::datetime_column(name, nullable);
            }
            if (type_text == "TEXT") {
                return Column::text_column(name, nullable);
            }

            std::smatch size_match;
            if (std::regex_match(type_text, size_match, number_regex)) {
                uint16_t precision = 0;
                uint16_t scale = 0;
                if (!parse_uint16(size_match[1].str(), precision) ||
                    !parse_uint16(size_match[2].str(), scale) ||
                    precision > LIMITS::MAX_NUMBER_PRECISION ||
                    scale > precision) {
                    return std::nullopt;
                }
                return Column::number_column(name, nullable, static_cast<uint8_t>(precision), static_cast<uint8_t>(scale));
            }
            if (std::regex_match(type_text, size_match, string_regex)) {
                uint16_t max_size = 0;
                if (!parse_uint16(size_match[1].str(), max_size)) {
                    return std::nullopt;
                }
                return Column::string_column(name, nullable, max_size);
            }
            if (std::regex_match(type_text, size_match, varstring_regex)) {
                uint16_t max_size = 0;
                if (!parse_uint16(size_match[1].str(), max_size)) {
                    return std::nullopt;
                }
                return Column::varstring_column(name, nullable, max_size);
            }
        } catch (...) {
            return std::nullopt;
        }

        return std::nullopt;
    }

    ParseResult diagnose_column_definition(const TextPart& definition) {
        const std::vector<Token> tokens = tokenize(definition.text);
        if (tokens.size() < 2) {
            return parse_error("expected column type", definition.text, definition.position);
        }

        const Token& name = tokens[0];
        const Token& type = tokens[1];
        if (!Column::is_valid_name(name.text)) {
            return parse_error("column name must be lowercase identifier", name.text, definition.position + name.position);
        }

        if (type.text == "STRING") {
            return parse_error("STRING requires a size, use STRING(n)", type.text, definition.position + type.position);
        }
        if (type.text == "VARSTRING") {
            return parse_error("VARSTRING requires a size, use VARSTRING(n)", type.text, definition.position + type.position);
        }
        if (type.text == "NUMBER") {
            return parse_error("NUMBER requires precision and scale, use NUMBER(p, s)", type.text, definition.position + type.position);
        }

        if (type.text == "STRING" || type.text == "VARSTRING" || type.text == "NUMBER") {
            return parse_error("invalid type format", type.text, definition.position + type.position);
        }

        const std::vector<std::string> valid_types = {
            "INTEGER", "NUMBER", "CHAR", "STRING", "VARSTRING", "DATE", "TIME", "DATETIME", "TEXT"
        };
        for (const std::string& valid_type : valid_types) {
            if (type.text == valid_type) {
                return parse_error("invalid column definition", definition.text, definition.position);
            }
        }

        return parse_error("unknown column type", type.text, definition.position + type.position);
    }

    std::optional<ParsedQuery> parse_internal(const std::string& command) {
        ParsedQuery query;
        std::smatch match;

        if (command == "EXIT" || command == "QUIT") {
            query.type = QueryType::exit;
        } else if (command == "HELP") {
            query.type = QueryType::help;
        } else if (command == "SHOW DATABASES") {
            query.type = QueryType::show_databases;
        } else if (command == "SHOW TABLES") {
            query.type = QueryType::show_tables;
        } else if (std::regex_match(command, match, std::regex(R"(^CREATE\s+DATABASE\s+([a-z_][a-z0-9_]*)$)"))) {
            query.type = QueryType::create_database;
            query.database_name = match[1].str();
        } else if (std::regex_match(command, match, std::regex(R"(^USE\s+([a-z_][a-z0-9_]*)$)"))) {
            query.type = QueryType::use_database;
            query.database_name = match[1].str();
        } else if (std::regex_match(command, match, std::regex(R"(^CREATE\s+TABLE\s+([a-z_][a-z0-9_]*)\s*\(([\s\S]*)\)$)"))) {
            query.type = QueryType::create_table;
            query.table_name = match[1].str();
            for (const std::string& definition : split_csv(match[2].str())) {
                std::optional<Column> column = parse_column_definition(definition);
                if (!column.has_value()) {
                    return std::nullopt;
                }
                query.columns.push_back(*column);
            }
        } else if (std::regex_match(command, match, std::regex(R"(^(DESCRIBE|DESC)\s+([a-z_][a-z0-9_]*)$)"))) {
            query.type = QueryType::describe_table;
            query.table_name = match[2].str();
        } else if (std::regex_match(command, match, std::regex(R"(^INSERT\s+INTO\s+([a-z_][a-z0-9_]*)\s+VALUES\s*\(([\s\S]*)\)$)"))) {
            query.type = QueryType::insert_row;
            query.table_name = match[1].str();
            query.values_text = match[2].str();
        } else if (std::regex_match(command, match, std::regex(R"(^SELECT\s+\*\s+FROM\s+([a-z_][a-z0-9_]*)(?:\s+WHERE\s+([a-z_][a-z0-9_]*)\s*(=|!=|>=|<=|>|<)\s*([\s\S]+))?$)"))) {
            query.type = QueryType::select_all;
            query.table_name = match[1].str();
            if (match[2].matched) {
                query.condition = parse_condition(match[2].str(), match[3].str(), match[4].str(), command);
                if (!query.condition.has_value()) {
                    return std::nullopt;
                }
            }
        } else if (std::regex_match(command, match, std::regex(R"(^DELETE\s+FROM\s+([a-z_][a-z0-9_]*)\s+WHERE\s+([a-z_][a-z0-9_]*)\s*(=|!=|>=|<=|>|<)\s*([\s\S]+)$)"))) {
            query.type = QueryType::delete_row;
            query.table_name = match[1].str();
            query.condition = parse_condition(match[2].str(), match[3].str(), match[4].str(), command);
            if (!query.condition.has_value()) {
                return std::nullopt;
            }
        } else if (std::regex_match(command, match, std::regex(R"(^UPDATE\s+([a-z_][a-z0-9_]*)\s+SET\s+VALUES\s*\(([\s\S]*)\)\s+WHERE\s+([a-z_][a-z0-9_]*)\s*(=|!=|>=|<=|>|<)\s*([\s\S]+)$)"))) {
            query.type = QueryType::update_row;
            query.table_name = match[1].str();
            query.values_text = match[2].str();
            query.condition = parse_condition(match[3].str(), match[4].str(), match[5].str(), command);
            if (!query.condition.has_value()) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }

        return query;
    }

    ParseResult diagnose_parse_error(const std::string& command) {
        if (command.empty()) {
            return parse_error("empty query", "", 0);
        }

        const std::optional<ParseError> delimiter = delimiter_error(command);
        if (delimiter.has_value()) {
            ParseResult result;
            result.error = *delimiter;
            return result;
        }

        std::size_t word_position = 0;
        const std::string word = first_word(command, word_position);
        const std::vector<Token> tokens = tokenize(command);
        const std::optional<ParseResult> lowercase_error = diagnose_lowercase_keyword(tokens);
        if (lowercase_error.has_value()) {
            return *lowercase_error;
        }

        std::smatch match;
        if (std::regex_match(command, match, std::regex(R"(^CREATE\s+TABLE\s+([^\s(]+)\s*\(([\s\S]*)\)$)"))) {
            const std::string table_name = match[1].str();
            if (!Column::is_valid_name(table_name)) {
                return parse_error("table name must be lowercase identifier", table_name, command.find(table_name));
            }

            const std::size_t column_start = command.find('(') + 1;
            for (const TextPart& definition : split_csv_with_positions(match[2].str(), column_start)) {
                if (!parse_column_definition(definition.text).has_value()) {
                    return diagnose_column_definition(definition);
                }
            }
        }

        const std::optional<ParseResult> sequence_error = diagnose_keyword_sequence(command, tokens);
        if (sequence_error.has_value()) {
            return *sequence_error;
        }

        const std::vector<std::string> uppercase_keywords = {
            "EXIT", "QUIT", "HELP", "SHOW", "CREATE", "USE", "DESCRIBE", "DESC", "INSERT", "SELECT", "DELETE", "UPDATE"
        };
        for (const std::string& keyword : uppercase_keywords) {
            if (word == keyword) {
                return parse_error("invalid syntax after", word, word_position);
            }
        }

        if (has_lowercase(word)) {
            return parse_error("unknown command", word, word_position);
        }

        return parse_error("unknown command", word, word_position);
    }
}

std::optional<ParsedQuery> QueryParser::parse(const std::string& command) {
    return parse_internal(command);
}

bool ParseResult::ok() const {
    return query.has_value() && !error.has_value();
}

ParseResult QueryParser::parse_with_error(const std::string& command) {
    const std::optional<ParsedQuery> query = parse_internal(command);
    if (query.has_value()) {
        return parsed_result(*query);
    }
    return diagnose_parse_error(command);
}
