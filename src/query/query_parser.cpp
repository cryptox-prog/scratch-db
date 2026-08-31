#include "query/query_parser.hpp"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/constants.hpp"

namespace {
    enum class TokenType {
        word,
        number,
        string,
        left_paren,
        right_paren,
        comma,
        dot,
        star,
        op,
    };

    struct Token {
        TokenType type = TokenType::word;
        std::string text;
        std::size_t position = 0;
    };

    ParseResult parsed_result(ParsedQuery query) {
        ParseResult result;
        result.query = std::move(query);
        return result;
    }

    ParseResult parse_error(const std::string& message, const std::string& token, std::size_t position) {
        ParseResult result;
        result.error = ParseError{message, token, position};
        return result;
    }

    bool is_identifier_start(char ch) {
        return std::islower(static_cast<unsigned char>(ch)) || ch == '_';
    }

    bool is_identifier_body(char ch) {
        return std::islower(static_cast<unsigned char>(ch)) ||
               std::isdigit(static_cast<unsigned char>(ch)) ||
               ch == '_';
    }

    bool is_identifier(const std::string& text) {
        if (text.empty() || !is_identifier_start(text[0])) {
            return false;
        }
        for (char ch : text) {
            if (!is_identifier_body(ch)) {
                return false;
            }
        }
        return true;
    }

    bool is_lowercase_keyword(const std::string& text) {
        const std::vector<std::string> keywords = {
            "exit", "quit", "help", "show", "databases", "tables", "create", "database", "table",
            "use", "describe", "desc", "insert", "into", "values", "select", "from", "delete",
            "where", "and", "or", "group", "by", "having", "order", "asc", "desc", "limit",
            "with", "update", "set", "null", "not", "in", "exists",
            "union", "intersect", "all", "some", "as", "join", "left", "right", "natural", "on",
            "max", "min", "avg", "sum", "count"
        };

        for (const std::string& keyword : keywords) {
            if (text == keyword) {
                return true;
            }
        }
        return false;
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

    bool parse_uint8(const std::string& text, uint8_t& value) {
        uint16_t parsed = 0;
        if (!parse_uint16(text, parsed) || parsed > UINT8_MAX) {
            return false;
        }
        value = static_cast<uint8_t>(parsed);
        return true;
    }

    std::vector<Token> tokenize(const std::string& command, std::optional<ParseError>& error) {
        std::vector<Token> tokens;
        std::vector<std::size_t> parens;
        std::size_t i = 0;

        while (i < command.size()) {
            while (i < command.size() && std::isspace(static_cast<unsigned char>(command[i]))) {
                ++i;
            }
            if (i == command.size()) {
                break;
            }

            const std::size_t start = i;
            const char ch = command[i];

            if (ch == '.') {
                tokens.push_back({TokenType::dot, ".", i});
                ++i;
            } else if (ch == '(') {
                parens.push_back(i);
                tokens.push_back({TokenType::left_paren, "(", i});
                ++i;
            } else if (ch == ')') {
                if (parens.empty()) {
                    error = ParseError{"unexpected closing parenthesis", ")", i};
                    return tokens;
                }
                parens.pop_back();
                tokens.push_back({TokenType::right_paren, ")", i});
                ++i;
            } else if (ch == ',') {
                tokens.push_back({TokenType::comma, ",", i});
                ++i;
            } else if (ch == '*') {
                tokens.push_back({TokenType::star, "*", i});
                ++i;
            } else if (ch == '\'' || ch == '"') {
                const char quote = ch;
                ++i;
                while (i < command.size() && command[i] != quote) {
                    ++i;
                }
                if (i == command.size()) {
                    error = ParseError{"unterminated string literal", std::string(1, quote), start};
                    return tokens;
                }
                ++i;
                tokens.push_back({TokenType::string, command.substr(start, i - start), start});
            } else if (ch == '=' || ch == '!' || ch == '>' || ch == '<') {
                if (i + 1 < command.size() && command[i + 1] == '=') {
                    tokens.push_back({TokenType::op, command.substr(i, 2), i});
                    i += 2;
                } else if (ch == '!') {
                    error = ParseError{"expected !=", "!", i};
                    return tokens;
                } else {
                    tokens.push_back({TokenType::op, command.substr(i, 1), i});
                    ++i;
                }
            } else {
                bool starts_number = std::isdigit(static_cast<unsigned char>(command[i])) ||
                                    (command[i] == '-' && i + 1 < command.size() && std::isdigit(static_cast<unsigned char>(command[i + 1])));
                if (starts_number) {
                    std::size_t end = i + 1;
                    bool seen_dot = false;
                    while (end < command.size()) {
                        const char c = command[end];
                        if (std::isdigit(static_cast<unsigned char>(c))) {
                            ++end;
                            continue;
                        }
                        if (c == '.' && !seen_dot && end + 1 < command.size() && std::isdigit(static_cast<unsigned char>(command[end + 1]))) {
                            seen_dot = true;
                            ++end;
                            continue;
                        }
                        if (c == '-' && end + 1 < command.size() && std::isdigit(static_cast<unsigned char>(command[end + 1]))) {
                            ++end;
                            continue;
                        }
                        break;
                    }
                    tokens.push_back({TokenType::number, command.substr(i, end - i), i});
                    i = end;
                } else {
                    while (i < command.size() &&
                           !std::isspace(static_cast<unsigned char>(command[i])) &&
                           command[i] != '(' &&
                           command[i] != ')' &&
                           command[i] != ',' &&
                           command[i] != '.' &&
                           command[i] != '*' &&
                           command[i] != '=' &&
                           command[i] != '!' &&
                           command[i] != '>' &&
                           command[i] != '<') {
                        ++i;
                    }
                    tokens.push_back({TokenType::word, command.substr(start, i - start), start});
                }
            }
        }

        if (!parens.empty()) {
            error = ParseError{"missing closing parenthesis", "(", parens.back()};
        }
        return tokens;
    }

    class Parser {
    public:
        Parser(const std::string& command, const std::vector<Token>& tokens)
            : command_(command), tokens_(tokens) {}

        ParseResult parse() {
            if (tokens_.empty()) {
                return fail("empty query", "", 0);
            }

            const Token& first = tokens_[0];
            if (lowercase_keyword(first)) {
                return fail("keyword must be uppercase", first.text, first.position);
            }

            if (match("EXIT") || match("QUIT")) {
                ParsedQuery query;
                query.type = QueryType::exit;
                return end_or_error(query);
            }
            if (match("HELP")) {
                ParsedQuery query;
                query.type = QueryType::help;
                return end_or_error(query);
            }
            if (match("BEGIN")) {
                ParsedQuery query;
                query.type = QueryType::begin_transaction;
                return end_or_error(query);
            }
            if (match("COMMIT")) {
                ParsedQuery query;
                query.type = QueryType::commit_transaction;
                return end_or_error(query);
            }
            if (match("ROLLBACK")) {
                ParsedQuery query;
                query.type = QueryType::rollback_transaction;
                return end_or_error(query);
            }
            if (match("SHOW")) {
                return parse_show();
            }
            if (match("CREATE")) {
                return parse_create();
            }
            if (match("ALTER")) {
                return parse_alter();
            }
            if (match("USE")) {
                return parse_use();
            }
            if (match("DESCRIBE") || match("DESC")) {
                return parse_describe();
            }
            if (match("INSERT")) {
                return parse_insert();
            }
            if (match("SELECT")) {
                return parse_select();
            }
            if (match("WITH")) {
                return parse_with();
            }
            if (match("DELETE")) {
                return parse_delete();
            }
            if (match("UPDATE")) {
                return parse_update();
            }

            return fail("unknown command", first.text, first.position);
        }

    private:
        bool at_end() const {
            return index_ >= tokens_.size();
        }

        const Token* peek() const {
            return at_end() ? nullptr : &tokens_[index_];
        }

        const Token* previous() const {
            return index_ == 0 ? nullptr : &tokens_[index_ - 1];
        }

        bool match(const std::string& text) {
            if (!at_end() && tokens_[index_].text == text) {
                ++index_;
                return true;
            }
            return false;
        }

        bool match_type(TokenType type) {
            if (!at_end() && tokens_[index_].type == type) {
                ++index_;
                return true;
            }
            return false;
        }

        ParseResult fail(const std::string& message, const std::string& token, std::size_t position) const {
            return parse_error(message, token, position);
        }

        ParseResult expected(const std::string& text) const {
            if (at_end()) {
                return fail("expected " + text, "", command_.size());
            }
            return fail("expected " + text, tokens_[index_].text, tokens_[index_].position);
        }

        bool lowercase_keyword(const Token& token) const {
            return token.type == TokenType::word && is_lowercase_keyword(token.text);
        }

        ParseResult read_identifier(std::string& identifier, const std::string& label) {
            if (at_end()) {
                return fail("expected " + label, "", command_.size());
            }
            const Token& token = tokens_[index_];
            if (lowercase_keyword(token)) {
                return fail("keyword must be uppercase", token.text, token.position);
            }
            if (!is_identifier(token.text)) {
                return fail(label + " must be lowercase identifier", token.text, token.position);
            }
            identifier = token.text;
            ++index_;
            return ParseResult{};
        }

        ParseResult end_or_error(const ParsedQuery& query) {
            if (!at_end()) {
                const Token& token = tokens_[index_];
                return fail("unexpected token", token.text, token.position);
            }
            return parsed_result(query);
        }

        ParseResult parse_show() {
            ParsedQuery query;
            if (match("DATABASES")) {
                query.type = QueryType::show_databases;
                return end_or_error(query);
            }
            if (match("TABLES")) {
                query.type = QueryType::show_tables;
                return end_or_error(query);
            }
            return expected("DATABASES or TABLES");
        }

        ParseResult parse_create() {
            if (match("DATABASE")) {
                ParsedQuery query;
                query.type = QueryType::create_database;
                ParseResult name = read_identifier(query.database_name, "database name");
                if (!name.ok() && name.error.has_value()) {
                    return name;
                }
                return end_or_error(query);
            }
            if (match("TABLE")) {
                return parse_create_table();
            }
            return expected("DATABASE or TABLE");
        }

        ParseResult parse_use() {
            ParsedQuery query;
            query.type = QueryType::use_database;
            ParseResult name = read_identifier(query.database_name, "database name");
            if (!name.ok() && name.error.has_value()) {
                return name;
            }
            return end_or_error(query);
        }

        ParseResult parse_alter() {
            if (!match("TABLE")) {
                return expected("TABLE");
            }

            ParsedQuery query;
            query.type = QueryType::alter_table;
            ParseResult name = read_identifier(query.table_name, "table name");
            if (!name.ok() && name.error.has_value()) {
                return name;
            }

            if (match("ADD")) {
                std::optional<ConstraintDefinition> constraint;
                ParseResult parsed_constraint = parse_table_constraint(1, constraint);
                if (!parsed_constraint.ok() && parsed_constraint.error.has_value()) {
                    return parsed_constraint;
                }
                query.constraints.push_back(*constraint);
                return end_or_error(query);
            }

            if (match("DROP")) {
                if (!match("CONSTRAINT")) {
                    return expected("CONSTRAINT");
                }
                if (at_end() || tokens_[index_].type != TokenType::number) {
                    return expected("constraint id");
                }
                query.drop_constraint_id = static_cast<uint64_t>(std::stoull(tokens_[index_].text));
                ++index_;
                return end_or_error(query);
            }

            return expected("ADD or DROP");
        }

        ParseResult read_column_list(const std::string& label, std::vector<std::string>& columns) {
            if (!match_type(TokenType::left_paren)) {
                return expected("(");
            }
            while (true) {
                std::string column;
                ParseResult column_result = read_identifier(column, label);
                if (!column_result.ok() && column_result.error.has_value()) {
                    return column_result;
                }
                columns.push_back(column);
                if (!match_type(TokenType::comma)) {
                    break;
                }
            }
            if (!match_type(TokenType::right_paren)) {
                return expected(")");
            }
            return ParseResult{};
        }

        ParseResult parse_table_constraint(uint64_t constraint_id, std::optional<ConstraintDefinition>& constraint) {
            if (match("PRIMARY")) {
                if (!match("KEY")) {
                    return expected("KEY");
                }
                std::vector<std::string> columns;
                ParseResult column_list = read_column_list("primary key column name", columns);
                if (!column_list.ok() && column_list.error.has_value()) {
                    return column_list;
                }
                constraint = ConstraintDefinition::make_primary_key(constraint_id, std::move(columns));
                return ParseResult{};
            }

            if (match("UNIQUE")) {
                std::vector<std::string> columns;
                ParseResult column_list = read_column_list("unique column name", columns);
                if (!column_list.ok() && column_list.error.has_value()) {
                    return column_list;
                }
                constraint = ConstraintDefinition::make_unique(constraint_id, std::move(columns));
                return ParseResult{};
            }

            if (match("FOREIGN")) {
                if (!match("KEY")) {
                    return expected("KEY");
                }
                std::vector<std::string> columns;
                ParseResult column_list = read_column_list("foreign key column name", columns);
                if (!column_list.ok() && column_list.error.has_value()) {
                    return column_list;
                }
                if (columns.size() != 1) {
                    return fail("foreign key requires one column", columns.empty() ? "" : columns[0], previous() == nullptr ? command_.size() : previous()->position);
                }
                if (!match("REFERENCES")) {
                    return expected("REFERENCES");
                }
                std::string ref_table;
                ParseResult ref_table_result = read_identifier(ref_table, "referenced table name");
                if (!ref_table_result.ok() && ref_table_result.error.has_value()) {
                    return ref_table_result;
                }
                std::vector<std::string> ref_columns;
                ParseResult ref_column_list = read_column_list("referenced column name", ref_columns);
                if (!ref_column_list.ok() && ref_column_list.error.has_value()) {
                    return ref_column_list;
                }
                if (ref_columns.size() != 1) {
                    return fail("foreign key reference requires one column", ref_columns.empty() ? "" : ref_columns[0], previous() == nullptr ? command_.size() : previous()->position);
                }
                constraint = ConstraintDefinition::make_foreign_key(constraint_id, {columns[0]}, ref_table, ref_columns[0]);
                return ParseResult{};
            }

            if (match("CHECK")) {
                if (!match_type(TokenType::left_paren)) {
                    return expected("(");
                }
                std::vector<std::string> check_parts;
                while (!at_end()) {
                    const Token* token = peek();
                    if (token == nullptr || token->type == TokenType::right_paren) {
                        break;
                    }
                    if (token->type == TokenType::word ||
                        token->type == TokenType::number ||
                        token->type == TokenType::string ||
                        token->type == TokenType::op ||
                        token->type == TokenType::star) {
                        check_parts.push_back(token->text);
                    }
                    ++index_;
                }
                if (!match_type(TokenType::right_paren)) {
                    return expected(")");
                }
                constraint = ConstraintDefinition::make_check(constraint_id, std::move(check_parts));
                return ParseResult{};
            }

            return expected("constraint");
        }

        ParseResult parse_describe() {
            ParsedQuery query;
            query.type = QueryType::describe_table;
            ParseResult name = read_identifier(query.table_name, "table name");
            if (!name.ok() && name.error.has_value()) {
                return name;
            }
            return end_or_error(query);
        }

        ParseResult parse_create_table() {
            ParsedQuery query;
            query.type = QueryType::create_table;
            ParseResult name = read_identifier(query.table_name, "table name");
            if (!name.ok() && name.error.has_value()) {
                return name;
            }
            if (!match_type(TokenType::left_paren)) {
                return expected("(");
            }
            if (match_type(TokenType::right_paren)) {
                return fail("expected column definition", ")", previous()->position);
            }

            uint64_t constraint_id = 1;
            while (true) {
                if (match_type(TokenType::right_paren)) {
                    break;
                }

                std::optional<Column> column;
                ParseResult parsed_column = parse_column(column);
                if (!parsed_column.ok() && parsed_column.error.has_value()) {
                    return parsed_column;
                }
                query.columns.push_back(*column);

                while (true) {
                    if (match("PRIMARY")) {
                        if (!match("KEY")) {
                            return expected("KEY");
                        }
                        query.constraints.push_back(ConstraintDefinition::make_primary_key(constraint_id++, {column->name()}));
                        continue;
                    }
                    if (match("UNIQUE")) {
                        query.constraints.push_back(ConstraintDefinition::make_unique(constraint_id++, {column->name()}));
                        continue;
                    }
                    if (match("REFERENCES")) {
                        std::string ref_table;
                        ParseResult ref_table_result = read_identifier(ref_table, "referenced table name");
                        if (!ref_table_result.ok() && ref_table_result.error.has_value()) {
                            return ref_table_result;
                        }
                        if (!match_type(TokenType::left_paren)) {
                            return expected("(");
                        }
                        std::string ref_column;
                        ParseResult ref_column_result = read_identifier(ref_column, "referenced column name");
                        if (!ref_column_result.ok() && ref_column_result.error.has_value()) {
                            return ref_column_result;
                        }
                        if (!match_type(TokenType::right_paren)) {
                            return expected(")");
                        }
                        query.constraints.push_back(ConstraintDefinition::make_foreign_key(constraint_id++, {column->name()}, ref_table, ref_column));
                        continue;
                    }
                    if (match("CHECK")) {
                        if (!match_type(TokenType::left_paren)) {
                            return expected("(");
                        }
                        std::vector<std::string> check_parts;
                        while (!at_end()) {
                            const Token* token = peek();
                            if (token == nullptr) {
                                return expected(")");
                            }
                            if (token->type == TokenType::right_paren) {
                                break;
                            }
                            if (token->type == TokenType::word || token->type == TokenType::number || token->type == TokenType::string || token->type == TokenType::op || token->type == TokenType::star) {
                                check_parts.push_back(token->text);
                                ++index_;
                            } else if (token->type == TokenType::left_paren || token->type == TokenType::comma || token->type == TokenType::dot) {
                                if (token->type == TokenType::left_paren) {
                                    check_parts.push_back(token->text);
                                }
                                ++index_;
                            } else {
                                break;
                            }
                        }
                        if (!match_type(TokenType::right_paren)) {
                            return expected(")");
                        }
                        query.constraints.push_back(ConstraintDefinition::make_check(constraint_id++, check_parts));
                        continue;
                    }
                    break;
                }

                if (match_type(TokenType::comma)) {
                    continue;
                }
                if (match_type(TokenType::right_paren)) {
                    break;
                }
                if (peek() != nullptr &&
                    (peek()->text == "PRIMARY" || peek()->text == "UNIQUE" || peek()->text == "FOREIGN" || peek()->text == "CHECK")) {
                    std::optional<ConstraintDefinition> constraint;
                    ParseResult parsed_constraint = parse_table_constraint(constraint_id++, constraint);
                    if (!parsed_constraint.ok() && parsed_constraint.error.has_value()) {
                        return parsed_constraint;
                    }
                    query.constraints.push_back(*constraint);
                    if (match_type(TokenType::comma)) {
                        continue;
                    }
                    if (match_type(TokenType::right_paren)) {
                        break;
                    }
                    return expected(", or )");
                }

                return expected(", or )");
            }

            return end_or_error(query);
        }

        ParseResult parse_column(std::optional<Column>& column) {
            std::string column_name;
            ParseResult name = read_identifier(column_name, "column name");
            if (!name.ok() && name.error.has_value()) {
                return name;
            }
            if (at_end()) {
                return fail("expected column type", "", command_.size());
            }

            const Token type = tokens_[index_++];
            if (lowercase_keyword(type)) {
                return fail("keyword must be uppercase", type.text, type.position);
            }

            bool nullable = true;
            if (type.text == "INTEGER") {
                column = Column::integer_column(column_name, nullable);
            } else if (type.text == "CHAR") {
                column = Column::char_column(column_name, nullable);
            } else if (type.text == "DATE") {
                column = Column::date_column(column_name, nullable);
            } else if (type.text == "TIME") {
                column = Column::time_column(column_name, nullable);
            } else if (type.text == "DATETIME") {
                column = Column::datetime_column(column_name, nullable);
            } else if (type.text == "TEXT") {
                column = Column::text_column(column_name, nullable);
            } else if (type.text == "STRING") {
                uint16_t size = 0;
                ParseResult size_result = parse_one_size("STRING", type, size);
                if (!size_result.ok() && size_result.error.has_value()) {
                    return size_result;
                }
                column = Column::string_column(column_name, nullable, size);
            } else if (type.text == "VARSTRING") {
                uint16_t size = 0;
                ParseResult size_result = parse_one_size("VARSTRING", type, size);
                if (!size_result.ok() && size_result.error.has_value()) {
                    return size_result;
                }
                column = Column::varstring_column(column_name, nullable, size);
            } else if (type.text == "NUMBER") {
                uint8_t precision = 0;
                uint8_t scale = 0;
                ParseResult number_result = parse_number_format(type, precision, scale);
                if (!number_result.ok() && number_result.error.has_value()) {
                    return number_result;
                }
                column = Column::number_column(column_name, nullable, precision, scale);
            } else {
                return fail("unknown column type", type.text, type.position);
            }

            ParseResult nullability = parse_nullability(nullable);
            if (!nullability.ok() && nullability.error.has_value()) {
                return nullability;
            }
            column->set_nullable(nullable);
            return ParseResult{};
        }

        ParseResult parse_one_size(const std::string& type_name, const Token& type_token, uint16_t& size) {
            if (!match_type(TokenType::left_paren)) {
                return fail(type_name + " requires a size, use " + type_name + "(n)", type_token.text, type_token.position);
            }
            if (at_end()) {
                return fail("expected size", "", command_.size());
            }
            const Token size_token = tokens_[index_++];
            if (size_token.type != TokenType::number || !parse_uint16(size_token.text, size)) {
                return fail("invalid " + type_name + " size", size_token.text, size_token.position);
            }
            if (!match_type(TokenType::right_paren)) {
                return expected(")");
            }
            return ParseResult{};
        }

        ParseResult parse_number_format(const Token& type_token, uint8_t& precision, uint8_t& scale) {
            if (!match_type(TokenType::left_paren)) {
                return fail("NUMBER requires precision and scale, use NUMBER(p, s)", type_token.text, type_token.position);
            }
            if (at_end()) {
                return fail("expected precision", "", command_.size());
            }
            const Token precision_token = tokens_[index_++];
            if (precision_token.type != TokenType::number || !parse_uint8(precision_token.text, precision)) {
                return fail("invalid NUMBER precision", precision_token.text, precision_token.position);
            }
            if (!match_type(TokenType::comma)) {
                return expected(",");
            }
            if (at_end()) {
                return fail("expected scale", "", command_.size());
            }
            const Token scale_token = tokens_[index_++];
            if (scale_token.type != TokenType::number || !parse_uint8(scale_token.text, scale)) {
                return fail("invalid NUMBER scale", scale_token.text, scale_token.position);
            }
            if (!match_type(TokenType::right_paren)) {
                return expected(")");
            }
            if (precision > LIMITS::MAX_NUMBER_PRECISION) {
                return fail("NUMBER precision exceeds maximum " + std::to_string(LIMITS::MAX_NUMBER_PRECISION), precision_token.text, precision_token.position);
            }
            if (scale > precision) {
                return fail("NUMBER scale cannot exceed precision", scale_token.text, scale_token.position);
            }
            return ParseResult{};
        }

        ParseResult parse_nullability(bool& nullable) {
            if (match("NULL")) {
                nullable = true;
                return ParseResult{};
            }
            if (match("NOT")) {
                if (!match("NULL")) {
                    return expected("NULL");
                }
                nullable = false;
                return ParseResult{};
            }
            return ParseResult{};
        }

        ParseResult parse_insert() {
            if (!match("INTO")) {
                return expected("INTO");
            }

            ParsedQuery query;
            query.type = QueryType::insert_row;
            ParseResult table = read_identifier(query.table_name, "table name");
            if (!table.ok() && table.error.has_value()) {
                return table;
            }
            if (match_type(TokenType::left_paren)) {
                while (true) {
                    std::string column_name;
                    ParseResult column = read_identifier(column_name, "column name");
                    if (!column.ok() && column.error.has_value()) {
                        return column;
                    }
                    query.insert_columns.push_back(column_name);
                    if (!match_type(TokenType::comma)) {
                        break;
                    }
                }
                if (!match_type(TokenType::right_paren)) {
                    return expected(")");
                }
            }
            if (!match("VALUES")) {
                return expected("VALUES");
            }

            while (true) {
                std::string row_values;
                ParseResult values = parse_parenthesized_body(row_values);
                if (!values.ok() && values.error.has_value()) {
                    return values;
                }
                query.insert_value_rows.push_back(row_values);
                if (!match_type(TokenType::comma)) {
                    break;
                }
            }
            if (!query.insert_value_rows.empty()) {
                query.values_text = query.insert_value_rows.front();
            }
            return end_or_error(query);
        }

        ParseResult parse_select() {
            ParsedQuery query;
            ParseResult select = parse_single_select(query);
            if (!select.ok() && select.error.has_value()) {
                return select;
            }

            ParsedQuery* current = &query;
            while (match("UNION") || match("INTERSECT")) {
                const Token* op_token = previous();
                current->compound_operator = op_token->text == "UNION" ?
                    QueryCompoundOperator::union_op :
                    QueryCompoundOperator::intersect_op;

                if (match("ALL")) {
                    current->compound_quantifier = QuerySetQuantifier::all;
                } else if (match("SOME")) {
                    current->compound_quantifier = QuerySetQuantifier::some;
                } else {
                    current->compound_quantifier = QuerySetQuantifier::some;
                }

                current->compound_query = std::make_unique<ParsedQuery>();
                if (!match("SELECT")) {
                    return expected("SELECT");
                }
                ParseResult next_select = parse_single_select(*current->compound_query);
                if (!next_select.ok() && next_select.error.has_value()) {
                    return next_select;
                }
                current = current->compound_query.get();
            }

            return end_or_error(query);
        }

        ParseResult parse_with() {
            std::vector<QueryCommonTableExpression> ctes;
            while (true) {
                QueryCommonTableExpression cte;
                ParseResult name = read_identifier(cte.name, "common table expression name");
                if (!name.ok() && name.error.has_value()) {
                    return name;
                }
                if (!match("AS")) {
                    return expected("AS");
                }
                QueryExpression subquery;
                ParseResult parsed_subquery = parse_subquery_expression(subquery);
                if (!parsed_subquery.ok() && parsed_subquery.error.has_value()) {
                    return parsed_subquery;
                }
                cte.query = std::move(subquery.subquery);
                ctes.push_back(std::move(cte));

                if (!match_type(TokenType::comma)) {
                    break;
                }
            }

            if (!match("SELECT")) {
                return expected("SELECT");
            }
            ParseResult select = parse_select();
            if (!select.ok() && select.error.has_value()) {
                return select;
            }
            select.query->common_table_expressions = std::move(ctes);
            return select;
        }

        ParseResult parse_table_reference(std::string& table_name, std::string& alias) {
            if (match_type(TokenType::left_paren)) {
                return fail("derived table is only allowed after FROM", "(", previous()->position);
            }
            ParseResult table = read_identifier(table_name, "table name");
            if (!table.ok() && table.error.has_value()) {
                return table;
            }
            alias.clear();
            if (match("AS")) {
                ParseResult alias_result = read_identifier(alias, "table alias");
                if (!alias_result.ok() && alias_result.error.has_value()) {
                    return alias_result;
                }
            }
            return ParseResult{};
        }

        ParseResult parse_from_source(ParsedQuery& query) {
            if (!match_type(TokenType::left_paren)) {
                return parse_table_reference(query.table_name, query.table_alias);
            }

            const Token* open = previous();
            std::size_t depth = 1;
            const std::size_t subquery_start = open->position + 1;
            while (!at_end() && depth > 0) {
                const Token token = tokens_[index_++];
                if (token.type == TokenType::left_paren) {
                    ++depth;
                } else if (token.type == TokenType::right_paren) {
                    --depth;
                    if (depth == 0) {
                        const std::string subquery_text = command_.substr(subquery_start, token.position - subquery_start);
                        ParseResult subquery = QueryParser::parse_with_error(subquery_text);
                        if (!subquery.ok()) {
                            ParseError error = *subquery.error;
                            error.position += subquery_start;
                            return parse_error(error.message, error.token, error.position);
                        }
                        if (subquery.query->type != QueryType::select_all) {
                            return fail("derived table must be SELECT", subquery_text, subquery_start);
                        }
                        query.derived_table = std::make_unique<ParsedQuery>(*subquery.query);
                        if (match("AS")) {
                            return read_identifier(query.table_alias, "table alias");
                        }
                        return read_identifier(query.table_alias, "table alias");
                    }
                }
            }
            return fail("missing closing parenthesis", "(", open->position);
        }

        ParseResult parse_column_reference(std::string& table_alias, std::string& column_name, const std::string& label) {
            std::string first_name;
            ParseResult first = read_identifier(first_name, label);
            if (!first.ok() && first.error.has_value()) {
                return first;
            }

            table_alias.clear();
            column_name = first_name;
            if (match_type(TokenType::dot)) {
                ParseResult second = read_identifier(column_name, label);
                if (!second.ok() && second.error.has_value()) {
                    return second;
                }
                table_alias = std::move(first_name);
            }
            return ParseResult{};
        }

        ParseResult parse_select_list(ParsedQuery& query) {
            if (match_type(TokenType::star)) {
                query.select_all = true;
                return ParseResult{};
            }

            query.select_all = false;
            while (true) {
                SelectedColumn selected;
                selected.position = at_end() ? command_.size() : tokens_[index_].position;
                if (!at_end() && aggregate_from_text(tokens_[index_].text, selected.aggregate)) {
                    ++index_;
                    if (!match_type(TokenType::left_paren)) {
                        return expected("(");
                    }
                    if (selected.aggregate == QueryAggregateFunction::count && match_type(TokenType::star)) {
                        selected.column_name = "*";
                    } else {
                        ParseResult column = parse_column_reference(selected.table_alias, selected.column_name, "selected column");
                        if (!column.ok() && column.error.has_value()) {
                            return column;
                        }
                    }
                    if (!match_type(TokenType::right_paren)) {
                        return expected(")");
                    }
                } else {
                    ParseResult column = parse_column_reference(selected.table_alias, selected.column_name, "selected column");
                    if (!column.ok() && column.error.has_value()) {
                        return column;
                    }
                    selected.position = previous() == nullptr ? selected.position : previous()->position;
                }
                if (match("AS")) {
                    ParseResult alias = read_identifier(selected.alias, "column alias");
                    if (!alias.ok() && alias.error.has_value()) {
                        return alias;
                    }
                }
                query.selected_columns.push_back(selected);

                if (!match_type(TokenType::comma)) {
                    break;
                }
            }
            return ParseResult{};
        }

        bool aggregate_from_text(const std::string& text, QueryAggregateFunction& aggregate) const {
            if (text == "MAX") {
                aggregate = QueryAggregateFunction::max;
                return true;
            }
            if (text == "MIN") {
                aggregate = QueryAggregateFunction::min;
                return true;
            }
            if (text == "AVG") {
                aggregate = QueryAggregateFunction::avg;
                return true;
            }
            if (text == "SUM") {
                aggregate = QueryAggregateFunction::sum;
                return true;
            }
            if (text == "COUNT") {
                aggregate = QueryAggregateFunction::count;
                return true;
            }
            return false;
        }

        ParseResult parse_single_select(ParsedQuery& query) {
            query.type = QueryType::select_all;
            ParseResult select_list = parse_select_list(query);
            if (!select_list.ok() && select_list.error.has_value()) {
                return select_list;
            }
            if (!match("FROM")) {
                return expected("FROM");
            }
            ParseResult table = parse_from_source(query);
            if (!table.ok() && table.error.has_value()) {
                return table;
            }

            auto append_join = [&query](std::unique_ptr<QueryJoin> join) {
                if (query.join == nullptr) {
                    query.join = std::move(join);
                    return;
                }

                QueryJoin* tail = query.join.get();
                while (tail->next_join != nullptr) {
                    tail = tail->next_join.get();
                }
                tail->next_join = std::move(join);
            };

            while (true) {
                if (match_type(TokenType::comma)) {
                    auto join = std::make_unique<QueryJoin>();
                    join->type = QueryJoinType::cross;
                    std::string right_alias;
                    ParseResult right_table = parse_table_reference(join->table_name, right_alias);
                    if (!right_table.ok() && right_table.error.has_value()) {
                        return right_table;
                    }
                    join->table_alias = std::move(right_alias);
                    if (match("ON")) {
                        ParseResult condition = parse_condition_expression(join->condition);
                        if (!condition.ok() && condition.error.has_value()) {
                            return condition;
                        }
                    }
                    append_join(std::move(join));
                    continue;
                }
                if (match("LEFT") || match("RIGHT") || match("JOIN")) {
                    auto join = std::make_unique<QueryJoin>();
                    const std::string previous_text = previous() == nullptr ? "" : previous()->text;
                    join->type = previous_text == "LEFT" ? QueryJoinType::left :
                                       (previous_text == "RIGHT" ? QueryJoinType::right : QueryJoinType::inner);
                    if (previous_text == "LEFT" || previous_text == "RIGHT") {
                        if (!match("JOIN")) {
                            return expected("JOIN");
                        }
                    }
                    std::string right_alias;
                    ParseResult right_table = parse_table_reference(join->table_name, right_alias);
                    if (!right_table.ok() && right_table.error.has_value()) {
                        return right_table;
                    }
                    join->table_alias = std::move(right_alias);
                    if (match("ON")) {
                        ParseResult condition = parse_condition_expression(join->condition);
                        if (!condition.ok() && condition.error.has_value()) {
                            return condition;
                        }
                    }
                    append_join(std::move(join));
                    continue;
                }
                if (match("NATURAL")) {
                    if (!match("JOIN")) {
                        return expected("JOIN");
                    }
                    auto join = std::make_unique<QueryJoin>();
                    join->type = QueryJoinType::natural;
                    std::string right_alias;
                    ParseResult right_table = parse_table_reference(join->table_name, right_alias);
                    if (!right_table.ok() && right_table.error.has_value()) {
                        return right_table;
                    }
                    join->table_alias = std::move(right_alias);
                    append_join(std::move(join));
                    continue;
                }
                break;
            }

            if (match("WHERE")) {
                ParseResult condition = parse_condition_expression(query.condition);
                if (!condition.ok() && condition.error.has_value()) {
                    return condition;
                }
            }
            if (match("GROUP")) {
                if (!match("BY")) {
                    return expected("BY");
                }
                ParseResult group_by = parse_group_by(query);
                if (!group_by.ok() && group_by.error.has_value()) {
                    return group_by;
                }
            }
            if (match("HAVING")) {
                ParseResult having = parse_condition_expression(query.having_condition);
                if (!having.ok() && having.error.has_value()) {
                    return having;
                }
            }
            if (match("ORDER")) {
                if (!match("BY")) {
                    return expected("BY");
                }
                ParseResult order_by = parse_order_by(query);
                if (!order_by.ok() && order_by.error.has_value()) {
                    return order_by;
                }
            }
            if (match("LIMIT")) {
                ParseResult limit = parse_limit(query);
                if (!limit.ok() && limit.error.has_value()) {
                    return limit;
                }
            }
            return ParseResult{};
        }

        ParseResult parse_group_by(ParsedQuery& query) {
            while (true) {
                SelectedColumn selected;
                selected.position = at_end() ? command_.size() : tokens_[index_].position;
                ParseResult column = parse_column_reference(selected.table_alias, selected.column_name, "group column");
                if (!column.ok() && column.error.has_value()) {
                    return column;
                }
                selected.position = previous() == nullptr ? selected.position : previous()->position;
                query.group_by_columns.push_back(selected);
                if (!match_type(TokenType::comma)) {
                    break;
                }
            }
            return ParseResult{};
        }

        ParseResult parse_limit(ParsedQuery& query) {
            if (at_end()) {
                return fail("expected LIMIT count", "", command_.size());
            }
            const Token token = tokens_[index_++];
            if (token.type != TokenType::number) {
                return fail("invalid LIMIT count", token.text, token.position);
            }
            try {
                std::size_t used = 0;
                const unsigned long long parsed = std::stoull(token.text, &used);
                if (used != token.text.size()) {
                    return fail("invalid LIMIT count", token.text, token.position);
                }
                query.limit_count = static_cast<uint64_t>(parsed);
            } catch (...) {
                return fail("invalid LIMIT count", token.text, token.position);
            }
            return ParseResult{};
        }

        ParseResult parse_order_by(ParsedQuery& query) {
            while (true) {
                OrderByColumn order_by;
                order_by.column.position = at_end() ? command_.size() : tokens_[index_].position;
                ParseResult column = parse_column_reference(order_by.column.table_alias, order_by.column.column_name, "order column");
                if (!column.ok() && column.error.has_value()) {
                    return column;
                }
                order_by.column.position = previous() == nullptr ? order_by.column.position : previous()->position;
                if (match("ASC")) {
                    order_by.descending = false;
                } else if (match("DESC")) {
                    order_by.descending = true;
                }
                query.order_by_columns.push_back(order_by);
                if (!match_type(TokenType::comma)) {
                    break;
                }
            }
            return ParseResult{};
        }

        ParseResult parse_delete() {
            if (!match("FROM")) {
                return expected("FROM");
            }

            ParsedQuery query;
            query.type = QueryType::delete_row;
            ParseResult table = read_identifier(query.table_name, "table name");
            if (!table.ok() && table.error.has_value()) {
                return table;
            }
            if (!match("WHERE")) {
                return expected("WHERE");
            }
            ParseResult condition = parse_condition_expression(query.condition);
            if (!condition.ok() && condition.error.has_value()) {
                return condition;
            }
            return end_or_error(query);
        }

        ParseResult parse_update() {
            ParsedQuery query;
            query.type = QueryType::update_row;
            ParseResult table = read_identifier(query.table_name, "table name");
            if (!table.ok() && table.error.has_value()) {
                return table;
            }
            if (!match("SET")) {
                return expected("SET");
            }
            if (match("VALUES")) {
                ParseResult values = parse_parenthesized_body(query.values_text);
                if (!values.ok() && values.error.has_value()) {
                    return values;
                }
            } else {
                while (true) {
                    std::string column_name;
                    ParseResult column = read_identifier(column_name, "column name");
                    if (!column.ok() && column.error.has_value()) {
                        return column;
                    }
                    if (!match_type(TokenType::op) || (previous() == nullptr || previous()->text != "=")) {
                        return expected("=");
                    }

                    std::string value_text;
                    const Token* value_token = peek();
                    if (value_token == nullptr) {
                        return expected("value");
                    }
                    if (value_token->type == TokenType::word && value_token->text == "NULL") {
                        ++index_;
                        value_text = value_token->text;
                    } else if (value_token->type == TokenType::string || value_token->type == TokenType::number || value_token->type == TokenType::word) {
                        ++index_;
                        value_text = value_token->text;
                    } else if (value_token->type == TokenType::left_paren) {
                        ParseResult subvalue = parse_parenthesized_body(value_text);
                        if (!subvalue.ok() && subvalue.error.has_value()) {
                            return subvalue;
                        }
                    } else {
                        return expected("value");
                    }
                    query.update_assignments.emplace_back(column_name, value_text);

                    if (!match_type(TokenType::comma)) {
                        break;
                    }
                }
            }
            if (!match("WHERE")) {
                return expected("WHERE");
            }
            ParseResult condition = parse_condition_expression(query.condition);
            if (!condition.ok() && condition.error.has_value()) {
                return condition;
            }
            return end_or_error(query);
        }

        ParseResult parse_condition_expression(std::unique_ptr<QueryConditionNode>& condition) {
            ParseResult left = parse_or_expression(condition);
            if (!left.ok() && left.error.has_value()) {
                return left;
            }
            return ParseResult{};
        }

        ParseResult parse_or_expression(std::unique_ptr<QueryConditionNode>& node) {
            ParseResult left = parse_and_expression(node);
            if (!left.ok() && left.error.has_value()) {
                return left;
            }

            while (match("OR")) {
                std::unique_ptr<QueryConditionNode> right;
                ParseResult parsed_right = parse_and_expression(right);
                if (!parsed_right.ok() && parsed_right.error.has_value()) {
                    return parsed_right;
                }

                auto parent = std::make_unique<QueryConditionNode>();
                parent->type = QueryConditionNodeType::or_node;
                parent->left = std::move(node);
                parent->right = std::move(right);
                node = std::move(parent);
            }

            return ParseResult{};
        }

        ParseResult parse_and_expression(std::unique_ptr<QueryConditionNode>& node) {
            ParseResult left = parse_comparison(node);
            if (!left.ok() && left.error.has_value()) {
                return left;
            }

            while (match("AND")) {
                std::unique_ptr<QueryConditionNode> right;
                ParseResult parsed_right = parse_comparison(right);
                if (!parsed_right.ok() && parsed_right.error.has_value()) {
                    return parsed_right;
                }

                auto parent = std::make_unique<QueryConditionNode>();
                parent->type = QueryConditionNodeType::and_node;
                parent->left = std::move(node);
                parent->right = std::move(right);
                node = std::move(parent);
            }

            return ParseResult{};
        }

        ParseResult parse_comparison(std::unique_ptr<QueryConditionNode>& node) {
            if (match_type(TokenType::left_paren)) {
                std::unique_ptr<QueryConditionNode> child;
                ParseResult parsed_child = parse_or_expression(child);
                if (!parsed_child.ok() && parsed_child.error.has_value()) {
                    return parsed_child;
                }
                if (!match_type(TokenType::right_paren)) {
                    return expected(")");
                }
                node = std::move(child);
                return ParseResult{};
            }

            if (match("NOT")) {
                if (match("EXISTS")) {
                    QueryExpression subquery;
                    ParseResult parsed_subquery = parse_subquery_expression(subquery);
                    if (!parsed_subquery.ok() && parsed_subquery.error.has_value()) {
                        return parsed_subquery;
                    }
                    auto child = std::make_unique<QueryConditionNode>();
                    child->type = QueryConditionNodeType::exists_subquery;
                    child->condition.right_expression = std::move(subquery);

                    node = std::make_unique<QueryConditionNode>();
                    node->type = QueryConditionNodeType::not_node;
                    node->left = std::move(child);
                    return ParseResult{};
                }
                std::unique_ptr<QueryConditionNode> child;
                ParseResult parsed_child = parse_comparison(child);
                if (!parsed_child.ok() && parsed_child.error.has_value()) {
                    return parsed_child;
                }
                node = std::make_unique<QueryConditionNode>();
                node->type = QueryConditionNodeType::not_node;
                node->left = std::move(child);
                return ParseResult{};
            }

            if (match("EXISTS")) {
                QueryExpression subquery;
                ParseResult parsed_subquery = parse_subquery_expression(subquery);
                if (!parsed_subquery.ok() && parsed_subquery.error.has_value()) {
                    return parsed_subquery;
                }
                node = std::make_unique<QueryConditionNode>();
                node->type = QueryConditionNodeType::exists_subquery;
                node->condition.right_expression = std::move(subquery);
                return ParseResult{};
            }

            QueryCondition result;
            ParseResult left_column = parse_column_expression(result.left_expression, "column name");
            if (!left_column.ok() && left_column.error.has_value()) {
                return left_column;
            }

            if (match("NOT")) {
                if (!match("IN")) {
                    return expected("IN");
                }
                QueryExpression subquery;
                ParseResult parsed_subquery = parse_subquery_expression(subquery);
                if (!parsed_subquery.ok() && parsed_subquery.error.has_value()) {
                    return parsed_subquery;
                }
                result.right_expression = std::move(subquery);
                auto inner = std::make_unique<QueryConditionNode>();
                inner->type = QueryConditionNodeType::in_subquery;
                inner->condition = std::move(result);
                node = std::make_unique<QueryConditionNode>();
                node->type = QueryConditionNodeType::not_node;
                node->left = std::move(inner);
                return ParseResult{};
            }

            if (match("IN")) {
                QueryExpression subquery;
                ParseResult parsed_subquery = parse_subquery_expression(subquery);
                if (!parsed_subquery.ok() && parsed_subquery.error.has_value()) {
                    return parsed_subquery;
                }
                result.right_expression = std::move(subquery);
                node = std::make_unique<QueryConditionNode>();
                node->type = QueryConditionNodeType::in_subquery;
                node->condition = std::move(result);
                return ParseResult{};
            }

            if (at_end() || tokens_[index_].type != TokenType::op) {
                return expected("operator");
            }
            const Token op_token = tokens_[index_++];
            if (op_token.text == "=") {
                result.op = QueryOperator::equal;
            } else if (op_token.text == "!=") {
                result.op = QueryOperator::not_equal;
            } else if (op_token.text == ">") {
                result.op = QueryOperator::greater;
            } else if (op_token.text == "<") {
                result.op = QueryOperator::less;
            } else if (op_token.text == ">=") {
                result.op = QueryOperator::greater_equal;
            } else if (op_token.text == "<=") {
                result.op = QueryOperator::less_equal;
            } else {
                return fail("unknown operator", op_token.text, op_token.position);
            }

            if (match("ANY")) {
                QueryExpression subquery;
                ParseResult parsed_subquery = parse_subquery_expression(subquery);
                if (!parsed_subquery.ok() && parsed_subquery.error.has_value()) {
                    return parsed_subquery;
                }
                result.right_expression = std::move(subquery);
                node = std::make_unique<QueryConditionNode>();
                node->type = QueryConditionNodeType::any_subquery;
                node->condition = std::move(result);
                return ParseResult{};
            }
            if (match("ALL")) {
                QueryExpression subquery;
                ParseResult parsed_subquery = parse_subquery_expression(subquery);
                if (!parsed_subquery.ok() && parsed_subquery.error.has_value()) {
                    return parsed_subquery;
                }
                result.right_expression = std::move(subquery);
                node = std::make_unique<QueryConditionNode>();
                node->type = QueryConditionNodeType::all_subquery;
                node->condition = std::move(result);
                return ParseResult{};
            }

            if (at_end()) {
                return fail("expected value", "", command_.size());
            }

            ParseResult right = parse_value_expression(result.right_expression);
            if (!right.ok() && right.error.has_value()) {
                return right;
            }
            node = std::make_unique<QueryConditionNode>();
            node->type = QueryConditionNodeType::comparison;
            node->condition = std::move(result);
            return ParseResult{};
        }

        ParseResult parse_column_expression(QueryExpression& expression, const std::string& label) {
            if (!at_end() && aggregate_from_text(tokens_[index_].text, expression.aggregate)) {
                expression.type = QueryExpressionType::aggregate;
                expression.position = tokens_[index_].position;
                ++index_;
                if (!match_type(TokenType::left_paren)) {
                    return expected("(");
                }
                if (expression.aggregate == QueryAggregateFunction::count && match_type(TokenType::star)) {
                    expression.column_name = "*";
                } else {
                    ParseResult column = parse_column_reference(expression.table_alias, expression.column_name, label);
                    if (!column.ok() && column.error.has_value()) {
                        return column;
                    }
                }
                if (!match_type(TokenType::right_paren)) {
                    return expected(")");
                }
                return ParseResult{};
            }

            expression.type = QueryExpressionType::column_reference;
            ParseResult column = parse_column_reference(expression.table_alias, expression.column_name, label);
            if (!column.ok() && column.error.has_value()) {
                return column;
            }
            expression.position = previous() == nullptr ? command_.size() : previous()->position;
            return ParseResult{};
        }

        ParseResult parse_value_expression(QueryExpression& expression) {
            const Token value_token = tokens_[index_++];
            expression.type = QueryExpressionType::literal;
            expression.literal_text = value_token.text;
            expression.position = value_token.position;
            if (value_token.type == TokenType::left_paren) {
                --index_;
                return parse_subquery_expression(expression);
            }
            if (value_token.type == TokenType::word && !at_end() && tokens_[index_].type == TokenType::dot) {
                ++index_;
                std::string column_name;
                ParseResult value_reference = read_identifier(column_name, "column name");
                if (!value_reference.ok() && value_reference.error.has_value()) {
                    return value_reference;
                }
                expression.type = QueryExpressionType::column_reference;
                expression.table_alias = value_token.text;
                expression.column_name = column_name;
                expression.literal_text = value_token.text + "." + column_name;
            } else if (value_token.type == TokenType::word && value_token.text != "NULL") {
                expression.type = QueryExpressionType::column_reference;
                expression.column_name = value_token.text;
            }
            return ParseResult{};
        }

        ParseResult parse_subquery_expression(QueryExpression& expression) {
            if (!match_type(TokenType::left_paren)) {
                return expected("(");
            }
            const Token* open = previous();
            std::size_t depth = 1;
            const std::size_t subquery_start = open->position + 1;
            while (!at_end() && depth > 0) {
                const Token token = tokens_[index_++];
                if (token.type == TokenType::left_paren) {
                    ++depth;
                } else if (token.type == TokenType::right_paren) {
                    --depth;
                    if (depth == 0) {
                        const std::string subquery_text = command_.substr(subquery_start, token.position - subquery_start);
                        ParseResult subquery = QueryParser::parse_with_error(subquery_text);
                        if (!subquery.ok()) {
                            ParseError error = *subquery.error;
                            error.position += subquery_start;
                            return parse_error(error.message, error.token, error.position);
                        }
                        if (subquery.query->type != QueryType::select_all) {
                            return fail("subquery must be SELECT", subquery_text, subquery_start);
                        }
                        expression.type = QueryExpressionType::scalar_subquery;
                        expression.literal_text = subquery_text;
                        expression.subquery = std::make_unique<ParsedQuery>(*subquery.query);
                        expression.position = open->position;
                        return ParseResult{};
                    }
                }
            }
            return fail("missing closing parenthesis", "(", open->position);
        }

        ParseResult parse_parenthesized_body(std::string& body) {
            if (!match_type(TokenType::left_paren)) {
                return expected("(");
            }
            const Token* open = previous();
            std::size_t depth = 1;
            const std::size_t body_start = open->position + 1;

            while (!at_end()) {
                const Token token = tokens_[index_++];
                if (token.type == TokenType::left_paren) {
                    ++depth;
                } else if (token.type == TokenType::right_paren) {
                    --depth;
                    if (depth == 0) {
                        body = command_.substr(body_start, token.position - body_start);
                        return ParseResult{};
                    }
                }
            }

            return fail("missing closing parenthesis", "(", open->position);
        }

        const std::string& command_;
        const std::vector<Token>& tokens_;
        std::size_t index_ = 0;
    };
}

QueryExpression::QueryExpression(const QueryExpression& other)
    : table_alias(other.table_alias),
      column_name(other.column_name),
      aggregate(other.aggregate),
      literal_text(other.literal_text),
      position(other.position) {
    type = other.type;
    if (other.subquery != nullptr) {
        subquery = std::make_unique<ParsedQuery>(*other.subquery);
    }
}

QueryExpression& QueryExpression::operator=(const QueryExpression& other) {
    if (this == &other) {
        return *this;
    }
    type = other.type;
    table_alias = other.table_alias;
    column_name = other.column_name;
    aggregate = other.aggregate;
    literal_text = other.literal_text;
    position = other.position;
    subquery = other.subquery == nullptr ? nullptr : std::make_unique<ParsedQuery>(*other.subquery);
    return *this;
}

QueryCommonTableExpression::QueryCommonTableExpression(const QueryCommonTableExpression& other)
    : name(other.name) {
    if (other.query != nullptr) {
        query = std::make_unique<ParsedQuery>(*other.query);
    }
}

QueryCommonTableExpression& QueryCommonTableExpression::operator=(const QueryCommonTableExpression& other) {
    if (this == &other) {
        return *this;
    }
    name = other.name;
    query = other.query == nullptr ? nullptr : std::make_unique<ParsedQuery>(*other.query);
    return *this;
}

QueryJoin::QueryJoin(const QueryJoin& other)
    : type(other.type), table_name(other.table_name), table_alias(other.table_alias) {
    if (other.condition != nullptr) {
        condition = std::make_unique<QueryConditionNode>(*other.condition);
    }
    if (other.next_join != nullptr) {
        next_join = std::make_unique<QueryJoin>(*other.next_join);
    }
}

QueryJoin& QueryJoin::operator=(const QueryJoin& other) {
    if (this == &other) {
        return *this;
    }

    type = other.type;
    table_name = other.table_name;
    table_alias = other.table_alias;
    condition = other.condition == nullptr ? nullptr : std::make_unique<QueryConditionNode>(*other.condition);
    next_join = other.next_join == nullptr ? nullptr : std::make_unique<QueryJoin>(*other.next_join);
    return *this;
}

QueryConditionNode::QueryConditionNode(const QueryConditionNode& other)
    : type(other.type), condition(other.condition) {
    if (other.left != nullptr) {
        left = std::make_unique<QueryConditionNode>(*other.left);
    }
    if (other.right != nullptr) {
        right = std::make_unique<QueryConditionNode>(*other.right);
    }
}

QueryConditionNode& QueryConditionNode::operator=(const QueryConditionNode& other) {
    if (this == &other) {
        return *this;
    }

    type = other.type;
    condition = other.condition;
    left = other.left == nullptr ? nullptr : std::make_unique<QueryConditionNode>(*other.left);
    right = other.right == nullptr ? nullptr : std::make_unique<QueryConditionNode>(*other.right);
    return *this;
}

ParsedQuery::ParsedQuery(const ParsedQuery& other)
    : type(other.type),
      database_name(other.database_name),
      table_name(other.table_name),
      table_alias(other.table_alias),
      columns(other.columns),
      constraints(other.constraints),
      drop_constraint_id(other.drop_constraint_id),
      insert_columns(other.insert_columns),
      values_text(other.values_text),
      insert_value_rows(other.insert_value_rows),
      update_assignments(other.update_assignments),
      select_all(other.select_all),
      selected_columns(other.selected_columns),
      group_by_columns(other.group_by_columns),
      order_by_columns(other.order_by_columns),
      common_table_expressions(other.common_table_expressions),
      compound_operator(other.compound_operator),
      compound_quantifier(other.compound_quantifier),
      limit_count(other.limit_count) {
    if (other.derived_table != nullptr) {
        derived_table = std::make_unique<ParsedQuery>(*other.derived_table);
    }
    if (other.condition != nullptr) {
        condition = std::make_unique<QueryConditionNode>(*other.condition);
    }
    if (other.having_condition != nullptr) {
        having_condition = std::make_unique<QueryConditionNode>(*other.having_condition);
    }
    if (other.join != nullptr) {
        join = std::make_unique<QueryJoin>(*other.join);
    }
    if (other.compound_query != nullptr) {
        compound_query = std::make_unique<ParsedQuery>(*other.compound_query);
    }
}

ParsedQuery& ParsedQuery::operator=(const ParsedQuery& other) {
    if (this == &other) {
        return *this;
    }

    type = other.type;
    database_name = other.database_name;
    table_name = other.table_name;
    table_alias = other.table_alias;
    derived_table = other.derived_table == nullptr ? nullptr : std::make_unique<ParsedQuery>(*other.derived_table);
    columns = other.columns;
    constraints = other.constraints;
    drop_constraint_id = other.drop_constraint_id;
    insert_columns = other.insert_columns;
    values_text = other.values_text;
    insert_value_rows = other.insert_value_rows;
    update_assignments = other.update_assignments;
    select_all = other.select_all;
    selected_columns = other.selected_columns;
    group_by_columns = other.group_by_columns;
    order_by_columns = other.order_by_columns;
    common_table_expressions = other.common_table_expressions;
    condition = other.condition == nullptr ? nullptr : std::make_unique<QueryConditionNode>(*other.condition);
    having_condition = other.having_condition == nullptr ? nullptr : std::make_unique<QueryConditionNode>(*other.having_condition);
    join = other.join == nullptr ? nullptr : std::make_unique<QueryJoin>(*other.join);
    compound_operator = other.compound_operator;
    compound_quantifier = other.compound_quantifier;
    compound_query = other.compound_query == nullptr ? nullptr : std::make_unique<ParsedQuery>(*other.compound_query);
    limit_count = other.limit_count;
    return *this;
}

std::optional<ParsedQuery> QueryParser::parse(const std::string& command) {
    ParseResult result = parse_with_error(command);
    return std::move(result.query);
}

bool ParseResult::ok() const {
    return query.has_value() && !error.has_value();
}

ParseResult QueryParser::parse_with_error(const std::string& command) {
    std::optional<ParseError> token_error;
    const std::vector<Token> tokens = tokenize(command, token_error);
    if (token_error.has_value()) {
        ParseResult result;
        result.error = *token_error;
        return result;
    }

    Parser parser(command, tokens);
    return parser.parse();
}
