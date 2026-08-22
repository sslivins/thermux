/**
 * @file test_version_compare.c
 * @brief Unit tests for version comparison
 */

#include "unity.h"
#include "version_utils.h"

/* Test basic version comparison */
void test_version_equal(void)
{
    TEST_ASSERT_EQUAL_INT(0, version_compare("1.0.0", "1.0.0"));
    TEST_ASSERT_EQUAL_INT(0, version_compare("v1.0.0", "v1.0.0"));
    TEST_ASSERT_EQUAL_INT(0, version_compare("1.2.3", "1.2.3"));
}

void test_version_with_v_prefix(void)
{
    TEST_ASSERT_EQUAL_INT(0, version_compare("v1.0.0", "1.0.0"));
    TEST_ASSERT_EQUAL_INT(0, version_compare("1.0.0", "v1.0.0"));
    TEST_ASSERT_EQUAL_INT(0, version_compare("V1.0.0", "v1.0.0"));
}

void test_version_major_difference(void)
{
    TEST_ASSERT_GREATER_THAN(0, version_compare("2.0.0", "1.0.0"));
    TEST_ASSERT_LESS_THAN(0, version_compare("1.0.0", "2.0.0"));
    TEST_ASSERT_GREATER_THAN(0, version_compare("10.0.0", "9.0.0"));
}

void test_version_minor_difference(void)
{
    TEST_ASSERT_GREATER_THAN(0, version_compare("1.1.0", "1.0.0"));
    TEST_ASSERT_LESS_THAN(0, version_compare("1.0.0", "1.1.0"));
    TEST_ASSERT_GREATER_THAN(0, version_compare("1.10.0", "1.9.0"));
}

void test_version_patch_difference(void)
{
    TEST_ASSERT_GREATER_THAN(0, version_compare("1.0.1", "1.0.0"));
    TEST_ASSERT_LESS_THAN(0, version_compare("1.0.0", "1.0.1"));
    /* Critical test: 1.0.10 > 1.0.9 (string compare would fail this) */
    TEST_ASSERT_GREATER_THAN(0, version_compare("1.0.10", "1.0.9"));
    TEST_ASSERT_GREATER_THAN(0, version_compare("v1.0.12", "v1.0.9"));
}

void test_version_is_newer(void)
{
    TEST_ASSERT_TRUE(version_is_newer("1.0.1", "1.0.0"));
    TEST_ASSERT_TRUE(version_is_newer("v1.0.10", "v1.0.9"));
    TEST_ASSERT_FALSE(version_is_newer("1.0.0", "1.0.1"));
    TEST_ASSERT_FALSE(version_is_newer("1.0.0", "1.0.0"));
}

void test_version_null_handling(void)
{
    TEST_ASSERT_EQUAL_INT(0, version_compare(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, version_compare(NULL, "1.0.0"));
    TEST_ASSERT_EQUAL_INT(0, version_compare("1.0.0", NULL));
}

void test_version_partial(void)
{
    /* Partial versions should work (missing parts = 0) */
    TEST_ASSERT_EQUAL_INT(0, version_compare("1.0", "1.0.0"));
    TEST_ASSERT_EQUAL_INT(0, version_compare("1", "1.0.0"));
}

void test_version_prerelease_lower_than_release(void)
{
    /* A pre-release is lower precedence than its own final release */
    TEST_ASSERT_LESS_THAN(0, version_compare("1.2.0-beta.1", "1.2.0"));
    TEST_ASSERT_GREATER_THAN(0, version_compare("1.2.0", "1.2.0-beta.1"));
    TEST_ASSERT_LESS_THAN(0, version_compare("v1.2.0-rc.1", "v1.2.0"));
}

void test_version_prerelease_ordering(void)
{
    /* Numeric identifiers compare numerically */
    TEST_ASSERT_LESS_THAN(0, version_compare("1.2.0-beta.1", "1.2.0-beta.2"));
    TEST_ASSERT_GREATER_THAN(0, version_compare("1.2.0-beta.10", "1.2.0-beta.9"));
    /* Alphanumeric identifiers compare lexically */
    TEST_ASSERT_LESS_THAN(0, version_compare("1.2.0-alpha", "1.2.0-beta"));
    /* Numeric identifiers always sort below alphanumeric ones */
    TEST_ASSERT_LESS_THAN(0, version_compare("1.2.0-1", "1.2.0-beta"));
    /* A prefix identifier list has lower precedence than a longer one */
    TEST_ASSERT_LESS_THAN(0, version_compare("1.2.0-beta", "1.2.0-beta.1"));
    /* Equal pre-release strings are equal */
    TEST_ASSERT_EQUAL_INT(0, version_compare("1.2.0-beta.1", "1.2.0-beta.1"));
}

void test_version_prerelease_does_not_affect_numeric_core(void)
{
    /* The numeric core still takes priority over any pre-release suffix */
    TEST_ASSERT_LESS_THAN(0, version_compare("1.2.0-beta.99", "1.3.0-beta.1"));
    TEST_ASSERT_GREATER_THAN(0, version_compare("1.3.0-beta.1", "1.2.0-beta.99"));
}

void test_version_prerelease_channel_upgrade_path(void)
{
    /* Mirrors the real device upgrade path: a device on a beta build
     * should see the final release of that same version as newer. */
    TEST_ASSERT_TRUE(version_is_newer("3.2.0", "3.2.0-beta.1"));
    TEST_ASSERT_FALSE(version_is_newer("3.2.0-beta.1", "3.2.0"));
    TEST_ASSERT_TRUE(version_is_newer("3.2.0-beta.2", "3.2.0-beta.1"));
}

void run_version_tests(void)
{
    RUN_TEST(test_version_equal);
    RUN_TEST(test_version_with_v_prefix);
    RUN_TEST(test_version_major_difference);
    RUN_TEST(test_version_minor_difference);
    RUN_TEST(test_version_patch_difference);
    RUN_TEST(test_version_is_newer);
    RUN_TEST(test_version_null_handling);
    RUN_TEST(test_version_partial);
    RUN_TEST(test_version_prerelease_lower_than_release);
    RUN_TEST(test_version_prerelease_ordering);
    RUN_TEST(test_version_prerelease_does_not_affect_numeric_core);
    RUN_TEST(test_version_prerelease_channel_upgrade_path);
}
