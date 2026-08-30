#include "query/query_executor.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <iomanip>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include "catalog/catalog.hpp"
#include "query/value_parser.hpp"

namespace {
    int compare_values(const Value& left, const Value& right);

    QueryResult message_result(const std::string& message) {
        QueryResult result;
        result.metadata.message = message;
        return result;
    }

    std::size_t token_position(const std::string& command, const std::string& token) {
        const std::size_t position = command.find(token);
        return position == std::string::npos ? 0 : position;
    }

    QueryResult error_result(const std::string& message, const std::string& token, const std::string& source = "", std::size_t position = 0) {
        QueryResult result;
        result.error = QueryError{message, token, source, position};
        return result;
    }

    QueryResult value_error_result(const ValueParseError& error, const std::string& source) {
        return error_result(error.message, error.token, source, error.position);
    }

    std::vector<QueryResultColumn> schema_columns() {
        return {
            {"column", "string"},
            {"type", "string"},
            {"nullable", "string"},
            {"max_size", "integer"},
            {"precision", "integer"},
            {"scale", "integer"},
        };
    }

    std::vector<std::string> schema_row(const Column& column) {
        return {
            column.name(),
            Column::type_to_string(column.type()),
            column.nullable() ? "yes" : "no",
            std::to_string(column.max_size()),
            std::to_string(column.precision()),
            std::to_string(column.scale()),
        };
    }

    std::vector<QueryResultColumn> table_columns(const Schema& schema) {
        std::vector<QueryResultColumn> columns;
        for (const Column& column : schema.columns()) {
            columns.push_back({column.name(), Column::type_to_string(column.type())});
        }
        return columns;
    }

    void add_column_lookup(QueryResult& result, const std::string& key, std::size_t index) {
        const auto existing = result.column_positions.find(key);
        if (existing != result.column_positions.end() && existing->second != index) {
            result.ambiguous_columns.insert(key);
            return;
        }
        result.column_positions[key] = index;
    }

    void set_basic_column_lookup(QueryResult& result) {
        for (std::size_t i = 0; i < result.columns.size(); ++i) {
            add_column_lookup(result, result.columns[i].name, i);
        }
    }

    void set_table_column_lookup(QueryResult& result, const std::string& table_name, const std::string& table_alias) {
        set_basic_column_lookup(result);
        const std::string qualifier = table_alias.empty() ? table_name : table_alias;
        for (std::size_t i = 0; i < result.columns.size(); ++i) {
            add_column_lookup(result, qualifier + "." + result.columns[i].name, i);
        }
    }

    void set_join_column_lookup(QueryResult& result, const std::map<std::string, std::string>& alias_map) {
        set_basic_column_lookup(result);
        std::map<std::string, std::set<std::size_t>> unqualified_sources;
        for (const auto& [key, actual_name] : alias_map) {
            const std::size_t dot = key.find('.');
            if (dot == std::string::npos) {
                continue;
            }
            for (std::size_t i = 0; i < result.columns.size(); ++i) {
                if (result.columns[i].name == actual_name) {
                    add_column_lookup(result, key, i);
                    unqualified_sources[key.substr(dot + 1)].insert(i);
                    break;
                }
            }
        }
        for (const auto& [column_name, indexes] : unqualified_sources) {
            if (indexes.size() > 1) {
                result.ambiguous_columns.insert(column_name);
            } else if (!indexes.empty()) {
                add_column_lookup(result, column_name, *indexes.begin());
            }
        }
    }

    std::vector<std::string> table_row(const Schema& schema, const TableRow& table_row) {
        std::vector<std::string> row;
        for (uint16_t i = 0; i < table_row.row.value_count(); ++i) {
            const Column* column = schema.column(i);
            const Value* value = table_row.row.value(i);
            row.push_back(column != nullptr && value != nullptr ? ValueParser::format_value(*column, *value) : "");
        }
        return row;
    }

    std::string quote_scalar_if_needed(const QueryResultColumn& column, const std::string& value) {
        if (column.type == "integer" || column.type == "number") {
            return value;
        }
        return "'" + value + "'";
    }

    Column column_from_result_column(const QueryResultColumn& column) {
        ColumnType type = ColumnType::varstring;
        Column::type_from_string(column.type, type);
        if (type == ColumnType::integer) {
            return Column::integer_column(column.name, true);
        }
        if (type == ColumnType::number) {
            return Column::number_column(column.name, true, 18, 6);
        }
        if (type == ColumnType::character) {
            return Column::char_column(column.name, true);
        }
        if (type == ColumnType::date) {
            return Column::date_column(column.name, true);
        }
        if (type == ColumnType::time) {
            return Column::time_column(column.name, true);
        }
        if (type == ColumnType::datetime) {
            return Column::datetime_column(column.name, true);
        }
        if (type == ColumnType::text) {
            return Column::text_column(column.name, true);
        }
        return Column::varstring_column(column.name, true, Column::VARSTRING_MAX_SIZE);
    }

    Value value_from_result_text(const QueryResultColumn& column, const std::string& text) {
        Column parsed_column = column_from_result_column(column);
        const std::string value_text = quote_scalar_if_needed(column, text);
        ValueParseResult parsed = ValueParser::parse_value_with_error(parsed_column, value_text);
        return parsed.value.value_or(Value::null_value());
    }

    std::string aggregate_name(QueryAggregateFunction aggregate) {
        if (aggregate == QueryAggregateFunction::max) {
            return "max";
        }
        if (aggregate == QueryAggregateFunction::min) {
            return "min";
        }
        if (aggregate == QueryAggregateFunction::avg) {
            return "avg";
        }
        if (aggregate == QueryAggregateFunction::sum) {
            return "sum";
        }
        if (aggregate == QueryAggregateFunction::count) {
            return "count";
        }
        return "";
    }

    bool selected_has_aggregate(const ParsedQuery& query) {
        for (const SelectedColumn& column : query.selected_columns) {
            if (column.aggregate != QueryAggregateFunction::none) {
                return true;
            }
        }
        return false;
    }

    std::optional<Value> row_value_for_column(const Schema& schema, const std::vector<Value>& row_values, const std::string& column_name) {
        const int index = schema.column_index(column_name);
        if (index < 0 || static_cast<std::size_t>(index) >= row_values.size()) {
            return std::nullopt;
        }
        return row_values[static_cast<std::size_t>(index)];
    }

    bool compare_constraint_operator(const Value& left, const Value& right, QueryOperator op) {
        const int comparison = compare_values(left, right);
        if (op == QueryOperator::equal) {
            return comparison == 0;
        }
        if (op == QueryOperator::not_equal) {
            return comparison != 0;
        }
        if (op == QueryOperator::greater) {
            return comparison > 0;
        }
        if (op == QueryOperator::less) {
            return comparison < 0;
        }
        if (op == QueryOperator::greater_equal) {
            return comparison >= 0;
        }
        if (op == QueryOperator::less_equal) {
            return comparison <= 0;
        }
        return false;
    }

    bool row_satisfies_check_constraint(const Schema& schema, const std::vector<Value>& row_values, const ConstraintDefinition& constraint, QueryResult& error) {
        if (constraint.kind != "check" || constraint.args.size() < 3) {
            error = QueryResult{};
            return true;
        }

        const std::string& column_name = constraint.args[0];
        const std::string& op_text = constraint.args[1];
        const std::string& target_text = constraint.args[2];

        QueryOperator op = QueryOperator::equal;
        if (op_text == ">") {
            op = QueryOperator::greater;
        } else if (op_text == "<") {
            op = QueryOperator::less;
        } else if (op_text == ">=") {
            op = QueryOperator::greater_equal;
        } else if (op_text == "<=") {
            op = QueryOperator::less_equal;
        } else if (op_text == "!=") {
            op = QueryOperator::not_equal;
        } else if (op_text == "=") {
            op = QueryOperator::equal;
        } else {
            error = error_result("unsupported check operator", op_text, "CHECK", 0);
            return false;
        }

        std::optional<Value> left_value = row_value_for_column(schema, row_values, column_name);
        if (!left_value.has_value()) {
            error = error_result("unknown check column", column_name, "CHECK", 0);
            return false;
        }

        Value right_value = Value::null_value();
        const int target_index = schema.column_index(target_text);
        if (target_index >= 0 && static_cast<std::size_t>(target_index) < row_values.size()) {
            right_value = row_values[static_cast<std::size_t>(target_index)];
        } else {
            const Column* literal_column = schema.find_column(column_name);
            if (literal_column == nullptr) {
                error = error_result("unknown check column", column_name, "CHECK", 0);
                return false;
            }
            ValueParseResult parsed = ValueParser::parse_value_with_error(*literal_column, target_text, 0);
            if (!parsed.ok() || !parsed.value.has_value()) {
                error = value_error_result(*parsed.error, "CHECK");
                return false;
            }
            right_value = *parsed.value;
        }

        if (!compare_constraint_operator(*left_value, right_value, op)) {
            error = error_result("constraint " + std::to_string(constraint.id) + " check violation", column_name, "CHECK", 0);
            return false;
        }
        error = QueryResult{};
        return true;
    }

    bool row_satisfies_schema_constraints(const std::string& table_name, const Schema& schema, const std::vector<Value>& row_values, Database* db, std::optional<RecordId> ignore_record_id, QueryResult& error) {
        for (const ConstraintDefinition& constraint : schema.constraints()) {
            if (constraint.kind == "primary_key" || constraint.kind == "unique") {
                bool any_null = false;
                for (const std::string& column_name : constraint.columns) {
                    const std::optional<Value> value = row_value_for_column(schema, row_values, column_name);
                    if (!value.has_value() || value->is_null()) {
                        any_null = true;
                    }
                }
                if (any_null) {
                    error = error_result("constraint " + std::to_string(constraint.id) + " " + constraint.kind + " violation", constraint.columns.empty() ? table_name : constraint.columns[0], "constraint", 0);
                    return false;
                }

                bool found = false;
                const bool completed = db->scan_rows(table_name, [&schema, &constraint, &row_values, &ignore_record_id, &found](const TableRow& table_row) {
                    if (ignore_record_id.has_value() && table_row.record_id.page_id == ignore_record_id->page_id && table_row.record_id.slot_id == ignore_record_id->slot_id) {
                        return true;
                    }
                    std::vector<Value> existing_values = table_row.row.values();
                    bool equal = true;
                    for (const std::string& column_name : constraint.columns) {
                        const int index = schema.column_index(column_name);
                        if (index < 0 || static_cast<std::size_t>(index) >= existing_values.size() || static_cast<std::size_t>(index) >= row_values.size()) {
                            equal = false;
                            break;
                        }
                        if (compare_values(existing_values[static_cast<std::size_t>(index)], row_values[static_cast<std::size_t>(index)]) != 0) {
                            equal = false;
                            break;
                        }
                    }
                    if (equal) {
                        found = true;
                        return false;
                    }
                    return true;
                });
                if (!completed) {
                    error = error_result("constraint " + std::to_string(constraint.id) + " " + constraint.kind + " violation", constraint.columns.empty() ? table_name : constraint.columns[0], "constraint", 0);
                    return false;
                }
                if (found) {
                    error = error_result("constraint " + std::to_string(constraint.id) + " " + constraint.kind + " violation", constraint.columns.empty() ? table_name : constraint.columns[0], "constraint", 0);
                    return false;
                }
            } else if (constraint.kind == "foreign_key") {
                if (constraint.columns.size() != 1 || constraint.args.size() < 2) {
                    continue;
                }
                const std::string& local_column_name = constraint.columns[0];
                const std::string& ref_table_name = constraint.args[0];
                const std::string& ref_column_name = constraint.args[1];
                const std::optional<Value> local_value = row_value_for_column(schema, row_values, local_column_name);
                if (!local_value.has_value() || local_value->is_null()) {
                    continue;
                }
                const Column* local_column = schema.find_column(local_column_name);
                if (local_column == nullptr) {
                    error = error_result("constraint " + std::to_string(constraint.id) + " foreign_key violation", local_column_name, "foreign_key", 0);
                    return false;
                }
                const std::optional<Schema> ref_schema = db->load_schema(ref_table_name);
                if (!ref_schema.has_value()) {
                    error = error_result("constraint " + std::to_string(constraint.id) + " foreign_key violation", local_column_name, "foreign_key", 0);
                    return false;
                }
                const int ref_index = ref_schema->column_index(ref_column_name);
                if (ref_index < 0) {
                    error = error_result("constraint " + std::to_string(constraint.id) + " foreign_key violation", local_column_name, "foreign_key", 0);
                    return false;
                }
                const Column* ref_column = ref_schema->find_column(ref_column_name);
                if (ref_column == nullptr ||
                    local_column->type() != ref_column->type() ||
                    local_column->max_size() != ref_column->max_size() ||
                    local_column->precision() != ref_column->precision() ||
                    local_column->scale() != ref_column->scale()) {
                    error = error_result("constraint " + std::to_string(constraint.id) + " foreign_key violation", local_column_name, "foreign_key", 0);
                    return false;
                }
                bool matched = false;
                db->scan_rows(ref_table_name, [&local_value, &matched, ref_index](const TableRow& table_row) {
                    const Value* value = table_row.row.value(static_cast<uint16_t>(ref_index));
                    if (value != nullptr && compare_values(*value, *local_value) == 0) {
                        matched = true;
                        return false;
                    }
                    return true;
                });
                if (!matched) {
                    error = error_result("constraint " + std::to_string(constraint.id) + " foreign_key violation", local_column_name, "foreign_key", 0);
                    return false;
                }
            } else if (constraint.kind == "check") {
                if (!row_satisfies_check_constraint(schema, row_values, constraint, error)) {
                    return false;
                }
            }
        }

        error = QueryResult{};
        return true;
    }

    bool selected_is_grouped(const ParsedQuery& query, const SelectedColumn& selected) {
        for (const SelectedColumn& group_column : query.group_by_columns) {
            if (group_column.column_name == selected.column_name && group_column.table_alias == selected.table_alias) {
                return true;
            }
        }
        return false;
    }

    std::size_t decimal_scale_for_text(const std::string& text) {
        const std::size_t dot = text.find('.');
        if (dot == std::string::npos) {
            return 0;
        }
        return text.size() - dot - 1;
    }

    std::string format_numeric_aggregate(long double value, const QueryResultColumn& column, const QueryResult& source, std::size_t index) {
        if (column.type == "integer") {
            return std::to_string(static_cast<int64_t>(value));
        }

        std::size_t scale = 0;
        for (const std::vector<std::string>& row : source.rows) {
            if (index < row.size()) {
                scale = std::max(scale, decimal_scale_for_text(row[index]));
            }
        }
        if (scale == 0) {
            scale = 2;
        }

        std::ostringstream out;
        out << std::fixed << std::setprecision(static_cast<int>(scale)) << value;
        return out.str();
    }

    int compare_values(const Value& left, const Value& right) {
        if (left.is_null() || right.is_null()) {
            return left.is_null() == right.is_null() ? 0 : (left.is_null() ? -1 : 1);
        }

        if (left.type() == ColumnType::integer) {
            if (left.integer_data() == right.integer_data()) {
                return 0;
            }
            return left.integer_data() < right.integer_data() ? -1 : 1;
        }
        if (left.type() == ColumnType::number) {
            if (left.number_data() == right.number_data()) {
                return 0;
            }
            return left.number_data() < right.number_data() ? -1 : 1;
        }
        if (left.type() == ColumnType::date) {
            if (left.date_data().days_since_epoch() == right.date_data().days_since_epoch()) {
                return 0;
            }
            return left.date_data().days_since_epoch() < right.date_data().days_since_epoch() ? -1 : 1;
        }
        if (left.type() == ColumnType::time) {
            if (left.time_data().seconds_since_midnight() == right.time_data().seconds_since_midnight()) {
                return 0;
            }
            return left.time_data().seconds_since_midnight() < right.time_data().seconds_since_midnight() ? -1 : 1;
        }
        if (left.type() == ColumnType::datetime) {
            if (left.datetime_data().seconds_since_epoch() == right.datetime_data().seconds_since_epoch()) {
                return 0;
            }
            return left.datetime_data().seconds_since_epoch() < right.datetime_data().seconds_since_epoch() ? -1 : 1;
        }

        if (left.string_data() == right.string_data()) {
            return 0;
        }
        return left.string_data() < right.string_data() ? -1 : 1;
    }

    bool compare_with_operator(int comparison, QueryOperator op) {
        if (op == QueryOperator::equal) {
            return comparison == 0;
        }
        if (op == QueryOperator::not_equal) {
            return comparison != 0;
        }
        if (op == QueryOperator::greater) {
            return comparison > 0;
        }
        if (op == QueryOperator::less) {
            return comparison < 0;
        }
        if (op == QueryOperator::greater_equal) {
            return comparison >= 0;
        }
        if (op == QueryOperator::less_equal) {
            return comparison <= 0;
        }
        return false;
    }

    std::string expression_label(const QueryExpression& expression) {
        if (expression.type == QueryExpressionType::column_reference) {
            return expression.table_alias.empty() ? expression.column_name : expression.table_alias + "." + expression.column_name;
        }
        if (expression.type == QueryExpressionType::aggregate) {
            return aggregate_name(expression.aggregate) + "(" + expression.column_name + ")";
        }
        return expression.literal_text;
    }

    bool row_matches(const Schema& schema, const TableRow& table_row, const QueryCondition& condition, QueryResult& error) {
        const QueryExpression& left = condition.left_expression;
        const QueryExpression& right = condition.right_expression;
        if (left.type != QueryExpressionType::column_reference) {
            error = error_result("expected column", expression_label(left), "", left.position);
            return false;
        }

        const int column_index = schema.column_index(left.column_name);
        if (column_index < 0) {
            error = error_result("unknown column", left.column_name, "", left.position);
            return false;
        }

        const Column* column = schema.column(static_cast<uint16_t>(column_index));
        const Value* stored_value = table_row.row.value(static_cast<uint16_t>(column_index));
        if (column == nullptr || stored_value == nullptr) {
            error = error_result("invalid column", left.column_name, "", left.position);
            return false;
        }

        if (right.type == QueryExpressionType::column_reference) {
            const int right_index = schema.column_index(right.column_name);
            if (right_index < 0) {
                error = error_result("unknown column", right.column_name, "", right.position);
                return false;
            }
            const Value* right_value = table_row.row.value(static_cast<uint16_t>(right_index));
            if (right_value == nullptr) {
                error = error_result("invalid column", right.column_name, "", right.position);
                return false;
            }
            error = QueryResult{};
            return compare_with_operator(compare_values(*stored_value, *right_value), condition.op);
        }

        ValueParseResult condition_value = ValueParser::parse_value_with_error(*column, right.literal_text, right.position);
        if (!condition_value.ok() || !condition_value.value.has_value()) {
            error = value_error_result(*condition_value.error, "");
            return false;
        }

        error = QueryResult{};
        return compare_with_operator(compare_values(*stored_value, *condition_value.value), condition.op);
    }

    bool row_matches_subquery_values(const Schema& schema, const TableRow& table_row, const QueryConditionNode& condition, QueryResult& error) {
        const QueryCondition& query_condition = condition.condition;
        const QueryExpression& left = query_condition.left_expression;
        if (left.type != QueryExpressionType::column_reference) {
            error = error_result("expected column", expression_label(left), "", left.position);
            return false;
        }

        const int column_index = schema.column_index(left.column_name);
        if (column_index < 0) {
            error = error_result("unknown column", left.column_name, "", left.position);
            return false;
        }
        const Column* column = schema.column(static_cast<uint16_t>(column_index));
        const Value* stored_value = table_row.row.value(static_cast<uint16_t>(column_index));
        if (column == nullptr || stored_value == nullptr) {
            error = error_result("invalid column", left.column_name, "", left.position);
            return false;
        }

        bool saw_value = false;
        bool matched = false;
        bool all_match = true;
        for (const std::string& value_text : query_condition.right_values) {
            saw_value = true;
            ValueParseResult parsed_value = ValueParser::parse_value_with_error(*column, quote_scalar_if_needed({column->name(), Column::type_to_string(column->type())}, value_text), left.position);
            if (!parsed_value.ok() || !parsed_value.value.has_value()) {
                error = value_error_result(*parsed_value.error, "");
                return false;
            }
            const int comparison = compare_values(*stored_value, *parsed_value.value);
            const bool comparison_match = condition.type == QueryConditionNodeType::in_subquery ?
                comparison == 0 :
                compare_with_operator(comparison, query_condition.op);
            matched = matched || comparison_match;
            all_match = all_match && comparison_match;
        }

        error = QueryResult{};
        if (condition.type == QueryConditionNodeType::all_subquery) {
            return !saw_value || all_match;
        }
        return matched;
    }

    bool row_matches_condition(const Schema& schema, const TableRow& table_row, const QueryConditionNode* condition, QueryResult& error) {
        if (condition == nullptr) {
            return true;
        }

        if (condition->type == QueryConditionNodeType::comparison) {
            return row_matches(schema, table_row, condition->condition, error);
        }
        if (condition->type == QueryConditionNodeType::in_subquery || condition->type == QueryConditionNodeType::any_subquery || condition->type == QueryConditionNodeType::all_subquery) {
            return row_matches_subquery_values(schema, table_row, *condition, error);
        }
        if (condition->type == QueryConditionNodeType::exists_subquery) {
            error = QueryResult{};
            return condition->condition.exists_result;
        }
        if (condition->type == QueryConditionNodeType::not_node) {
            const bool matched = row_matches_condition(schema, table_row, condition->left.get(), error);
            return !error.ok() ? false : !matched;
        }

        const bool left = row_matches_condition(schema, table_row, condition->left.get(), error);
        if (!error.ok()) {
            return false;
        }

        if (condition->type == QueryConditionNodeType::and_node) {
            if (!left) {
                return false;
            }
            return row_matches_condition(schema, table_row, condition->right.get(), error);
        }

        if (condition->type == QueryConditionNodeType::or_node) {
            if (left) {
                return true;
            }
            return row_matches_condition(schema, table_row, condition->right.get(), error);
        }

        error = error_result("invalid WHERE condition", "", "", 0);
        return false;
    }

    std::string row_key(const std::vector<std::string>& row) {
        std::ostringstream out;
        for (const std::string& value : row) {
            out << value.size() << ":" << value << "|";
        }
        return out.str();
    }

    const Value* row_value_for_column(const Schema& schema, const Row& row, const std::string& column_name) {
        const int index = schema.column_index(column_name);
        if (index < 0) {
            return nullptr;
        }
        return row.value(static_cast<uint16_t>(index));
    }

    bool join_condition_matches(
        const Schema& left_schema,
        const TableRow& left_row,
        const Schema& right_schema,
        const TableRow& right_row,
        const QueryConditionNode* condition,
        QueryResult& error,
        const std::map<std::string, std::string>& left_alias_map = {},
        const std::map<std::string, std::string>& right_alias_map = {}
    ) {
        if (condition == nullptr) {
            return true;
        }

        if (condition->type == QueryConditionNodeType::comparison) {
            const QueryCondition& query_condition = condition->condition;
            const QueryExpression& left_expression = query_condition.left_expression;
            const QueryExpression& right_expression = query_condition.right_expression;

            auto resolve_name = [](const QueryExpression& expression, const std::map<std::string, std::string>& aliases) {
                if (!expression.table_alias.empty()) {
                    const std::string alias_key = expression.table_alias + "." + expression.column_name;
                    const auto it = aliases.find(alias_key);
                    if (it != aliases.end()) {
                        return it->second;
                    }
                }
                return expression.column_name;
            };

            if (left_expression.type != QueryExpressionType::column_reference) {
                error = error_result("expected column", expression_label(left_expression), "", left_expression.position);
                return false;
            }

            const std::string left_column_name = resolve_name(left_expression, left_alias_map);
            const Column* left_column = left_schema.find_column(left_column_name);
            if (left_column == nullptr) {
                const std::string right_column_name = resolve_name(left_expression, right_alias_map);
                const Column* right_column = right_schema.find_column(right_column_name);
                if (right_column == nullptr) {
                    error = error_result("unknown column", left_expression.column_name, "", left_expression.position);
                    return false;
                }
                const Value* stored_value = row_value_for_column(right_schema, right_row.row, right_column_name);
                if (stored_value == nullptr) {
                    error = error_result("invalid column", left_expression.column_name, "", left_expression.position);
                    return false;
                }

                const Column* literal_column = right_column;
                ValueParseResult parsed_value = ValueParser::parse_value_with_error(*literal_column, right_expression.literal_text, right_expression.position);
                if (!parsed_value.ok() || !parsed_value.value.has_value()) {
                    error = value_error_result(*parsed_value.error, "");
                    return false;
                }

                error = QueryResult{};
                return compare_with_operator(compare_values(*stored_value, *parsed_value.value), query_condition.op);
            }

            const Value* stored_value = row_value_for_column(left_schema, left_row.row, left_column_name);
            if (stored_value == nullptr) {
                error = error_result("invalid column", left_expression.column_name, "", left_expression.position);
                return false;
            }

            const Column* literal_column = left_column;
            if (right_expression.type == QueryExpressionType::column_reference) {
                const std::string right_column_name = resolve_name(right_expression, right_alias_map);
                const Column* right_column = right_schema.find_column(right_column_name);
                if (right_column == nullptr) {
                    error = error_result("unknown column", right_expression.column_name, "", right_expression.position);
                    return false;
                }
                const Value* right_value = row_value_for_column(right_schema, right_row.row, right_column_name);
                if (right_value == nullptr) {
                    error = error_result("invalid column", right_expression.column_name, "", right_expression.position);
                    return false;
                }
                error = QueryResult{};
                return compare_with_operator(compare_values(*stored_value, *right_value), query_condition.op);
            }

            ValueParseResult parsed_value = ValueParser::parse_value_with_error(*literal_column, right_expression.literal_text, right_expression.position);
            if (!parsed_value.ok() || !parsed_value.value.has_value()) {
                error = value_error_result(*parsed_value.error, "");
                return false;
            }
            if (!parsed_value.value->matches_column(*literal_column)) {
                error = error_result("invalid WHERE value for column " + literal_column->name(), right_expression.literal_text, "", right_expression.position);
                return false;
            }

            error = QueryResult{};
            return compare_with_operator(compare_values(*stored_value, *parsed_value.value), query_condition.op);
        }

        if (condition->type == QueryConditionNodeType::not_node) {
            const bool matched = join_condition_matches(left_schema, left_row, right_schema, right_row, condition->left.get(), error, left_alias_map, right_alias_map);
            return !error.ok() ? false : !matched;
        }

        if (condition->type == QueryConditionNodeType::in_subquery ||
            condition->type == QueryConditionNodeType::exists_subquery) {
            error = error_result("unsupported JOIN condition", "", "", 0);
            return false;
        }

        const bool left = join_condition_matches(left_schema, left_row, right_schema, right_row, condition->left.get(), error, left_alias_map, right_alias_map);
        if (!error.ok()) {
            return false;
        }

        if (condition->type == QueryConditionNodeType::and_node) {
            if (!left) {
                return false;
            }
            return join_condition_matches(left_schema, left_row, right_schema, right_row, condition->right.get(), error, left_alias_map, right_alias_map);
        }

        if (condition->type == QueryConditionNodeType::or_node) {
            if (left) {
                return true;
            }
            return join_condition_matches(left_schema, left_row, right_schema, right_row, condition->right.get(), error, left_alias_map, right_alias_map);
        }

        error = error_result("invalid WHERE condition", "", "", 0);
        return false;
    }

    bool natural_join_matches(const Schema& left_schema, const TableRow& left_row, const Schema& right_schema, const TableRow& right_row) {
        for (const Column& left_column : left_schema.columns()) {
            const Column* right_column = right_schema.find_column(left_column.name());
            if (right_column == nullptr) {
                continue;
            }
            const Value* left_value = row_value_for_column(left_schema, left_row.row, left_column.name());
            const Value* right_value = row_value_for_column(right_schema, right_row.row, left_column.name());
            if (left_value == nullptr || right_value == nullptr) {
                return false;
            }
            if (compare_values(*left_value, *right_value) != 0) {
                return false;
            }
        }
        return true;
    }

    std::vector<Column> join_columns_for_right_side(const Schema& current_schema, const Schema& right_schema, QueryJoinType join_type) {
        std::vector<Column> appended;
        std::set<std::string> seen_names;
        for (const Column& column : current_schema.columns()) {
            seen_names.insert(column.name());
        }

        for (const Column& column : right_schema.columns()) {
            if (join_type == QueryJoinType::natural && seen_names.find(column.name()) != seen_names.end()) {
                continue;
            }

            std::string column_name = column.name();
            uint32_t suffix = 1;
            while (seen_names.find(column_name) != seen_names.end()) {
                column_name = column.name() + "_" + std::to_string(suffix++);
            }
            seen_names.insert(column_name);
            appended.push_back(Column::from_catalog(column_name, column.type(), column.nullable(), column.max_size(), column.precision(), column.scale()));
        }

        return appended;
    }

    std::map<std::string, std::string> build_alias_map(const std::string& alias, const Schema& schema) {
        std::map<std::string, std::string> result;
        for (const Column& column : schema.columns()) {
            if (!alias.empty()) {
                result[alias + "." + column.name()] = column.name();
            }
            result[column.name()] = column.name();
        }
        return result;
    }

    std::map<std::string, std::string> build_join_alias_map(
        const std::map<std::string, std::string>& current_aliases,
        const std::string& right_alias,
        const Schema& current_schema,
        const Schema& right_schema,
        QueryJoinType join_type
    ) {
        std::map<std::string, std::string> merged = current_aliases;
        std::set<std::string> seen_names;
        for (const Column& column : current_schema.columns()) {
            seen_names.insert(column.name());
        }

        for (const Column& column : right_schema.columns()) {
            if (join_type == QueryJoinType::natural && seen_names.find(column.name()) != seen_names.end()) {
                continue;
            }

            std::string actual_name = column.name();
            uint32_t suffix = 1;
            while (seen_names.find(actual_name) != seen_names.end()) {
                actual_name = column.name() + "_" + std::to_string(suffix++);
            }
            seen_names.insert(actual_name);

            if (!right_alias.empty()) {
                merged[right_alias + "." + column.name()] = actual_name;
            }
            merged[column.name()] = actual_name;
        }

        return merged;
    }

    std::vector<Column> merged_join_schema(const Schema& current_schema, const Schema& right_schema, QueryJoinType join_type) {
        std::vector<Column> merged = current_schema.columns();
        const std::vector<Column> appended = join_columns_for_right_side(current_schema, right_schema, join_type);
        merged.insert(merged.end(), appended.begin(), appended.end());
        return merged;
    }

    bool compatible_columns(const QueryResult& left, const QueryResult& right) {
        if (left.columns.size() != right.columns.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left.columns.size(); ++i) {
            if (left.columns[i].type != right.columns[i].type) {
                return false;
            }
        }
        return true;
    }

    QueryResult union_results(QueryResult left, const QueryResult& right, QuerySetQuantifier quantifier) {
        if (quantifier == QuerySetQuantifier::all) {
            left.rows.insert(left.rows.end(), right.rows.begin(), right.rows.end());
            left.metadata.row_count = left.rows.size();
            return left;
        }

        std::map<std::string, bool> seen;
        std::vector<std::vector<std::string>> rows;
        for (const std::vector<std::string>& row : left.rows) {
            const std::string key = row_key(row);
            if (!seen[key]) {
                seen[key] = true;
                rows.push_back(row);
            }
        }
        for (const std::vector<std::string>& row : right.rows) {
            const std::string key = row_key(row);
            if (!seen[key]) {
                seen[key] = true;
                rows.push_back(row);
            }
        }

        left.rows = rows;
        left.metadata.row_count = left.rows.size();
        return left;
    }

    QueryResult intersect_results(QueryResult left, const QueryResult& right, QuerySetQuantifier quantifier) {
        std::map<std::string, uint64_t> right_counts;
        for (const std::vector<std::string>& row : right.rows) {
            ++right_counts[row_key(row)];
        }

        std::map<std::string, bool> seen;
        std::vector<std::vector<std::string>> rows;
        for (const std::vector<std::string>& row : left.rows) {
            const std::string key = row_key(row);
            if (right_counts[key] == 0) {
                continue;
            }

            if (quantifier == QuerySetQuantifier::all) {
                rows.push_back(row);
                --right_counts[key];
            } else if (!seen[key]) {
                seen[key] = true;
                rows.push_back(row);
            }
        }

        left.rows = rows;
        left.metadata.row_count = left.rows.size();
        return left;
    }

    int result_column_index(const QueryResult& result, const SelectedColumn& selected) {
        const std::string wanted = selected.table_alias.empty() ?
            selected.column_name :
            selected.table_alias + "." + selected.column_name;
        if (result.ambiguous_columns.find(wanted) != result.ambiguous_columns.end()) {
            return -2;
        }
        const auto it = result.column_positions.find(wanted);
        if (it != result.column_positions.end()) {
            return static_cast<int>(it->second);
        }
        return -1;
    }

    int order_by_index(const QueryResult& result, const ParsedQuery& query, const OrderByColumn& order_by) {
        const int direct = result_column_index(result, order_by.column);
        if (direct != -1 || direct == -2) {
            return direct;
        }

        for (const SelectedColumn& selected : query.selected_columns) {
            if (!selected.alias.empty() && selected.alias == order_by.column.column_name) {
                SelectedColumn source_selected = selected;
                source_selected.alias.clear();
                return result_column_index(result, source_selected);
            }
        }
        return -1;
    }

    int expression_column_index(const QueryResult& result, const QueryExpression& expression) {
        SelectedColumn selected;
        selected.table_alias = expression.table_alias;
        selected.column_name = expression.type == QueryExpressionType::aggregate ? aggregate_name(expression.aggregate) : expression.column_name;
        selected.position = expression.position;
        return result_column_index(result, selected);
    }

    QueryOperator flipped_operator(QueryOperator op) {
        if (op == QueryOperator::greater) {
            return QueryOperator::less;
        }
        if (op == QueryOperator::less) {
            return QueryOperator::greater;
        }
        if (op == QueryOperator::greater_equal) {
            return QueryOperator::less_equal;
        }
        if (op == QueryOperator::less_equal) {
            return QueryOperator::greater_equal;
        }
        return op;
    }

    void normalize_column_left(QueryCondition& condition) {
        if (condition.left_expression.type == QueryExpressionType::literal &&
            condition.right_expression.type == QueryExpressionType::column_reference) {
            std::swap(condition.left_expression, condition.right_expression);
            condition.op = flipped_operator(condition.op);
        }
    }

    void replace_outer_references(QueryConditionNode* condition, const QueryResult& outer_result, const std::vector<std::string>& outer_row) {
        if (condition == nullptr) {
            return;
        }
        if (condition->type == QueryConditionNodeType::comparison ||
            condition->type == QueryConditionNodeType::in_subquery ||
            condition->type == QueryConditionNodeType::any_subquery ||
            condition->type == QueryConditionNodeType::all_subquery) {
            QueryExpression* expressions[] = {
                &condition->condition.left_expression,
                &condition->condition.right_expression,
            };
            for (QueryExpression* expression : expressions) {
                if (expression->type != QueryExpressionType::column_reference) {
                    continue;
                }
                const int index = expression_column_index(outer_result, *expression);
                if (index < 0) {
                    continue;
                }
                expression->type = QueryExpressionType::literal;
                expression->literal_text = quote_scalar_if_needed(outer_result.columns[static_cast<std::size_t>(index)], outer_row[static_cast<std::size_t>(index)]);
                expression->table_alias.clear();
                expression->column_name.clear();
            }
            normalize_column_left(condition->condition);
        }
        replace_outer_references(condition->left.get(), outer_result, outer_row);
        replace_outer_references(condition->right.get(), outer_result, outer_row);
    }

    void replace_outer_references(ParsedQuery& query, const QueryResult* outer_result, const std::vector<std::string>* outer_row) {
        if (outer_result == nullptr || outer_row == nullptr) {
            return;
        }
        replace_outer_references(query.condition.get(), *outer_result, *outer_row);
        QueryJoin* join = query.join.get();
        while (join != nullptr) {
            replace_outer_references(join->condition.get(), *outer_result, *outer_row);
            join = join->next_join.get();
        }
        if (query.derived_table != nullptr) {
            replace_outer_references(*query.derived_table, outer_result, outer_row);
        }
        if (query.compound_query != nullptr) {
            replace_outer_references(*query.compound_query, outer_result, outer_row);
        }
    }

    bool result_row_matches(const QueryResult& source, const std::vector<std::string>& row, const QueryConditionNode* condition, QueryResult& error) {
        if (condition == nullptr) {
            return true;
        }
        if (condition->type == QueryConditionNodeType::comparison) {
            const QueryExpression& left = condition->condition.left_expression;
            const QueryExpression& right = condition->condition.right_expression;
            if (left.type != QueryExpressionType::column_reference && left.type != QueryExpressionType::aggregate) {
                error = error_result("expected column", expression_label(left), "", left.position);
                return false;
            }
            const int left_index = expression_column_index(source, left);
            if (left_index == -2) {
                error = error_result("ambiguous column", expression_label(left), "", left.position);
                return false;
            }
            if (left_index < 0) {
                error = error_result("unknown column", expression_label(left), "", left.position);
                return false;
            }

            const QueryResultColumn& left_column = source.columns[static_cast<std::size_t>(left_index)];
            const Value left_value = value_from_result_text(left_column, row[static_cast<std::size_t>(left_index)]);
            if (right.type == QueryExpressionType::column_reference || right.type == QueryExpressionType::aggregate) {
                const int right_index = expression_column_index(source, right);
                if (right_index == -2) {
                    error = error_result("ambiguous column", expression_label(right), "", right.position);
                    return false;
                }
                if (right_index < 0) {
                    error = error_result("unknown column", expression_label(right), "", right.position);
                    return false;
                }
                const Value right_value = value_from_result_text(source.columns[static_cast<std::size_t>(right_index)], row[static_cast<std::size_t>(right_index)]);
                error = QueryResult{};
                return compare_with_operator(compare_values(left_value, right_value), condition->condition.op);
            }

            Column parsed_column = column_from_result_column(left_column);
            ValueParseResult parsed_value = ValueParser::parse_value_with_error(parsed_column, right.literal_text, right.position);
            if (!parsed_value.ok() || !parsed_value.value.has_value()) {
                error = value_error_result(*parsed_value.error, "");
                return false;
            }
            error = QueryResult{};
            return compare_with_operator(compare_values(left_value, *parsed_value.value), condition->condition.op);
        }
        if (condition->type == QueryConditionNodeType::in_subquery || condition->type == QueryConditionNodeType::any_subquery || condition->type == QueryConditionNodeType::all_subquery) {
            const QueryExpression& left = condition->condition.left_expression;
            const int left_index = expression_column_index(source, left);
            if (left_index < 0) {
                error = error_result(left_index == -2 ? "ambiguous column" : "unknown column", expression_label(left), "", left.position);
                return false;
            }
            const QueryResultColumn& left_column = source.columns[static_cast<std::size_t>(left_index)];
            const Value left_value = value_from_result_text(left_column, row[static_cast<std::size_t>(left_index)]);
            Column parsed_column = column_from_result_column(left_column);

            bool saw_value = false;
            bool matched = false;
            bool all_match = true;
            for (const std::string& value_text : condition->condition.right_values) {
                saw_value = true;
                ValueParseResult parsed_value = ValueParser::parse_value_with_error(parsed_column, quote_scalar_if_needed(left_column, value_text), left.position);
                if (!parsed_value.ok() || !parsed_value.value.has_value()) {
                    error = value_error_result(*parsed_value.error, "");
                    return false;
                }
                const int comparison = compare_values(left_value, *parsed_value.value);
                const bool comparison_match = compare_with_operator(comparison, condition->condition.op);
                matched = matched || comparison_match;
                all_match = all_match && comparison_match;
            }

            if (condition->type == QueryConditionNodeType::in_subquery) {
                error = QueryResult{};
                return matched;
            }
            if (!saw_value) {
                error = QueryResult{};
                return condition->type == QueryConditionNodeType::all_subquery;
            }
            error = QueryResult{};
            return condition->type == QueryConditionNodeType::any_subquery ? matched : all_match;
        }
        if (condition->type == QueryConditionNodeType::exists_subquery) {
            error = QueryResult{};
            return condition->condition.exists_result;
        }
        if (condition->type == QueryConditionNodeType::not_node) {
            const bool matched = result_row_matches(source, row, condition->left.get(), error);
            return !error.ok() ? false : !matched;
        }

        const bool left = result_row_matches(source, row, condition->left.get(), error);
        if (!error.ok()) {
            return false;
        }
        if (condition->type == QueryConditionNodeType::and_node) {
            return left && result_row_matches(source, row, condition->right.get(), error);
        }
        if (condition->type == QueryConditionNodeType::or_node) {
            return left || result_row_matches(source, row, condition->right.get(), error);
        }
        error = error_result("invalid WHERE condition", "", "", 0);
        return false;
    }

    QueryResult project_result(const QueryResult& source, const ParsedQuery& query) {
        if (query.select_all) {
            return source;
        }

        QueryResult result;
        for (const SelectedColumn& selected : query.selected_columns) {
            const int index = result_column_index(source, selected);
            if (index == -2) {
                return error_result("ambiguous selected column", selected.column_name, "", selected.position);
            }
            if (index < 0) {
                return error_result("unknown selected column", selected.column_name, "", selected.position);
            }
            const QueryResultColumn& source_column = source.columns[static_cast<std::size_t>(index)];
            result.columns.push_back({selected.alias.empty() ? source_column.name : selected.alias, source_column.type});
        }

        for (const std::vector<std::string>& row : source.rows) {
            std::vector<std::string> projected;
            for (const SelectedColumn& selected : query.selected_columns) {
                const int index = result_column_index(source, selected);
                if (index == -2) {
                    return error_result("ambiguous selected column", selected.column_name, "", selected.position);
                }
                projected.push_back(row[static_cast<std::size_t>(index)]);
            }
            result.rows.push_back(std::move(projected));
        }
        result.metadata.row_count = result.rows.size();
        set_basic_column_lookup(result);
        return result;
    }

    QueryResult aggregate_result(const QueryResult& source, const ParsedQuery& query) {
        QueryResult result;
        std::vector<std::string> row;

        for (const SelectedColumn& selected : query.selected_columns) {
            if (selected.aggregate == QueryAggregateFunction::none) {
                return error_result("cannot mix aggregate and non-aggregate selected columns", selected.column_name, "", selected.position);
            }

            if (selected.aggregate == QueryAggregateFunction::count && selected.column_name == "*") {
                result.columns.push_back({selected.alias.empty() ? "count" : selected.alias, "integer"});
                row.push_back(std::to_string(source.rows.size()));
                continue;
            }

            const int index = result_column_index(source, selected);
            if (index == -2) {
                return error_result("ambiguous selected column", selected.column_name, "", selected.position);
            }
            if (index < 0) {
                return error_result("unknown selected column", selected.column_name, "", selected.position);
            }

            const QueryResultColumn& column = source.columns[static_cast<std::size_t>(index)];
            result.columns.push_back({selected.alias.empty() ? aggregate_name(selected.aggregate) : selected.alias, column.type});
            if (source.rows.empty()) {
                row.push_back("");
                continue;
            }

            std::string value = source.rows[0][static_cast<std::size_t>(index)];
            if (selected.aggregate == QueryAggregateFunction::max || selected.aggregate == QueryAggregateFunction::min) {
                for (const std::vector<std::string>& source_row : source.rows) {
                    const std::string& candidate = source_row[static_cast<std::size_t>(index)];
                    bool better = false;
                    if (column.type == "integer" || column.type == "number") {
                        const long double candidate_number = std::stold(candidate);
                        const long double current_number = std::stold(value);
                        better = selected.aggregate == QueryAggregateFunction::max ?
                            candidate_number > current_number :
                            candidate_number < current_number;
                    } else {
                        better = selected.aggregate == QueryAggregateFunction::max ?
                            candidate > value :
                            candidate < value;
                    }
                    if (better) {
                        value = candidate;
                    }
                }
                row.push_back(value);
                continue;
            }

            if (column.type != "integer" && column.type != "number") {
                return error_result("aggregate requires numeric column", selected.column_name, "", selected.position);
            }

            long double sum = 0;
            for (const std::vector<std::string>& source_row : source.rows) {
                sum += std::stold(source_row[static_cast<std::size_t>(index)]);
            }
            if (selected.aggregate == QueryAggregateFunction::sum) {
                row.push_back(format_numeric_aggregate(sum, column, source, static_cast<std::size_t>(index)));
            } else if (selected.aggregate == QueryAggregateFunction::avg) {
                row.push_back(format_numeric_aggregate(sum / static_cast<long double>(source.rows.size()), column, source, static_cast<std::size_t>(index)));
            }
        }

        result.rows.push_back(std::move(row));
        result.metadata.row_count = result.rows.size();
        set_basic_column_lookup(result);
        return result;
    }

    QueryResult grouped_result(const QueryResult& source, const ParsedQuery& query) {
        std::vector<int> group_indexes;
        for (const SelectedColumn& group_column : query.group_by_columns) {
            const int index = result_column_index(source, group_column);
            if (index == -2) {
                return error_result("ambiguous GROUP BY column", group_column.column_name, "", group_column.position);
            }
            if (index < 0) {
                return error_result("unknown GROUP BY column", group_column.column_name, "", group_column.position);
            }
            group_indexes.push_back(index);
        }

        for (const SelectedColumn& selected : query.selected_columns) {
            if (selected.aggregate == QueryAggregateFunction::none && !selected_is_grouped(query, selected)) {
                return error_result("selected column must appear in GROUP BY", selected.column_name, "", selected.position);
            }
        }

        QueryResult result;
        for (const SelectedColumn& selected : query.selected_columns) {
            if (selected.aggregate == QueryAggregateFunction::none) {
                const int index = result_column_index(source, selected);
                if (index == -2) {
                    return error_result("ambiguous selected column", selected.column_name, "", selected.position);
                }
                if (index < 0) {
                    return error_result("unknown selected column", selected.column_name, "", selected.position);
                }
                result.columns.push_back({selected.alias.empty() ? source.columns[static_cast<std::size_t>(index)].name : selected.alias, source.columns[static_cast<std::size_t>(index)].type});
            } else if (selected.aggregate == QueryAggregateFunction::count && selected.column_name == "*") {
                result.columns.push_back({selected.alias.empty() ? "count" : selected.alias, "integer"});
            } else {
                const int index = result_column_index(source, selected);
                if (index == -2) {
                    return error_result("ambiguous selected column", selected.column_name, "", selected.position);
                }
                if (index < 0) {
                    return error_result("unknown selected column", selected.column_name, "", selected.position);
                }
                result.columns.push_back({selected.alias.empty() ? aggregate_name(selected.aggregate) : selected.alias, source.columns[static_cast<std::size_t>(index)].type});
            }
        }

        std::map<std::string, std::vector<std::vector<std::string>>> groups;
        std::vector<std::string> group_order;
        for (const std::vector<std::string>& row : source.rows) {
            std::vector<std::string> key_values;
            for (int index : group_indexes) {
                key_values.push_back(row[static_cast<std::size_t>(index)]);
            }
            const std::string key = row_key(key_values);
            if (groups.find(key) == groups.end()) {
                group_order.push_back(key);
            }
            groups[key].push_back(row);
        }

        for (const std::string& key : group_order) {
            QueryResult group_source;
            group_source.columns = source.columns;
            group_source.rows = groups[key];
            group_source.metadata.row_count = group_source.rows.size();
            group_source.column_positions = source.column_positions;
            group_source.ambiguous_columns = source.ambiguous_columns;

            std::vector<std::string> output_row;
            for (const SelectedColumn& selected : query.selected_columns) {
                if (selected.aggregate == QueryAggregateFunction::none) {
                    const int index = result_column_index(source, selected);
                    if (index == -2) {
                        return error_result("ambiguous selected column", selected.column_name, "", selected.position);
                    }
                    output_row.push_back(group_source.rows[0][static_cast<std::size_t>(index)]);
                    continue;
                }

                ParsedQuery single_aggregate_query;
                single_aggregate_query.select_all = false;
                single_aggregate_query.selected_columns.push_back(selected);
                QueryResult aggregate = aggregate_result(group_source, single_aggregate_query);
                if (!aggregate.ok()) {
                    return aggregate;
                }
                output_row.push_back(aggregate.rows[0][0]);
            }
            result.rows.push_back(std::move(output_row));
        }

        result.metadata.row_count = result.rows.size();
        set_basic_column_lookup(result);
        return result;
    }

    QueryResult order_result(QueryResult result, const ParsedQuery& query) {
        if (query.order_by_columns.empty()) {
            return result;
        }

        std::vector<std::pair<int, bool>> order_indexes;
        for (const OrderByColumn& order_by : query.order_by_columns) {
            const int index = order_by_index(result, query, order_by);
            if (index == -2) {
                return error_result("ambiguous ORDER BY column", order_by.column.column_name, "", order_by.column.position);
            }
            if (index < 0) {
                return error_result("unknown ORDER BY column", order_by.column.column_name, "", order_by.column.position);
            }
            order_indexes.push_back({index, order_by.descending});
        }

        std::stable_sort(result.rows.begin(), result.rows.end(), [&result, &order_indexes](const std::vector<std::string>& left, const std::vector<std::string>& right) {
            for (const auto& [index, descending] : order_indexes) {
                const std::size_t column_index = static_cast<std::size_t>(index);
                const QueryResultColumn& column = result.columns[column_index];
                int comparison = 0;
                if (column.type == "integer" || column.type == "number") {
                    const long double left_number = std::stold(left[column_index]);
                    const long double right_number = std::stold(right[column_index]);
                    comparison = left_number == right_number ? 0 : (left_number < right_number ? -1 : 1);
                } else {
                    comparison = left[column_index] == right[column_index] ? 0 : (left[column_index] < right[column_index] ? -1 : 1);
                }
                if (comparison == 0) {
                    continue;
                }
                return descending ? comparison > 0 : comparison < 0;
            }
            return false;
        });

        return result;
    }

    QueryResult order_source_rows(QueryResult result, const ParsedQuery& query) {
        if (query.order_by_columns.empty()) {
            return result;
        }

        std::vector<std::pair<int, bool>> order_indexes;
        for (const OrderByColumn& order_by : query.order_by_columns) {
            const int index = order_by_index(result, query, order_by);
            if (index == -2) {
                return error_result("ambiguous ORDER BY column", order_by.column.column_name, "", order_by.column.position);
            }
            if (index < 0) {
                return error_result("unknown ORDER BY column", order_by.column.column_name, "", order_by.column.position);
            }
            order_indexes.push_back({index, order_by.descending});
        }

        std::stable_sort(result.rows.begin(), result.rows.end(), [&result, &order_indexes](const std::vector<std::string>& left, const std::vector<std::string>& right) {
            for (const auto& [index, descending] : order_indexes) {
                const std::size_t column_index = static_cast<std::size_t>(index);
                const QueryResultColumn& column = result.columns[column_index];
                int comparison = 0;
                if (column.type == "integer" || column.type == "number") {
                    const long double left_number = std::stold(left[column_index]);
                    const long double right_number = std::stold(right[column_index]);
                    comparison = left_number == right_number ? 0 : (left_number < right_number ? -1 : 1);
                } else {
                    comparison = left[column_index] == right[column_index] ? 0 : (left[column_index] < right[column_index] ? -1 : 1);
                }
                if (comparison == 0) {
                    continue;
                }
                return descending ? comparison > 0 : comparison < 0;
            }
            return false;
        });

        return result;
    }

    QueryResult having_result(QueryResult result, const ParsedQuery& query) {
        if (query.having_condition == nullptr) {
            return result;
        }

        QueryResult filtered;
        filtered.columns = result.columns;
        filtered.column_positions = result.column_positions;
        filtered.ambiguous_columns = result.ambiguous_columns;
        for (const std::vector<std::string>& row : result.rows) {
            QueryResult condition_error;
            if (result_row_matches(result, row, query.having_condition.get(), condition_error)) {
                filtered.rows.push_back(row);
            }
            if (!condition_error.ok()) {
                condition_error.error->source = "";
                return condition_error;
            }
        }
        filtered.metadata.row_count = filtered.rows.size();
        return filtered;
    }

    QueryResult limit_result(QueryResult result, const ParsedQuery& query) {
        if (!query.limit_count.has_value()) {
            return result;
        }
        if (result.rows.size() > *query.limit_count) {
            result.rows.resize(static_cast<std::size_t>(*query.limit_count));
        }
        result.metadata.row_count = result.rows.size();
        return result;
    }
}

bool QueryResult::ok() const {
    return !error.has_value();
}

QueryExecutor::QueryExecutor(std::filesystem::path data_root)
    : data_root_(std::move(data_root)) {}

const std::string& QueryExecutor::current_database() const {
    return current_database_;
}

QueryResult QueryExecutor::execute(const std::string& command) {
    current_command_ = command;
    const ParseResult parsed = QueryParser::parse_with_error(command);
    if (!parsed.ok()) {
        const ParseError& error = *parsed.error;
        return error_result(error.message, error.token, command, error.position);
    }
    return execute_parsed(*parsed.query);
}

Database* QueryExecutor::database() {
    return database_.get();
}

void QueryExecutor::select_database(const std::string& database_name) {
    current_database_ = database_name;
    database_ = std::make_unique<Database>(data_root_, current_database_);
}

QueryResult QueryExecutor::execute_parsed(const ParsedQuery& query) {
    switch (query.type) {
        case QueryType::exit: {
            QueryResult result;
            result.should_exit = true;
            return result;
        }
        case QueryType::help:
            return help();
        case QueryType::show_databases:
            return show_databases();
        case QueryType::show_tables:
            return show_tables();
        case QueryType::create_database:
            return create_database(query.database_name);
        case QueryType::use_database:
            return use_database(query.database_name);
        case QueryType::create_table:
            return create_table(query.table_name, query.columns, query.constraints);
        case QueryType::describe_table:
            return describe_table(query.table_name);
        case QueryType::insert_row:
            if (!query.insert_value_rows.empty()) {
                return insert_row(query.table_name, query.insert_columns, query.insert_value_rows);
            }
            return insert_row(query.table_name, query.insert_columns, query.values_text);
        case QueryType::select_all:
            return execute_select_chain(query);
        case QueryType::delete_row:
            return delete_row(query.table_name, query.condition.get());
        case QueryType::update_row:
            if (!query.update_assignments.empty()) {
                return update_row(query.table_name, query.update_assignments, query.condition.get());
            }
            return update_row(query.table_name, query.values_text, query.condition.get());
        case QueryType::unknown:
            return error_result("unknown command", "", current_command_);
    }

    return error_result("unknown command", "", current_command_);
}

QueryResult QueryExecutor::help() const {
    QueryResult result;
    result.columns = {{"command", "string"}};
    result.rows = {
        {"CREATE DATABASE name;"},
        {"USE name;"},
        {"SHOW DATABASES;"},
        {"SHOW TABLES;"},
        {"CREATE TABLE name (id INTEGER NOT NULL, name VARSTRING(128) NULL);"},
        {"DESCRIBE table;"},
        {"INSERT INTO table VALUES (1, 'alice', NULL);"},
        {"SELECT * FROM table;"},
        {"DELETE FROM table WHERE id = 1;"},
        {"UPDATE table SET name = 'alice', active = true WHERE id = 1;"},
        {"EXIT;"},
    };
    result.metadata.row_count = result.rows.size();
    return result;
}

QueryResult QueryExecutor::create_database(const std::string& database_name) {
    select_database(database_name);
    if (database_->database_exists()) {
        return error_result("database already exists", database_name, current_command_, token_position(current_command_, database_name));
    }
    if (!database_->create_database()) {
        return error_result("could not create database", database_name, current_command_, token_position(current_command_, database_name));
    }
    return message_result("database created");
}

QueryResult QueryExecutor::use_database(const std::string& database_name) {
    Database candidate(data_root_, database_name);
    if (!candidate.database_exists()) {
        return error_result("unknown database", database_name, current_command_, token_position(current_command_, database_name));
    }

    select_database(database_name);
    return message_result("database changed");
}

QueryResult QueryExecutor::show_databases() const {
    QueryResult result;
    result.columns = {{"database", "string"}};

    Catalog catalog(data_root_);
    for (const std::string& name : catalog.list_databases()) {
        result.rows.push_back({name});
    }
    result.metadata.row_count = result.rows.size();
    set_basic_column_lookup(result);
    return result;
}

QueryResult QueryExecutor::show_tables() {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", "", current_command_);
    }

    QueryResult result;
    result.columns = {{"table", "string"}};
    for (const std::string& name : db->list_tables()) {
        result.rows.push_back({name});
    }
    result.metadata.row_count = result.rows.size();
    set_basic_column_lookup(result);
    return result;
}

QueryResult QueryExecutor::create_table(const std::string& table_name, const std::vector<Column>& columns, const std::vector<ConstraintDefinition>& constraints) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    try {
        if (db->table_exists(table_name)) {
            return error_result("table already exists", table_name, current_command_, token_position(current_command_, table_name));
        }
        if (!db->create_table(Schema(table_name, columns, constraints))) {
            return error_result("could not create table", table_name, current_command_, token_position(current_command_, table_name));
        }
    } catch (const std::exception& error) {
        return error_result(error.what(), table_name, current_command_, token_position(current_command_, table_name));
    }

    return message_result("table created");
}

QueryResult QueryExecutor::describe_table(const std::string& table_name) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result;
    result.columns = schema_columns();
    for (const Column& column : schema->columns()) {
        result.rows.push_back(schema_row(column));
    }
    result.metadata.row_count = result.rows.size();
    return result;
}

namespace {
    std::vector<std::string> split_value_list(const std::string& text) {
        std::vector<std::string> values;
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
                std::string trimmed = current;
                while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
                    trimmed.erase(trimmed.begin());
                }
                while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
                    trimmed.pop_back();
                }
                values.push_back(trimmed);
                current.clear();
            } else {
                current.push_back(ch);
            }
        }

        if (!current.empty() || !text.empty()) {
            std::string trimmed = current;
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
                trimmed.erase(trimmed.begin());
            }
            while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
                trimmed.pop_back();
            }
            values.push_back(trimmed);
        }

        return values;
    }

    std::string join_value_list(const std::vector<std::string>& values) {
        std::string result;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                result += ", ";
            }
            result += values[i];
        }
        return result;
    }
}

QueryResult QueryExecutor::insert_row(const std::string& table_name, const std::string& values_text) {
    return insert_row(table_name, {}, values_text);
}

QueryResult QueryExecutor::insert_row(const std::string& table_name, const std::vector<std::string>& insert_columns, const std::string& values_text) {
    return insert_row(table_name, insert_columns, std::vector<std::string>{values_text});
}

QueryResult QueryExecutor::insert_row(const std::string& table_name, const std::vector<std::string>& insert_columns, const std::vector<std::string>& values_rows) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    if (values_rows.empty()) {
        return error_result("empty VALUES list", table_name, current_command_, token_position(current_command_, table_name));
    }

    uint64_t inserted = 0;
    for (const std::string& values_text : values_rows) {
        std::vector<std::string> values = split_value_list(values_text);
        std::string ordered_values = values_text;
        if (!insert_columns.empty()) {
            if (values.size() != insert_columns.size()) {
                return error_result(
                    "wrong value count: expected " + std::to_string(insert_columns.size()) + ", got " + std::to_string(values.size()),
                    values_text,
                    current_command_,
                    token_position(current_command_, values_text)
                );
            }

            std::vector<std::string> ordered(schema->column_count(), "");
            std::set<std::string> seen;
            for (std::size_t i = 0; i < insert_columns.size(); ++i) {
                int column_index = schema->column_index(insert_columns[i]);
                if (column_index < 0) {
                    return error_result("unknown column", insert_columns[i], current_command_, token_position(current_command_, insert_columns[i]));
                }
                if (!seen.insert(insert_columns[i]).second) {
                    return error_result("duplicate column", insert_columns[i], current_command_, token_position(current_command_, insert_columns[i]));
                }
                ordered[static_cast<std::size_t>(column_index)] = values[i];
            }

            for (uint16_t i = 0; i < schema->column_count(); ++i) {
                if (ordered[i].empty()) {
                    const Column* column = schema->column(i);
                    if (column == nullptr) {
                        return error_result("invalid schema column", "", current_command_);
                    }
                    if (column->nullable()) {
                        ordered[i] = "NULL";
                    } else {
                        return error_result("missing value for required column", column->name(), current_command_, token_position(current_command_, column->name()));
                    }
                }
            }

            ordered_values = join_value_list(ordered);
        }

        const std::size_t values_position = token_position(current_command_, values_text);
        ValueParseResult row = ValueParser::parse_row_with_error(*schema, ordered_values, values_position);
        if (!row.ok() || !row.row.has_value()) {
            return value_error_result(*row.error, current_command_);
        }

        QueryResult constraint_error;
        if (!row_satisfies_schema_constraints(table_name, *schema, row.row->values(), db, std::nullopt, constraint_error)) {
            if (constraint_error.ok()) {
                return error_result("constraint violation", table_name, current_command_, token_position(current_command_, table_name));
            }
            constraint_error.error->source = current_command_;
            return constraint_error;
        }

        std::optional<RecordId> record_id = db->insert_row(table_name, *row.row);
        if (!record_id.has_value()) {
            return error_result("insert failed", table_name, current_command_, token_position(current_command_, table_name));
        }
        ++inserted;
    }

    QueryResult result = message_result("row inserted");
    result.metadata.row_count = inserted;
    return result;
}

QueryResult QueryExecutor::execute_join(const ParsedQuery& query) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", query.table_name, current_command_, token_position(current_command_, query.table_name));
    }

    std::optional<Schema> left_schema = db->load_schema(query.table_name);
    if (!left_schema.has_value()) {
        return error_result("unknown table", query.table_name, current_command_, token_position(current_command_, query.table_name));
    }
    if (query.join == nullptr) {
        return select_all(query.table_name, query.table_alias, query.condition.get());
    }

    std::vector<TableRow> current_rows = db->scan_rows(query.table_name);
    Schema current_schema = *left_schema;
    std::map<std::string, std::string> current_alias_map = build_alias_map(query.table_alias.empty() ? query.table_name : query.table_alias, current_schema);
    const QueryJoin* current_join = query.join.get();

    while (current_join != nullptr) {
        std::optional<Schema> right_schema = db->load_schema(current_join->table_name);
        if (!right_schema.has_value()) {
            return error_result("unknown table", current_join->table_name, current_command_, token_position(current_command_, current_join->table_name));
        }

        const std::string right_alias = current_join->table_alias.empty() ? current_join->table_name : current_join->table_alias;
        const std::map<std::string, std::string> right_alias_map = build_alias_map(right_alias, *right_schema);
        const std::vector<Column> right_side_columns = join_columns_for_right_side(current_schema, *right_schema, current_join->type);
        std::vector<TableRow> next_rows;
        std::vector<TableRow> right_rows = db->scan_rows(current_join->table_name);

        for (const TableRow& left_row : current_rows) {
            bool matched = false;
            for (const TableRow& right_row : right_rows) {
                bool include = true;
                if (current_join->type == QueryJoinType::cross) {
                    include = true;
                } else if (current_join->type == QueryJoinType::natural) {
                    include = natural_join_matches(current_schema, left_row, *right_schema, right_row);
                } else if (current_join->type == QueryJoinType::inner || current_join->type == QueryJoinType::left || current_join->type == QueryJoinType::right) {
                    QueryResult condition_error;
                    include = current_join->condition == nullptr || join_condition_matches(current_schema, left_row, *right_schema, right_row, current_join->condition.get(), condition_error, current_alias_map, right_alias_map);
                    if (!include && !condition_error.ok()) {
                        condition_error.error->source = current_command_;
                        return condition_error;
                    }
                }

                if (!include) {
                    continue;
                }

                std::vector<Value> combined_values = left_row.row.values();
                for (uint16_t i = 0; i < right_row.row.value_count(); ++i) {
                    const Column* column = right_schema->column(i);
                    if (column == nullptr) {
                        continue;
                    }
                    if (current_join->type == QueryJoinType::natural && current_schema.find_column(column->name()) != nullptr) {
                        continue;
                    }
                    combined_values.push_back(*right_row.row.value(i));
                }
                next_rows.push_back(TableRow{left_row.record_id, Row(combined_values)});
                matched = true;
            }

            if (current_join->type == QueryJoinType::left && !matched) {
                std::vector<Value> combined_values = left_row.row.values();
                for (const Column& column : right_side_columns) {
                    (void)column;
                    combined_values.push_back(Value::null_value());
                }
                next_rows.push_back(TableRow{left_row.record_id, Row(combined_values)});
            }
        }

        const std::vector<Column> merged_columns = merged_join_schema(current_schema, *right_schema, current_join->type);
        current_alias_map = build_join_alias_map(current_alias_map, right_alias, current_schema, *right_schema, current_join->type);
        current_rows = std::move(next_rows);
        current_schema = Schema(current_schema.table_name(), merged_columns);
        current_join = current_join->next_join.get();
    }

    QueryResult result;
    result.columns = table_columns(current_schema);
    for (const TableRow& row : current_rows) {
        if (query.condition != nullptr) {
            QueryResult condition_error;
            if (!join_condition_matches(current_schema, row, current_schema, row, query.condition.get(), condition_error, current_alias_map, current_alias_map)) {
                if (!condition_error.ok()) {
                    condition_error.error->source = current_command_;
                    return condition_error;
                }
                continue;
            }
        }
        result.rows.push_back(table_row(current_schema, row));
    }
    result.metadata.row_count = result.rows.size();
    set_join_column_lookup(result, current_alias_map);
    return result;
}

QueryResult QueryExecutor::scalar_subquery_value(const ParsedQuery& query, std::string& value_text) {
    return scalar_subquery_value(query, nullptr, nullptr, value_text);
}

QueryResult QueryExecutor::scalar_subquery_value(const ParsedQuery& query, const QueryResult* outer_result, const std::vector<std::string>* outer_row, std::string& value_text) {
    QueryResult result = execute_select_chain(query, outer_result, outer_row);
    if (!result.ok()) {
        return result;
    }
    if (result.columns.size() != 1) {
        return error_result("scalar subquery must return one column", "SELECT", current_command_, token_position(current_command_, "SELECT"));
    }
    if (result.rows.size() != 1) {
        return error_result("scalar subquery must return one row", "SELECT", current_command_, token_position(current_command_, "SELECT"));
    }
    value_text = quote_scalar_if_needed(result.columns[0], result.rows[0][0]);
    return QueryResult{};
}

QueryResult QueryExecutor::resolve_subqueries(QueryConditionNode* condition) {
    return resolve_subqueries(condition, nullptr, nullptr);
}

QueryResult QueryExecutor::resolve_subqueries(QueryConditionNode* condition, const QueryResult* outer_result, const std::vector<std::string>* outer_row) {
    if (condition == nullptr) {
        return QueryResult{};
    }
    if (condition->type == QueryConditionNodeType::comparison) {
        if (condition->condition.right_expression.subquery == nullptr) {
            return QueryResult{};
        }
        std::string value_text;
        QueryResult result = scalar_subquery_value(*condition->condition.right_expression.subquery, outer_result, outer_row, value_text);
        if (!result.ok()) {
            return result;
        }
        condition->condition.right_expression.type = QueryExpressionType::literal;
        condition->condition.right_expression.literal_text = std::move(value_text);
        condition->condition.right_expression.column_name.clear();
        condition->condition.right_expression.table_alias.clear();
        condition->condition.right_expression.subquery.reset();
        return QueryResult{};
    }
    if (condition->type == QueryConditionNodeType::in_subquery ||
        condition->type == QueryConditionNodeType::any_subquery ||
        condition->type == QueryConditionNodeType::all_subquery) {
        if (condition->condition.right_expression.subquery == nullptr) {
            return QueryResult{};
        }
        QueryResult result = execute_select_chain(*condition->condition.right_expression.subquery, outer_result, outer_row);
        if (!result.ok()) {
            return result;
        }
        if (result.columns.size() != 1) {
            return error_result("subquery must return one column", "SELECT", current_command_, token_position(current_command_, "SELECT"));
        }
        condition->condition.right_values.clear();
        for (const std::vector<std::string>& row : result.rows) {
            if (!row.empty()) {
                condition->condition.right_values.push_back(row[0]);
            }
        }
        condition->condition.right_expression.subquery.reset();
        return QueryResult{};
    }
    if (condition->type == QueryConditionNodeType::exists_subquery) {
        if (condition->condition.right_expression.subquery == nullptr) {
            return QueryResult{};
        }
        QueryResult result = execute_select_chain(*condition->condition.right_expression.subquery, outer_result, outer_row);
        if (!result.ok()) {
            return result;
        }
        condition->condition.exists_result = !result.rows.empty();
        condition->condition.right_expression.subquery.reset();
        return QueryResult{};
    }
    if (condition->type == QueryConditionNodeType::not_node) {
        return resolve_subqueries(condition->left.get(), outer_result, outer_row);
    }

    QueryResult left = resolve_subqueries(condition->left.get(), outer_result, outer_row);
    if (!left.ok()) {
        return left;
    }
    return resolve_subqueries(condition->right.get(), outer_result, outer_row);
}

QueryResult QueryExecutor::execute_select(const ParsedQuery& query) {
    ParsedQuery resolved_query = query;
    ParsedQuery source_query = resolved_query;
    source_query.condition.reset();

    QueryResult rows = execute_row_source(source_query);
    if (!rows.ok()) {
        return rows;
    }
    if (resolved_query.condition != nullptr) {
        QueryResult filtered;
        filtered.columns = rows.columns;
        filtered.column_positions = rows.column_positions;
        filtered.ambiguous_columns = rows.ambiguous_columns;

        for (const std::vector<std::string>& row : rows.rows) {
            std::unique_ptr<QueryConditionNode> row_condition = std::make_unique<QueryConditionNode>(*resolved_query.condition);
            QueryResult resolved = resolve_subqueries(row_condition.get(), &rows, &row);
            if (!resolved.ok()) {
                return resolved;
            }
            QueryResult condition_error;
            if (result_row_matches(rows, row, row_condition.get(), condition_error)) {
                filtered.rows.push_back(row);
            }
            if (!condition_error.ok()) {
                condition_error.error->source = current_command_;
                return condition_error;
            }
        }
        filtered.metadata.row_count = filtered.rows.size();
        rows = std::move(filtered);
    }
    if (!resolved_query.group_by_columns.empty()) {
        if (resolved_query.select_all) {
            return error_result("SELECT * is not allowed with GROUP BY", "*", current_command_, token_position(current_command_, "*"));
        }
        return limit_result(order_result(having_result(grouped_result(rows, resolved_query), resolved_query), resolved_query), resolved_query);
    }
    if (selected_has_aggregate(resolved_query)) {
        return limit_result(order_result(having_result(aggregate_result(rows, resolved_query), resolved_query), resolved_query), resolved_query);
    }
    if (resolved_query.having_condition != nullptr) {
        return error_result("HAVING requires aggregate query", "HAVING", current_command_, token_position(current_command_, "HAVING"));
    }
    QueryResult ordered_rows = order_source_rows(rows, resolved_query);
    if (!ordered_rows.ok()) {
        return ordered_rows;
    }
    return limit_result(project_result(ordered_rows, resolved_query), resolved_query);
}

QueryResult QueryExecutor::execute_row_source(const ParsedQuery& query) {
    if (query.derived_table != nullptr) {
        ParsedQuery derived_query = *query.derived_table;
        derived_query.common_table_expressions = query.common_table_expressions;
        QueryResult result = execute_select_chain(derived_query);
        if (!result.ok()) {
            return result;
        }
        if (query.table_alias.empty()) {
            return error_result("derived table requires alias", "", current_command_);
        }
        result.column_positions.clear();
        result.ambiguous_columns.clear();
        set_table_column_lookup(result, query.table_alias, query.table_alias);
        return result;
    }

    for (const QueryCommonTableExpression& cte : query.common_table_expressions) {
        if (cte.name != query.table_name) {
            continue;
        }
        if (cte.query == nullptr) {
            return error_result("invalid common table expression", cte.name, current_command_, token_position(current_command_, cte.name));
        }
        ParsedQuery cte_query = *cte.query;
        cte_query.common_table_expressions = query.common_table_expressions;
        QueryResult result = execute_select_chain(cte_query);
        if (!result.ok()) {
            return result;
        }
        result.column_positions.clear();
        result.ambiguous_columns.clear();
        set_table_column_lookup(result, query.table_name, query.table_alias);
        return result;
    }

    return query.join == nullptr ?
        select_all(query.table_name, query.table_alias, query.condition.get()) :
        execute_join(query);
}

QueryResult QueryExecutor::execute_select_chain(const ParsedQuery& query) {
    return execute_select_chain(query, nullptr, nullptr);
}

QueryResult QueryExecutor::execute_select_chain(const ParsedQuery& query, const QueryResult* outer_result, const std::vector<std::string>* outer_row) {
    ParsedQuery prepared_query = query;
    replace_outer_references(prepared_query, outer_result, outer_row);
    QueryResult result = execute_select(prepared_query);
    if (!result.ok()) {
        return result;
    }

    const ParsedQuery* current = &prepared_query;
    while (current->compound_query != nullptr && current->compound_operator.has_value()) {
        QueryResult right = execute_select(*current->compound_query);
        if (!right.ok()) {
            return right;
        }
        if (!compatible_columns(result, right)) {
            return error_result("compound SELECT columns do not match", current->compound_query->table_name, current_command_, token_position(current_command_, current->compound_query->table_name));
        }

        if (*current->compound_operator == QueryCompoundOperator::union_op) {
            result = union_results(std::move(result), right, current->compound_quantifier);
        } else {
            result = intersect_results(std::move(result), right, current->compound_quantifier);
        }

        current = current->compound_query.get();
    }

    return result;
}

QueryResult QueryExecutor::select_all(const std::string& table_name, const std::string& table_alias, const QueryConditionNode* condition) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result;
    result.columns = table_columns(*schema);
    QueryResult condition_error;
    const bool completed = db->scan_rows(table_name, [&schema, condition, &condition_error, &result](const TableRow& row) {
        const bool matches = row_matches_condition(*schema, row, condition, condition_error);
        if (!condition_error.ok()) {
            return false;
        }
        if (matches) {
            result.rows.push_back(table_row(*schema, row));
        }
        return true;
    });

    if (!completed) {
        if (!condition_error.ok()) {
            condition_error.error->source = current_command_;
            return condition_error;
        }
        return error_result("scan failed", table_name, current_command_, token_position(current_command_, table_name));
    }

    result.metadata.row_count = result.rows.size();
    set_table_column_lookup(result, table_name, table_alias);
    return result;
}

QueryResult QueryExecutor::delete_row(const std::string& table_name, const QueryConditionNode* condition) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult condition_error;
    uint64_t deleted = 0;
    const bool completed = db->scan_rows(table_name, [db, &schema, &table_name, condition, &condition_error, &deleted](const TableRow& row) {
        const bool matches = row_matches_condition(*schema, row, condition, condition_error);
        if (!condition_error.ok()) {
            return false;
        }
        if (matches && db->delete_row(table_name, row.record_id)) {
            ++deleted;
        }
        return true;
    });

    if (!completed) {
        if (!condition_error.ok()) {
            condition_error.error->source = current_command_;
            return condition_error;
        }
        return error_result("scan failed", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result = message_result("row deleted");
    result.metadata.row_count = deleted;
    return result;
}

QueryResult QueryExecutor::update_row(const std::string& table_name, const std::string& values_text, const QueryConditionNode* condition) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    const std::size_t values_position = token_position(current_command_, values_text);
    ValueParseResult row = ValueParser::parse_row_with_error(*schema, values_text, values_position);
    if (!row.ok() || !row.row.has_value()) {
        return value_error_result(*row.error, current_command_);
    }

    QueryResult condition_error;
    uint64_t updated = 0;
    const bool completed = db->scan_rows(table_name, [db, &schema, &table_name, condition, &condition_error, &row, &updated](const TableRow& table_row) {
        const bool matches = row_matches_condition(*schema, table_row, condition, condition_error);
        if (!condition_error.ok()) {
            return false;
        }

        RecordId record_id = table_row.record_id;
        if (matches) {
            if (!row_satisfies_schema_constraints(table_name, *schema, row.row->values(), db, record_id, condition_error)) {
                return false;
            }
            if (db->update_row(table_name, record_id, *row.row)) {
                ++updated;
            }
        }
        return true;
    });

    if (!completed) {
        if (!condition_error.ok()) {
            condition_error.error->source = current_command_;
            return condition_error;
        }
        return error_result("scan failed", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result = message_result("row updated");
    result.metadata.row_count = updated;
    return result;
}

QueryResult QueryExecutor::update_row(const std::string& table_name, const std::vector<std::pair<std::string, std::string>>& assignments, const QueryConditionNode* condition) {
    Database* db = database();
    if (db == nullptr) {
        return error_result("no database selected", table_name, current_command_, token_position(current_command_, table_name));
    }

    std::optional<Schema> schema = db->load_schema(table_name);
    if (!schema.has_value()) {
        return error_result("unknown table", table_name, current_command_, token_position(current_command_, table_name));
    }

    if (assignments.empty()) {
        return error_result("no assignments specified", "SET", current_command_, token_position(current_command_, "SET"));
    }

    QueryResult condition_error;
    uint64_t updated = 0;
    const bool completed = db->scan_rows(table_name, [this, db, &schema, &table_name, condition, &condition_error, &assignments, &updated](const TableRow& table_row) {
        const bool matches = row_matches_condition(*schema, table_row, condition, condition_error);
        if (!condition_error.ok()) {
            return false;
        }
        if (!matches) {
            return true;
        }

        std::vector<Value> values = table_row.row.values();
        for (const auto& [column_name, value_text] : assignments) {
            const int index = schema->column_index(column_name);
            if (index < 0) {
                condition_error = error_result("unknown column", column_name, current_command_, token_position(current_command_, column_name));
                return false;
            }
            const Column& column = schema->columns()[static_cast<std::size_t>(index)];
            ValueParseResult parsed = ValueParser::parse_value_with_error(column, value_text, token_position(current_command_, value_text));
            if (!parsed.ok() || !parsed.value.has_value()) {
                condition_error = value_error_result(*parsed.error, current_command_);
                return false;
            }
            values[static_cast<std::size_t>(index)] = *parsed.value;
        }

        RecordId record_id = table_row.record_id;
        if (!row_satisfies_schema_constraints(table_name, *schema, values, db, record_id, condition_error)) {
            return false;
        }
        if (db->update_row(table_name, record_id, Row(values))) {
            ++updated;
        }
        return true;
    });

    if (!completed) {
        if (!condition_error.ok()) {
            condition_error.error->source = current_command_;
            return condition_error;
        }
        return error_result("scan failed", table_name, current_command_, token_position(current_command_, table_name));
    }

    QueryResult result = message_result("row updated");
    result.metadata.row_count = updated;
    return result;
}
