/**
 * @file test_config_utils.c
 * @brief Unit tests for configuration validation
 */

#include "unity.h"
#include "config_utils.h"
#include <string.h>

/* ===== Read Interval Tests ===== */

void test_config_read_interval_valid(void)
{
    uint32_t clamped;
    
    /* Valid values */
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_read_interval(5000, &clamped));
    TEST_ASSERT_EQUAL_INT(5000, clamped);
    
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_read_interval(10000, &clamped));
    TEST_ASSERT_EQUAL_INT(10000, clamped);
    
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_read_interval(300000, &clamped));
    TEST_ASSERT_EQUAL_INT(300000, clamped);
}

void test_config_read_interval_too_low(void)
{
    uint32_t clamped;
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LOW, config_validate_read_interval(0, &clamped));
    TEST_ASSERT_EQUAL_INT(5000, clamped);  /* Clamped to min */
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LOW, config_validate_read_interval(500, &clamped));
    TEST_ASSERT_EQUAL_INT(5000, clamped);
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LOW, config_validate_read_interval(4999, &clamped));
    TEST_ASSERT_EQUAL_INT(5000, clamped);
}

void test_config_read_interval_too_high(void)
{
    uint32_t clamped;
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_HIGH, config_validate_read_interval(300001, &clamped));
    TEST_ASSERT_EQUAL_INT(300000, clamped);  /* Clamped to max */
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_HIGH, config_validate_read_interval(1000000, &clamped));
    TEST_ASSERT_EQUAL_INT(300000, clamped);
}

void test_config_read_interval_null_output(void)
{
    /* Should not crash with NULL output */
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_read_interval(10000, NULL));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LOW, config_validate_read_interval(0, NULL));
}

/* ===== Publish Interval Tests ===== */

void test_config_publish_interval_valid(void)
{
    uint32_t clamped;
    
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_publish_interval(5000, &clamped));
    TEST_ASSERT_EQUAL_INT(5000, clamped);
    
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_publish_interval(30000, &clamped));
    TEST_ASSERT_EQUAL_INT(30000, clamped);
    
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_publish_interval(600000, &clamped));
    TEST_ASSERT_EQUAL_INT(600000, clamped);
}

void test_config_publish_interval_too_low(void)
{
    uint32_t clamped;
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LOW, config_validate_publish_interval(0, &clamped));
    TEST_ASSERT_EQUAL_INT(5000, clamped);
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LOW, config_validate_publish_interval(4999, &clamped));
    TEST_ASSERT_EQUAL_INT(5000, clamped);
}

void test_config_publish_interval_too_high(void)
{
    uint32_t clamped;
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_HIGH, config_validate_publish_interval(600001, &clamped));
    TEST_ASSERT_EQUAL_INT(600000, clamped);
}

/* ===== Resolution Tests ===== */

void test_config_resolution_valid(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_resolution(9));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_resolution(10));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_resolution(11));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_resolution(12));
}

void test_config_resolution_invalid(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LOW, config_validate_resolution(8));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LOW, config_validate_resolution(0));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_HIGH, config_validate_resolution(13));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_HIGH, config_validate_resolution(255));
}

/* ===== Friendly Name Tests ===== */

void test_config_friendly_name_valid(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_friendly_name("Kitchen"));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_friendly_name("Living Room Sensor"));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_friendly_name("A"));  /* Min length */
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_friendly_name("1234567890123456789012345678901")); /* 31 chars - max */
}

void test_config_friendly_name_null(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_NULL_INPUT, config_validate_friendly_name(NULL));
}

void test_config_friendly_name_empty(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_SHORT, config_validate_friendly_name(""));
}

void test_config_friendly_name_too_long(void)
{
    /* 32 chars - exceeds limit */
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LONG, 
        config_validate_friendly_name("12345678901234567890123456789012"));
}

void test_config_friendly_name_invalid_chars(void)
{
    /* Non-printable characters */
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_INVALID_CHARS, config_validate_friendly_name("Test\n"));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_INVALID_CHARS, config_validate_friendly_name("Test\t"));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_INVALID_CHARS, config_validate_friendly_name("\x01Test"));
}

/* ===== MQTT URI Tests ===== */

void test_config_mqtt_uri_valid(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_mqtt_uri("mqtt://192.168.1.100:1883"));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_mqtt_uri("mqtt://broker.local"));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_mqtt_uri("mqtts://secure.broker.com:8883"));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_mqtt_uri("ws://broker.local:9001"));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_mqtt_uri("wss://broker.local:9001"));
}

void test_config_mqtt_uri_null(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_NULL_INPUT, config_validate_mqtt_uri(NULL));
}

void test_config_mqtt_uri_empty(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_SHORT, config_validate_mqtt_uri(""));
}

void test_config_mqtt_uri_invalid_prefix(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_INVALID_FORMAT, config_validate_mqtt_uri("http://broker.com"));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_INVALID_FORMAT, config_validate_mqtt_uri("tcp://broker.com"));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_INVALID_FORMAT, config_validate_mqtt_uri("192.168.1.100:1883"));
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_INVALID_FORMAT, config_validate_mqtt_uri("broker.local"));
}

void test_config_mqtt_uri_too_long(void)
{
    /* Build a URI that exceeds 127 chars */
    char long_uri[200];
    strcpy(long_uri, "mqtt://");
    for (int i = 7; i < 150; i++) {
        long_uri[i] = 'a';
    }
    long_uri[150] = '\0';
    
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LONG, config_validate_mqtt_uri(long_uri));
}

/* ===== WiFi SSID Tests ===== */

void test_config_wifi_ssid_valid(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_wifi_ssid("MyNetwork"));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_wifi_ssid("A"));
    TEST_ASSERT_EQUAL_INT(CONFIG_VALID, config_validate_wifi_ssid("1234567890123456789012345678901")); /* 31 chars */
}

void test_config_wifi_ssid_null(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_NULL_INPUT, config_validate_wifi_ssid(NULL));
}

void test_config_wifi_ssid_empty(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_SHORT, config_validate_wifi_ssid(""));
}

void test_config_wifi_ssid_too_long(void)
{
    TEST_ASSERT_EQUAL_INT(CONFIG_ERR_TOO_LONG, 
        config_validate_wifi_ssid("12345678901234567890123456789012")); /* 32 chars */
}

/* ===== Printable ASCII Tests ===== */

void test_config_is_printable_ascii_valid(void)
{
    TEST_ASSERT_TRUE(config_is_printable_ascii("Hello World!"));
    TEST_ASSERT_TRUE(config_is_printable_ascii("Test 123 @#$%"));
    TEST_ASSERT_TRUE(config_is_printable_ascii(" "));  /* Space is printable */
    TEST_ASSERT_TRUE(config_is_printable_ascii("~"));  /* Tilde (126) */
}

void test_config_is_printable_ascii_invalid(void)
{
    TEST_ASSERT_FALSE(config_is_printable_ascii("Test\n"));    /* Newline */
    TEST_ASSERT_FALSE(config_is_printable_ascii("Test\t"));    /* Tab */
    TEST_ASSERT_FALSE(config_is_printable_ascii("\x1F"));      /* Control char */
    TEST_ASSERT_FALSE(config_is_printable_ascii("\x7F"));      /* DEL */
    TEST_ASSERT_FALSE(config_is_printable_ascii("\x80Test"));  /* High ASCII */
}

void test_config_is_printable_ascii_null(void)
{
    TEST_ASSERT_FALSE(config_is_printable_ascii(NULL));
}

/* ===== Error String Tests ===== */

void test_config_error_str(void)
{
    TEST_ASSERT_EQUAL_STRING("Valid", config_error_str(CONFIG_VALID));
    TEST_ASSERT_EQUAL_STRING("Null input", config_error_str(CONFIG_ERR_NULL_INPUT));
    TEST_ASSERT_EQUAL_STRING("Value too low", config_error_str(CONFIG_ERR_TOO_LOW));
    TEST_ASSERT_EQUAL_STRING("Value too high", config_error_str(CONFIG_ERR_TOO_HIGH));
    TEST_ASSERT_EQUAL_STRING("String too short", config_error_str(CONFIG_ERR_TOO_SHORT));
    TEST_ASSERT_EQUAL_STRING("String too long", config_error_str(CONFIG_ERR_TOO_LONG));
    TEST_ASSERT_EQUAL_STRING("Invalid format", config_error_str(CONFIG_ERR_INVALID_FORMAT));
    TEST_ASSERT_EQUAL_STRING("Invalid characters", config_error_str(CONFIG_ERR_INVALID_CHARS));
    TEST_ASSERT_EQUAL_STRING("Unknown error", config_error_str(99));
}

/* ===== Test Runner ===== */

void run_config_tests(void)
{
    /* Read interval tests */
    RUN_TEST(test_config_read_interval_valid);
    RUN_TEST(test_config_read_interval_too_low);
    RUN_TEST(test_config_read_interval_too_high);
    RUN_TEST(test_config_read_interval_null_output);
    
    /* Publish interval tests */
    RUN_TEST(test_config_publish_interval_valid);
    RUN_TEST(test_config_publish_interval_too_low);
    RUN_TEST(test_config_publish_interval_too_high);
    
    /* Resolution tests */
    RUN_TEST(test_config_resolution_valid);
    RUN_TEST(test_config_resolution_invalid);
    
    /* Friendly name tests */
    RUN_TEST(test_config_friendly_name_valid);
    RUN_TEST(test_config_friendly_name_null);
    RUN_TEST(test_config_friendly_name_empty);
    RUN_TEST(test_config_friendly_name_too_long);
    RUN_TEST(test_config_friendly_name_invalid_chars);
    
    /* MQTT URI tests */
    RUN_TEST(test_config_mqtt_uri_valid);
    RUN_TEST(test_config_mqtt_uri_null);
    RUN_TEST(test_config_mqtt_uri_empty);
    RUN_TEST(test_config_mqtt_uri_invalid_prefix);
    RUN_TEST(test_config_mqtt_uri_too_long);
    
    /* WiFi SSID tests */
    RUN_TEST(test_config_wifi_ssid_valid);
    RUN_TEST(test_config_wifi_ssid_null);
    RUN_TEST(test_config_wifi_ssid_empty);
    RUN_TEST(test_config_wifi_ssid_too_long);
    
    /* Printable ASCII tests */
    RUN_TEST(test_config_is_printable_ascii_valid);
    RUN_TEST(test_config_is_printable_ascii_invalid);
    RUN_TEST(test_config_is_printable_ascii_null);
    
    /* Error string test */
    RUN_TEST(test_config_error_str);
}
