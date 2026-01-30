/**
 * @file test_runner.c
 * @brief Main test runner
 */

#include "unity.h"

/* Test suites */
extern void run_version_tests(void);
extern void run_address_tests(void);

int main(void)
{
    UNITY_BEGIN();
    
    printf("\n[Version Comparison Tests]\n");
    run_version_tests();
    
    printf("\n[Address Utilities Tests]\n");
    run_address_tests();
    
    UNITY_END();
    
    return unity_tests_failed > 0 ? 1 : 0;
}
