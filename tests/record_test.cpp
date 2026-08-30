#include "record/serializer.hpp"
#include "test_utils.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

Schema student_schema() {
    return Schema("student", {
        Column::integer_column("id", false),
        Column::varstring_column("name", false, 128),
        Column::varstring_column("nickname", true, 64),
    });
}

void serialize_round_trip() {
    const Schema schema = student_schema();
    const Row row({
        Value::integer_value(42),
        Value::varstring_value("alice"),
        Value::varstring_value("ally"),
    });

    std::vector<uint8_t> record;
    require(RecordSerializer::serialize(schema, row, record), "serialize failed");

    Row decoded;
    require(RecordSerializer::deserialize(schema, record, decoded), "deserialize failed");
    require(decoded.value_count() == 3, "bad value count");
    require(decoded.value(0)->integer_data() == 42, "bad integer value");
    require(decoded.value(1)->string_data() == "alice", "bad text value");
    require(decoded.value(2)->string_data() == "ally", "bad nullable text value");
}

void serialize_null_value() {
    require(Value::null_value().type() == ColumnType::null_type, "null value has wrong type");

    const Schema schema = student_schema();
    const Row row({
        Value::integer_value(7),
        Value::varstring_value("bob"),
        Value::null_value(),
    });

    std::vector<uint8_t> record;
    require(RecordSerializer::serialize(schema, row, record), "serialize null failed");
    require((record[0] & static_cast<uint8_t>(1u << 2)) != 0, "null bit not set");

    Row decoded;
    require(RecordSerializer::deserialize(schema, record, decoded), "deserialize null failed");
    require(decoded.value(2)->is_null(), "null value not preserved");
}

void reject_bad_values() {
    const Schema schema = student_schema();
    std::vector<uint8_t> record;

    require(!RecordSerializer::serialize(schema, Row({
        Value::varstring_value("wrong"),
        Value::varstring_value("alice"),
        Value::null_value(),
    }), record), "wrong type accepted");
}

void reject_text_too_large() {
    const Schema schema("tiny", {
        Column::varstring_column("name", false, 3),
    });

    std::vector<uint8_t> record;
    require(!RecordSerializer::serialize(schema, Row({
        Value::varstring_value("toolong"),
    }), record), "too-large text accepted");
}

void reject_malformed_record() {
    const Schema schema = student_schema();
    const std::vector<uint8_t> record = {0};

    Row row;
    require(!RecordSerializer::deserialize(schema, record, row), "malformed record accepted");
}

void serialize_all_types() {
    const Schema schema("typed", {
        Column::integer_column("id", false),
        Column::number_column("price", false, 4, 2),
        Column::char_column("grade", false),
        Column::string_column("code", false, 4),
        Column::varstring_column("name", false, 16),
        Column::date_column("born", false),
        Column::time_column("at_time", false),
        Column::datetime_column("created_at", false),
        Column::text_column("notes", true),
    });

    const Row row({
        Value::integer_value(99),
        Value::number_value(1234),
        Value::char_value('A'),
        Value::string_value("xy"),
        Value::varstring_value("alice"),
        Value::date_value(*Date::from_string("2026-08-29")),
        Value::time_value(*Time::from_string("13:45:20")),
        Value::datetime_value(*DateTime::from_string("2026-08-29 13:45:20")),
        Value::text_value("hello"),
    });

    std::vector<uint8_t> record;
    require(RecordSerializer::serialize(schema, row, record), "serialize all types failed");

    Row decoded;
    require(RecordSerializer::deserialize(schema, record, decoded), "deserialize all types failed");
    require(decoded.value(0)->integer_data() == 99, "integer round trip failed");
    require(decoded.value(1)->number_data() == 1234, "number round trip failed");
    require(decoded.value(2)->string_data() == "A", "char round trip failed");
    require(decoded.value(3)->string_data().substr(0, 2) == "xy", "string round trip failed");
    require(decoded.value(4)->string_data() == "alice", "varstring round trip failed");
    require(decoded.value(5)->date_data().to_string() == "2026-08-29", "date round trip failed");
    require(decoded.value(6)->time_data().to_string() == "13:45:20", "time round trip failed");
    require(decoded.value(7)->datetime_data().to_string() == "2026-08-29 13:45:20", "datetime round trip failed");
    require(decoded.value(8)->string_data() == "hello", "text round trip failed");
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"record round trip", serialize_round_trip});
    tests.push_back({"record null", serialize_null_value});
    tests.push_back({"record bad values", reject_bad_values});
    tests.push_back({"record text size", reject_text_too_large});
    tests.push_back({"record malformed", reject_malformed_record});
    tests.push_back({"record all types", serialize_all_types});

    TestSummary summary;
    print_test_header();
    for (std::size_t i = 0; i < tests.size(); ++i) {
        run_test_row(static_cast<int>(i + 1), tests[i], summary);
    }
    print_test_footer(summary);

    return summary.failed == 0 ? 0 : 1;
}
