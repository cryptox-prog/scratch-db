#include "db_types/date_time.hpp"

#include <iomanip>
#include <sstream>

namespace {
    constexpr int64_t SECONDS_PER_DAY = 86400;

    bool parse_number(const std::string& text, std::size_t pos, std::size_t count, int& value) {
        if (pos + count > text.size()) {
            return false;
        }

        int parsed = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char ch = text[pos + i];
            if (ch < '0' || ch > '9') {
                return false;
            }
            parsed = parsed * 10 + (ch - '0');
        }

        value = parsed;
        return true;
    }

    bool is_leap_year(int year) {
        return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    }

    int days_in_month(int year, int month) {
        static constexpr int DAYS[] = {
            31, 28, 31, 30, 31, 30,
            31, 31, 30, 31, 30, 31,
        };

        if (month == 2 && is_leap_year(year)) {
            return 29;
        }
        return DAYS[month - 1];
    }

    bool is_valid_date(int year, int month, int day) {
        return month >= 1 &&
               month <= 12 &&
               day >= 1 &&
               day <= days_in_month(year, month);
    }

    int64_t days_from_civil(int year, unsigned month, unsigned day) {
        year -= month <= 2;
        const int era = (year >= 0 ? year : year - 399) / 400;
        const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
        const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
        return era * 146097 + static_cast<int>(day_of_era) - 719468;
    }

    void civil_from_days(int64_t days, int& year, unsigned& month, unsigned& day) {
        days += 719468;
        const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
        const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
        const unsigned year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
        year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
        const unsigned day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
        const unsigned month_prime = (5 * day_of_year + 2) / 153;
        day = day_of_year - (153 * month_prime + 2) / 5 + 1;
        month = month_prime + (month_prime < 10 ? 3 : -9);
        year += month <= 2;
    }

    std::string two_digits(unsigned value) {
        std::ostringstream out;
        out << std::setw(2) << std::setfill('0') << value;
        return out.str();
    }
}

Date::Date(int32_t days_since_epoch) : days_since_epoch_(days_since_epoch) {}

int32_t Date::days_since_epoch() const {
    return days_since_epoch_;
}

std::string Date::to_string() const {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days_since_epoch_, year, month, day);

    std::ostringstream out;
    out << std::setw(4) << std::setfill('0') << year
        << "-" << two_digits(month)
        << "-" << two_digits(day);
    return out.str();
}

std::optional<Date> Date::from_string(const std::string& text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    if (!parse_number(text, 0, 4, year) ||
        !parse_number(text, 5, 2, month) ||
        !parse_number(text, 8, 2, day) ||
        !is_valid_date(year, month, day)) {
        return std::nullopt;
    }

    return Date(static_cast<int32_t>(days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day))));
}

Time::Time(uint32_t seconds_since_midnight) : seconds_since_midnight_(seconds_since_midnight) {}

uint32_t Time::seconds_since_midnight() const {
    return seconds_since_midnight_;
}

std::string Time::to_string() const {
    const uint32_t hour = seconds_since_midnight_ / 3600;
    const uint32_t minute = (seconds_since_midnight_ % 3600) / 60;
    const uint32_t second = seconds_since_midnight_ % 60;
    return two_digits(hour) + ":" + two_digits(minute) + ":" + two_digits(second);
}

std::optional<Time> Time::from_string(const std::string& text) {
    if (text.size() != 8 || text[2] != ':' || text[5] != ':') {
        return std::nullopt;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_number(text, 0, 2, hour) ||
        !parse_number(text, 3, 2, minute) ||
        !parse_number(text, 6, 2, second) ||
        hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return std::nullopt;
    }

    return Time(static_cast<uint32_t>(hour * 3600 + minute * 60 + second));
}

DateTime::DateTime(int64_t seconds_since_epoch) : seconds_since_epoch_(seconds_since_epoch) {}

int64_t DateTime::seconds_since_epoch() const {
    return seconds_since_epoch_;
}

std::string DateTime::to_string() const {
    const int64_t days = seconds_since_epoch_ / SECONDS_PER_DAY;
    int64_t seconds = seconds_since_epoch_ % SECONDS_PER_DAY;
    if (seconds < 0) {
        seconds += SECONDS_PER_DAY;
    }

    return Date(static_cast<int32_t>(days)).to_string() + " " + Time(static_cast<uint32_t>(seconds)).to_string();
}

std::optional<DateTime> DateTime::from_string(const std::string& text) {
    if (text.size() != 19 || text[10] != ' ') {
        return std::nullopt;
    }

    std::optional<Date> date = Date::from_string(text.substr(0, 10));
    std::optional<Time> time = Time::from_string(text.substr(11, 8));
    if (!date.has_value() || !time.has_value()) {
        return std::nullopt;
    }

    const int64_t seconds =
        static_cast<int64_t>(date->days_since_epoch()) * SECONDS_PER_DAY +
        static_cast<int64_t>(time->seconds_since_midnight());
    return DateTime(seconds);
}
