#pragma once

#include <cstdint>
#include <optional>
#include <string>

class Date {
public:
    explicit Date(int32_t days_since_epoch);

    int32_t days_since_epoch() const;
    std::string to_string() const;

    static std::optional<Date> from_string(const std::string& text);

private:
    int32_t days_since_epoch_;
};

class Time {
public:
    explicit Time(uint32_t seconds_since_midnight);

    uint32_t seconds_since_midnight() const;
    std::string to_string() const;

    static std::optional<Time> from_string(const std::string& text);

private:
    uint32_t seconds_since_midnight_;
};

class DateTime {
public:
    explicit DateTime(int64_t seconds_since_epoch);

    int64_t seconds_since_epoch() const;
    std::string to_string() const;

    static std::optional<DateTime> from_string(const std::string& text);

private:
    int64_t seconds_since_epoch_;
};
