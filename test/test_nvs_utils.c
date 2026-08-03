/**
 * @file test_nvs_utils.c
 * @brief Unit tests for NVS utilities
 */

#include "unity.h"
#include "nvs_utils.h"
#include <string.h>

/* ===== Sensor Key Generation Tests ===== */

void test_nvs_generate_sensor_key_basic(void)
{
    uint8_t addr[] = {0x28, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    char key[16];
    
    int result = nvs_generate_sensor_key(addr, key, sizeof(key));
    
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_STRING("s_ff123456789a", key);  /* Unique serial bytes 1-6 */
}

void test_nvs_generate_sensor_key_zeros(void)
{
    uint8_t addr[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    char key[16];
    
    int result = nvs_generate_sensor_key(addr, key, sizeof(key));
    
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_STRING("s_000000000000", key);
}

void test_nvs_generate_sensor_key_max_values(void)
{
    uint8_t addr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    char key[16];
    
    int result = nvs_generate_sensor_key(addr, key, sizeof(key));
    
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_STRING("s_ffffffffffff", key);
}

void test_nvs_generate_sensor_key_length_check(void)
{
    uint8_t addr[] = {0x28, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    char key[16];
    
    /* Key is "s_ff123456789a" = 14 chars, within 15 char NVS limit */
    nvs_generate_sensor_key(addr, key, sizeof(key));
    
    TEST_ASSERT_TRUE(strlen(key) <= NVS_KEY_MAX_LEN);
}

/* Regression test for the friendly-name collision bug: two sensors whose
 * ROM addresses differ ONLY in the unique serial bytes 1-3 (and share bytes
 * 4-7) must produce DISTINCT keys. The old scheme keyed on bytes 4-7 only and
 * collided here, causing naming one sensor to alias another. */
void test_nvs_generate_sensor_key_no_collision(void)
{
    uint8_t a[] = {0x28, 0x11, 0x22, 0x33, 0x56, 0x78, 0x9A, 0xBC};
    uint8_t b[] = {0x28, 0xAA, 0xBB, 0xCC, 0x56, 0x78, 0x9A, 0xBC};
    char key_a[16];
    char key_b[16];
    
    TEST_ASSERT_EQUAL_INT(0, nvs_generate_sensor_key(a, key_a, sizeof(key_a)));
    TEST_ASSERT_EQUAL_INT(0, nvs_generate_sensor_key(b, key_b, sizeof(key_b)));
    
    TEST_ASSERT_EQUAL_STRING("s_11223356789a", key_a);
    TEST_ASSERT_EQUAL_STRING("s_aabbcc56789a", key_b);
    TEST_ASSERT_FALSE(strcmp(key_a, key_b) == 0);
}

void test_nvs_generate_sensor_key_buffer_too_small(void)
{
    uint8_t addr[] = {0x28, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    char key[5];  /* Too small - needs at least 15 */
    
    int result = nvs_generate_sensor_key(addr, key, sizeof(key));
    
    TEST_ASSERT_EQUAL_INT(-1, result);
}

void test_nvs_generate_sensor_key_null_inputs(void)
{
    uint8_t addr[] = {0x28, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    char key[16];
    
    TEST_ASSERT_EQUAL_INT(-1, nvs_generate_sensor_key(NULL, key, sizeof(key)));
    TEST_ASSERT_EQUAL_INT(-1, nvs_generate_sensor_key(addr, NULL, sizeof(key)));
}

/* ===== Key Validation Tests ===== */

void test_nvs_validate_key_valid(void)
{
    TEST_ASSERT_TRUE(nvs_validate_key("s_12345678"));
    TEST_ASSERT_TRUE(nvs_validate_key("mqtt_uri"));
    TEST_ASSERT_TRUE(nvs_validate_key("wifi_ssid"));
    TEST_ASSERT_TRUE(nvs_validate_key("a"));  /* Min length */
    TEST_ASSERT_TRUE(nvs_validate_key("123456789012345"));  /* 15 chars - max */
    TEST_ASSERT_TRUE(nvs_validate_key("key_with_under"));
}

void test_nvs_validate_key_invalid_length(void)
{
    TEST_ASSERT_FALSE(nvs_validate_key(""));  /* Empty */
    TEST_ASSERT_FALSE(nvs_validate_key("1234567890123456"));  /* 16 chars - too long */
    TEST_ASSERT_FALSE(nvs_validate_key("this_key_is_way_too_long"));
}

void test_nvs_validate_key_invalid_chars(void)
{
    TEST_ASSERT_FALSE(nvs_validate_key("key-with-dash"));
    TEST_ASSERT_FALSE(nvs_validate_key("key.with.dot"));
    TEST_ASSERT_FALSE(nvs_validate_key("key with space"));
    TEST_ASSERT_FALSE(nvs_validate_key("key/slash"));
    TEST_ASSERT_FALSE(nvs_validate_key("key@special"));
}

void test_nvs_validate_key_null(void)
{
    TEST_ASSERT_FALSE(nvs_validate_key(NULL));
}

/* ===== Max Value Length Tests ===== */

void test_nvs_get_max_value_len_sensor(void)
{
    TEST_ASSERT_EQUAL_INT(32, nvs_get_max_value_len("s_123456789012"));
    TEST_ASSERT_EQUAL_INT(32, nvs_get_max_value_len("s_aabbccddeeff"));
}

void test_nvs_get_max_value_len_mqtt(void)
{
    TEST_ASSERT_EQUAL_INT(128, nvs_get_max_value_len("mqtt_uri"));
    TEST_ASSERT_EQUAL_INT(64, nvs_get_max_value_len("mqtt_user"));
    TEST_ASSERT_EQUAL_INT(64, nvs_get_max_value_len("mqtt_pass"));
}

void test_nvs_get_max_value_len_wifi(void)
{
    TEST_ASSERT_EQUAL_INT(32, nvs_get_max_value_len("wifi_ssid"));
    TEST_ASSERT_EQUAL_INT(64, nvs_get_max_value_len("wifi_pass"));
}

void test_nvs_get_max_value_len_invalid(void)
{
    TEST_ASSERT_EQUAL_INT(0, nvs_get_max_value_len(NULL));
    TEST_ASSERT_EQUAL_INT(0, nvs_get_max_value_len(""));
    TEST_ASSERT_EQUAL_INT(0, nvs_get_max_value_len("invalid-key"));  /* Has dash */
}

/* ===== Sensor Key Detection Tests ===== */

void test_nvs_is_sensor_key_valid(void)
{
    TEST_ASSERT_TRUE(nvs_is_sensor_key("s_123456789012"));
    TEST_ASSERT_TRUE(nvs_is_sensor_key("s_aabbccddeeff"));
    TEST_ASSERT_TRUE(nvs_is_sensor_key("s_000000000000"));
    TEST_ASSERT_TRUE(nvs_is_sensor_key("s_ffffffffffff"));
    TEST_ASSERT_TRUE(nvs_is_sensor_key("s_AABBCCDDEEFF"));  /* Uppercase hex */
}

void test_nvs_is_sensor_key_invalid(void)
{
    TEST_ASSERT_FALSE(nvs_is_sensor_key("mqtt_uri"));
    TEST_ASSERT_FALSE(nvs_is_sensor_key("wifi_ssid"));
    TEST_ASSERT_FALSE(nvs_is_sensor_key("s_123"));            /* Too short */
    TEST_ASSERT_FALSE(nvs_is_sensor_key("s_56789abc"));       /* Old 10-char format */
    TEST_ASSERT_FALSE(nvs_is_sensor_key("s_1234567890123"));  /* 13 hex - wrong length */
    TEST_ASSERT_FALSE(nvs_is_sensor_key("s_12345678901g"));   /* Non-hex char */
    TEST_ASSERT_FALSE(nvs_is_sensor_key("x_123456789012"));   /* Wrong prefix */
}

void test_nvs_is_sensor_key_null(void)
{
    TEST_ASSERT_FALSE(nvs_is_sensor_key(NULL));
}

/* ===== Sensor Key Parsing Tests ===== */

void test_nvs_parse_sensor_key_basic(void)
{
    uint8_t partial_addr[6];
    
    int result = nvs_parse_sensor_key("s_ff123456789a", partial_addr);
    
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(0xFF, partial_addr[0]);
    TEST_ASSERT_EQUAL_INT(0x12, partial_addr[1]);
    TEST_ASSERT_EQUAL_INT(0x34, partial_addr[2]);
    TEST_ASSERT_EQUAL_INT(0x56, partial_addr[3]);
    TEST_ASSERT_EQUAL_INT(0x78, partial_addr[4]);
    TEST_ASSERT_EQUAL_INT(0x9A, partial_addr[5]);
}

void test_nvs_parse_sensor_key_zeros(void)
{
    uint8_t partial_addr[6];
    
    int result = nvs_parse_sensor_key("s_000000000000", partial_addr);
    
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(0x00, partial_addr[0]);
    TEST_ASSERT_EQUAL_INT(0x00, partial_addr[1]);
    TEST_ASSERT_EQUAL_INT(0x00, partial_addr[2]);
    TEST_ASSERT_EQUAL_INT(0x00, partial_addr[3]);
    TEST_ASSERT_EQUAL_INT(0x00, partial_addr[4]);
    TEST_ASSERT_EQUAL_INT(0x00, partial_addr[5]);
}

void test_nvs_parse_sensor_key_invalid(void)
{
    uint8_t partial_addr[6];
    
    TEST_ASSERT_EQUAL_INT(-1, nvs_parse_sensor_key("mqtt_uri", partial_addr));
    TEST_ASSERT_EQUAL_INT(-1, nvs_parse_sensor_key("s_123", partial_addr));
    TEST_ASSERT_EQUAL_INT(-1, nvs_parse_sensor_key("s_56789abc", partial_addr)); /* Old format */
    TEST_ASSERT_EQUAL_INT(-1, nvs_parse_sensor_key(NULL, partial_addr));
}

void test_nvs_parse_sensor_key_null_output(void)
{
    TEST_ASSERT_EQUAL_INT(-1, nvs_parse_sensor_key("s_123456789012", NULL));
}

/* ===== Roundtrip Test ===== */

void test_nvs_key_roundtrip(void)
{
    uint8_t original_addr[] = {0x28, 0xFF, 0x12, 0x34, 0xAB, 0xCD, 0xEF, 0x01};
    char key[16];
    uint8_t recovered_partial[6];
    
    /* Generate key from address */
    int gen_result = nvs_generate_sensor_key(original_addr, key, sizeof(key));
    TEST_ASSERT_EQUAL_INT(0, gen_result);
    
    /* Parse partial address back from key */
    int parse_result = nvs_parse_sensor_key(key, recovered_partial);
    TEST_ASSERT_EQUAL_INT(0, parse_result);
    
    /* Verify unique serial bytes (1-6) match */
    TEST_ASSERT_EQUAL_INT(original_addr[1], recovered_partial[0]);
    TEST_ASSERT_EQUAL_INT(original_addr[2], recovered_partial[1]);
    TEST_ASSERT_EQUAL_INT(original_addr[3], recovered_partial[2]);
    TEST_ASSERT_EQUAL_INT(original_addr[4], recovered_partial[3]);
    TEST_ASSERT_EQUAL_INT(original_addr[5], recovered_partial[4]);
    TEST_ASSERT_EQUAL_INT(original_addr[6], recovered_partial[5]);
}

/* ===== Test Runner ===== */

void run_nvs_tests(void)
{
    /* Sensor key generation tests */
    RUN_TEST(test_nvs_generate_sensor_key_basic);
    RUN_TEST(test_nvs_generate_sensor_key_zeros);
    RUN_TEST(test_nvs_generate_sensor_key_max_values);
    RUN_TEST(test_nvs_generate_sensor_key_length_check);
    RUN_TEST(test_nvs_generate_sensor_key_no_collision);
    RUN_TEST(test_nvs_generate_sensor_key_buffer_too_small);
    RUN_TEST(test_nvs_generate_sensor_key_null_inputs);
    
    /* Key validation tests */
    RUN_TEST(test_nvs_validate_key_valid);
    RUN_TEST(test_nvs_validate_key_invalid_length);
    RUN_TEST(test_nvs_validate_key_invalid_chars);
    RUN_TEST(test_nvs_validate_key_null);
    
    /* Max value length tests */
    RUN_TEST(test_nvs_get_max_value_len_sensor);
    RUN_TEST(test_nvs_get_max_value_len_mqtt);
    RUN_TEST(test_nvs_get_max_value_len_wifi);
    RUN_TEST(test_nvs_get_max_value_len_invalid);
    
    /* Sensor key detection tests */
    RUN_TEST(test_nvs_is_sensor_key_valid);
    RUN_TEST(test_nvs_is_sensor_key_invalid);
    RUN_TEST(test_nvs_is_sensor_key_null);
    
    /* Sensor key parsing tests */
    RUN_TEST(test_nvs_parse_sensor_key_basic);
    RUN_TEST(test_nvs_parse_sensor_key_zeros);
    RUN_TEST(test_nvs_parse_sensor_key_invalid);
    RUN_TEST(test_nvs_parse_sensor_key_null_output);
    
    /* Roundtrip test */
    RUN_TEST(test_nvs_key_roundtrip);
}
