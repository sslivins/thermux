/**
 * @file test_runner.c
 * @brief Main test runner
 */

#include "unity.h"

/* Test suites */
extern void run_version_tests(void);
extern void run_address_tests(void);
extern void run_mqtt_tests(void);
extern void run_config_tests(void);
extern void run_nvs_tests(void);
extern void run_bus_stats_tests(void);
extern void run_modbus_map_tests(void);
extern void run_bacnet_map_tests(void);
extern void run_release_scan_tests(void);

int main(void)
{
    UNITY_BEGIN();
    
    printf("\n[Version Comparison Tests]\n");
    run_version_tests();
    
    printf("\n[Address Utilities Tests]\n");
    run_address_tests();
    
    printf("\n[MQTT Utilities Tests]\n");
    run_mqtt_tests();
    
    printf("\n[Config Validation Tests]\n");
    run_config_tests();
    
    printf("\n[NVS Utilities Tests]\n");
    run_nvs_tests();

    printf("\n[Bus Statistics Tests]\n");
    run_bus_stats_tests();

    printf("\n[Modbus Map Tests]\n");
    run_modbus_map_tests();

    printf("\n[BACnet Map Tests]\n");
    run_bacnet_map_tests();

    printf("\n[Release Scan Tests]\n");
    run_release_scan_tests();
    
    UNITY_END();
    
    return unity_tests_failed > 0 ? 1 : 0;
}
