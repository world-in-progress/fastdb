#pragma once

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace fastdb::test {

inline int require(bool condition,
                   const char* file,
                   int line,
                   const char* expression,
                   std::string_view message = {}) {
    if (condition) {
        return EXIT_SUCCESS;
    }

    std::cerr << file << ':' << line << ": requirement failed: " << expression;
    if (!message.empty()) {
        std::cerr << ": " << message;
    }
    std::cerr << '\n';
    return EXIT_FAILURE;
}

}  // namespace fastdb::test

#define FASTDB_TEST_REQUIRE_IMPL(expression, message)                         \
    do {                                                                      \
        const int fastdb_test_status = ::fastdb::test::require(               \
            static_cast<bool>(expression), __FILE__, __LINE__, #expression,   \
            message);                                                         \
        if (fastdb_test_status != EXIT_SUCCESS) {                             \
            return fastdb_test_status;                                        \
        }                                                                     \
    } while (false)

#define FASTDB_TEST_REQUIRE_1(expression) \
    FASTDB_TEST_REQUIRE_IMPL(expression, std::string_view{})
#define FASTDB_TEST_REQUIRE_2(expression, message) \
    FASTDB_TEST_REQUIRE_IMPL(expression, message)
#define FASTDB_TEST_SELECT_REQUIRE(_1, _2, selected, ...) selected
#define require(...)                                                         \
    FASTDB_TEST_SELECT_REQUIRE(__VA_ARGS__, FASTDB_TEST_REQUIRE_2,            \
                               FASTDB_TEST_REQUIRE_1)(__VA_ARGS__)
