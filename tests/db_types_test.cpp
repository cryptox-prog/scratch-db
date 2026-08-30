#include "db_types/date_time.hpp"
#include "test_utils.hpp"

#include <optional>
#include <vector>

namespace {

void date_round_trip() {
    std::optional<Date> date = Date::from_string("2026-08-29");
    require(date.has_value(), "valid date rejected");
    require(date->to_string() == "2026-08-29", "date round trip failed");
}

void date_rejects_invalid_days() {
    require(!Date::from_string("2026-02-29").has_value(), "non leap date accepted");
    require(Date::from_string("2024-02-29").has_value(), "leap date rejected");
    require(!Date::from_string("2026-04-31").has_value(), "bad month day accepted");
    require(!Date::from_string("2026-13-01").has_value(), "bad month accepted");
}

void time_round_trip() {
    std::optional<Time> time = Time::from_string("13:45:20");
    require(time.has_value(), "valid time rejected");
    require(time->to_string() == "13:45:20", "time round trip failed");
}

void time_rejects_invalid_values() {
    require(!Time::from_string("24:00:00").has_value(), "bad hour accepted");
    require(!Time::from_string("23:60:00").has_value(), "bad minute accepted");
    require(!Time::from_string("23:59:60").has_value(), "bad second accepted");
}

void datetime_round_trip() {
    std::optional<DateTime> datetime = DateTime::from_string("2026-08-29 13:45:20");
    require(datetime.has_value(), "valid datetime rejected");
    require(datetime->to_string() == "2026-08-29 13:45:20", "datetime round trip failed");
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"date round trip", date_round_trip});
    tests.push_back({"date invalid", date_rejects_invalid_days});
    tests.push_back({"time round trip", time_round_trip});
    tests.push_back({"time invalid", time_rejects_invalid_values});
    tests.push_back({"datetime round trip", datetime_round_trip});
    return run_tests(tests);
}
