/* Unity Test Framework - Minimal Version for Host Testing
 * Based on ThrowTheSwitch/Unity - MIT License
 * https://github.com/ThrowTheSwitch/Unity
 */

#ifndef UNITY_H
#define UNITY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Test counters - defined in unity.c */
extern int unity_tests_run;
extern int unity_tests_failed;
extern int unity_tests_passed;
extern const char *unity_current_test;

/* ANSI color codes */
#define UNITY_COLOR_RED    "\033[31m"
#define UNITY_COLOR_GREEN  "\033[32m"
#define UNITY_COLOR_YELLOW "\033[33m"
#define UNITY_COLOR_RESET  "\033[0m"

/* Core macros */
#define UNITY_BEGIN() do { \
    unity_tests_run = 0; \
    unity_tests_failed = 0; \
    unity_tests_passed = 0; \
    printf("\n-----------------------\n"); \
    printf("Running Unit Tests\n"); \
    printf("-----------------------\n"); \
} while(0)

#define UNITY_END() do { \
    printf("-----------------------\n"); \
    if (unity_tests_failed > 0) { \
        printf(UNITY_COLOR_RED "%d FAILED" UNITY_COLOR_RESET ", %d passed, %d total\n", \
            unity_tests_failed, unity_tests_passed, unity_tests_run); \
    } else { \
        printf(UNITY_COLOR_GREEN "ALL %d TESTS PASSED" UNITY_COLOR_RESET "\n", unity_tests_passed); \
    } \
    printf("-----------------------\n\n"); \
} while(0)

#define RUN_TEST(func) do { \
    unity_current_test = #func; \
    unity_tests_run++; \
    printf("  %s... ", #func); \
    fflush(stdout); \
    func(); \
    unity_tests_passed++; \
    printf(UNITY_COLOR_GREEN "PASSED" UNITY_COLOR_RESET "\n"); \
} while(0)

/* Assertion macros */
#define TEST_FAIL_MESSAGE(msg) do { \
    unity_tests_failed++; \
    unity_tests_passed--; \
    printf(UNITY_COLOR_RED "FAILED" UNITY_COLOR_RESET "\n"); \
    printf("    %s:%d: %s\n", __FILE__, __LINE__, msg); \
    return; \
} while(0)

#define TEST_ASSERT(condition) do { \
    if (!(condition)) { \
        TEST_FAIL_MESSAGE("Assertion failed: " #condition); \
    } \
} while(0)

#define TEST_ASSERT_TRUE(condition) TEST_ASSERT(condition)
#define TEST_ASSERT_FALSE(condition) TEST_ASSERT(!(condition))

#define TEST_ASSERT_EQUAL_INT(expected, actual) do { \
    int _e = (expected); \
    int _a = (actual); \
    if (_e != _a) { \
        char _msg[128]; \
        snprintf(_msg, sizeof(_msg), "Expected %d but was %d", _e, _a); \
        TEST_FAIL_MESSAGE(_msg); \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) do { \
    const char *_e = (expected); \
    const char *_a = (actual); \
    if (_e == NULL && _a == NULL) break; \
    if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) { \
        char _msg[256]; \
        snprintf(_msg, sizeof(_msg), "Expected \"%s\" but was \"%s\"", \
            _e ? _e : "(null)", _a ? _a : "(null)"); \
        TEST_FAIL_MESSAGE(_msg); \
    } \
} while(0)

#define TEST_ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        TEST_FAIL_MESSAGE("Expected NULL"); \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        TEST_FAIL_MESSAGE("Expected non-NULL"); \
    } \
} while(0)

#define TEST_ASSERT_GREATER_THAN(threshold, actual) do { \
    int _t = (threshold); \
    int _a = (actual); \
    if (_a <= _t) { \
        char _msg[128]; \
        snprintf(_msg, sizeof(_msg), "Expected > %d but was %d", _t, _a); \
        TEST_FAIL_MESSAGE(_msg); \
    } \
} while(0)

#define TEST_ASSERT_LESS_THAN(threshold, actual) do { \
    int _t = (threshold); \
    int _a = (actual); \
    if (_a >= _t) { \
        char _msg[128]; \
        snprintf(_msg, sizeof(_msg), "Expected < %d but was %d", _t, _a); \
        TEST_FAIL_MESSAGE(_msg); \
    } \
} while(0)

#endif /* UNITY_H */
