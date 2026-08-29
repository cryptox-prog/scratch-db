#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

struct TestCase {
    std::string name;
    std::function<void()> body;
};

struct TestSummary {
    int passed = 0;
    int failed = 0;
};

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void print_test_header() {
    std::cout << "+-----+--------------------------------+----------+--------+\n";
    std::cout << "| No. | Test                           | Time     | Result |\n";
    std::cout << "+-----+--------------------------------+----------+--------+\n";
}

inline void print_test_footer(const TestSummary& summary) {
    const int total = summary.passed + summary.failed;
    const double percent = total == 0 ? 0.0 : (100.0 * summary.passed) / total;

    std::cout << "+-----+--------------------------------+----------+--------+\n";
    std::cout << "Passed: " << summary.passed
              << "  Failed: " << summary.failed
              << "  Pass %: " << std::fixed << std::setprecision(2) << percent << "%\n";
}

inline void run_test_row(int serial, const TestCase& test, TestSummary& summary) {
    const auto start = std::chrono::steady_clock::now();
    bool passed = true;
    std::string failure;

    try {
        test.body();
    } catch (const std::exception& error) {
        passed = false;
        failure = error.what();
    } catch (...) {
        passed = false;
        failure = "unknown failure";
    }

    const auto end = std::chrono::steady_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    if (passed) {
        ++summary.passed;
    } else {
        ++summary.failed;
    }

    std::cout << "| " << std::setw(3) << serial << " "
              << "| " << std::left << std::setw(30) << test.name.substr(0, 30) << std::right << " "
              << "| " << std::setw(7) << us << "us "
              << "| " << std::setw(6) << (passed ? "PASS" : "FAIL") << " |\n";

    if (!passed) {
        std::cout << "|     | " << std::left << std::setw(30) << failure.substr(0, 30) << std::right
                  << " |          |        |\n";
    }
}

inline int run_tests(const std::vector<TestCase>& tests) {
    TestSummary summary;
    print_test_header();
    for (std::size_t i = 0; i < tests.size(); ++i) {
        run_test_row(static_cast<int>(i + 1), tests[i], summary);
    }
    print_test_footer(summary);

    return summary.failed == 0 ? 0 : 1;
}
