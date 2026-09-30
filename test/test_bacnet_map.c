/**
 * @file test_bacnet_map.c
 * @brief Unit tests for BACnet/IP mapping helpers
 */

#include "unity.h"
#include "bacnet_map.h"

#include <string.h>

#define TEST_ASSERT_EQUAL(e, a) TEST_ASSERT_EQUAL_INT((int)(e), (int)(a))
#define TEST_ASSERT_EQUAL_UINT32(e, a) TEST_ASSERT_TRUE((uint32_t)(e) == (uint32_t)(a))

void test_bacnet_default_instance_from_mac_is_in_range(void)
{
    uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xFF, 0xFF, 0xFF};
    uint32_t inst = bacnet_default_device_instance_from_mac(mac);
    TEST_ASSERT_TRUE(inst <= BACNET_MAX_DEVICE_INSTANCE);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFU % (BACNET_MAX_DEVICE_INSTANCE + 1U), inst);
}

void test_bacnet_config_validation(void)
{
    TEST_ASSERT_TRUE(bacnet_config_valid(47808, 123, 80));
    TEST_ASSERT_FALSE(bacnet_config_valid(0, 123, 80));
    TEST_ASSERT_FALSE(bacnet_config_valid(80, 123, 80));
    TEST_ASSERT_FALSE(bacnet_config_valid(47808, BACNET_MAX_DEVICE_INSTANCE + 1U, 80));
}

void test_bacnet_reliability_mapping(void)
{
    TEST_ASSERT_EQUAL(BACNET_MAP_RELIABILITY_NO_FAULT_DETECTED, bacnet_reliability_from_modbus_status(MB_STATUS_OK));
    TEST_ASSERT_EQUAL(BACNET_MAP_RELIABILITY_NO_SENSOR, bacnet_reliability_from_modbus_status(MB_STATUS_MISSING));
    TEST_ASSERT_EQUAL(BACNET_MAP_RELIABILITY_COMMUNICATION_FAILURE, bacnet_reliability_from_modbus_status(MB_STATUS_READ_ERROR));
    TEST_ASSERT_EQUAL(BACNET_MAP_RELIABILITY_UNRELIABLE_OTHER, bacnet_reliability_from_modbus_status(MB_STATUS_STALE));
    TEST_ASSERT_TRUE(bacnet_status_fault(MB_STATUS_STALE));
    TEST_ASSERT_FALSE(bacnet_status_fault(MB_STATUS_OK));
}

void test_bacnet_unique_names_fallback_and_dedupe(void)
{
    char existing[3][BACNET_OBJECT_NAME_MAX + 1] = {{0}};
    char out[BACNET_OBJECT_NAME_MAX + 1];
    bacnet_make_unique_ai_name(NULL, 4, existing, 0, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Channel 4", out);
    strcpy(existing[0], "Supply");
    bacnet_make_unique_ai_name("Supply", 7, existing, 1, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Supply (7)", out);
}

void test_bacnet_ai_identity_uses_rom_description(void)
{
    uint8_t rom[MODBUS_ROM_LEN] = {0x28, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xEF};
    char existing[1][BACNET_OBJECT_NAME_MAX + 1] = {{0}};
    bacnet_ai_identity_t ident;
    bacnet_make_ai_identity(2, rom, "HP Return", existing, 0, &ident);
    TEST_ASSERT_EQUAL_STRING("HP Return", ident.object_name);
    TEST_ASSERT_EQUAL_STRING("28123456789ABCEF", ident.description);
}

void run_bacnet_map_tests(void)
{
    RUN_TEST(test_bacnet_default_instance_from_mac_is_in_range);
    RUN_TEST(test_bacnet_config_validation);
    RUN_TEST(test_bacnet_reliability_mapping);
    RUN_TEST(test_bacnet_unique_names_fallback_and_dedupe);
    RUN_TEST(test_bacnet_ai_identity_uses_rom_description);
}
