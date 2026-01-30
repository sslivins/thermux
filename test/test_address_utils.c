/**
 * @file test_address_utils.c
 * @brief Unit tests for sensor address utilities
 */

#include "unity.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Function under test - replicated from sensor_manager for testing */
static void address_to_string(const uint8_t *address, char *str, size_t str_len)
{
    if (str_len < 17) return;  /* Need 16 chars + null */
    snprintf(str, str_len, "%02X%02X%02X%02X%02X%02X%02X%02X",
             address[0], address[1], address[2], address[3],
             address[4], address[5], address[6], address[7]);
}

static int string_to_address(const char *str, uint8_t *address)
{
    if (strlen(str) != 16) return -1;
    
    for (int i = 0; i < 8; i++) {
        unsigned int byte;
        if (sscanf(str + i * 2, "%02X", &byte) != 1) {
            return -1;
        }
        address[i] = (uint8_t)byte;
    }
    return 0;
}

void test_address_to_string(void)
{
    uint8_t addr[] = {0x28, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    char str[17];
    
    address_to_string(addr, str, sizeof(str));
    TEST_ASSERT_EQUAL_STRING("28FF123456789ABC", str);
}

void test_address_to_string_zeros(void)
{
    uint8_t addr[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    char str[17];
    
    address_to_string(addr, str, sizeof(str));
    TEST_ASSERT_EQUAL_STRING("0000000000000000", str);
}

void test_string_to_address(void)
{
    uint8_t addr[8];
    int result = string_to_address("28FF123456789ABC", addr);
    
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(0x28, addr[0]);
    TEST_ASSERT_EQUAL_INT(0xFF, addr[1]);
    TEST_ASSERT_EQUAL_INT(0x12, addr[2]);
    TEST_ASSERT_EQUAL_INT(0x34, addr[3]);
    TEST_ASSERT_EQUAL_INT(0x56, addr[4]);
    TEST_ASSERT_EQUAL_INT(0x78, addr[5]);
    TEST_ASSERT_EQUAL_INT(0x9A, addr[6]);
    TEST_ASSERT_EQUAL_INT(0xBC, addr[7]);
}

void test_string_to_address_lowercase(void)
{
    uint8_t addr[8];
    /* Should handle lowercase hex (sscanf %X handles both) */
    int result = string_to_address("28ff123456789abc", addr);
    
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(0x28, addr[0]);
    TEST_ASSERT_EQUAL_INT(0xFF, addr[1]);
}

void test_string_to_address_invalid_length(void)
{
    uint8_t addr[8];
    
    TEST_ASSERT_EQUAL_INT(-1, string_to_address("28FF", addr));
    TEST_ASSERT_EQUAL_INT(-1, string_to_address("28FF123456789ABCDE", addr));
    TEST_ASSERT_EQUAL_INT(-1, string_to_address("", addr));
}

void test_roundtrip(void)
{
    uint8_t original[] = {0x28, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67};
    uint8_t recovered[8];
    char str[17];
    
    address_to_string(original, str, sizeof(str));
    string_to_address(str, recovered);
    
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT(original[i], recovered[i]);
    }
}

void run_address_tests(void)
{
    RUN_TEST(test_address_to_string);
    RUN_TEST(test_address_to_string_zeros);
    RUN_TEST(test_string_to_address);
    RUN_TEST(test_string_to_address_lowercase);
    RUN_TEST(test_string_to_address_invalid_length);
    RUN_TEST(test_roundtrip);
}
