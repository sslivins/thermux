/**
 * @file test_modbus_map.c
 * @brief Unit tests for the Modbus register map, slot status and slot table
 */

#include "unity.h"
#include "modbus_map.h"

#include <math.h>
#include <string.h>

/* The bundled Unity shim only has a few asserts; map the typed ones onto it */
#define TEST_ASSERT_EQUAL(e, a)         TEST_ASSERT_EQUAL_INT((int)(e), (int)(a))
#define TEST_ASSERT_EQUAL_UINT8(e, a)   TEST_ASSERT_EQUAL_INT((int)(e), (int)(a))
#define TEST_ASSERT_EQUAL_UINT16(e, a)  TEST_ASSERT_EQUAL_INT((int)(uint16_t)(e), (int)(uint16_t)(a))
#define TEST_ASSERT_EQUAL_HEX16(e, a)   TEST_ASSERT_EQUAL_UINT16(e, a)
#define TEST_ASSERT_EQUAL_INT64(e, a)   TEST_ASSERT_TRUE((int64_t)(e) == (int64_t)(a))
#define TEST_ASSERT_EQUAL_HEX32(e, a)   TEST_ASSERT_TRUE((uint32_t)(e) == (uint32_t)(a))
#define TEST_ASSERT_EQUAL_MEMORY(e, a, n) TEST_ASSERT_TRUE(memcmp((e), (a), (n)) == 0)

static void make_rom(uint8_t *rom, uint8_t serial)
{
    const uint8_t base[MODBUS_ROM_LEN] = {0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(rom, base, MODBUS_ROM_LEN);
    rom[1] = serial;
    rom[7] = (uint8_t)(serial ^ 0x5A);
}

static modbus_sensor_input_t good_sensor(uint8_t serial, float temp, int64_t now_ms)
{
    modbus_sensor_input_t s;
    memset(&s, 0, sizeof(s));
    make_rom(s.rom, serial);
    s.temperature = temp;
    s.valid = true;
    s.last_read_ms = now_ms - 1000;
    s.last_attempt_ms = now_ms - 1000;
    return s;
}

/* ---- packing ---- */

void test_modbus_temp_rounding_and_sign(void)
{
    TEST_ASSERT_EQUAL_HEX16(2150, modbus_temp_to_reg(21.5f));
    TEST_ASSERT_EQUAL_HEX16(2106, modbus_temp_to_reg(21.0625f));   /* rounds 2106.25 */
    TEST_ASSERT_EQUAL_HEX16((uint16_t)(int16_t)-1006, modbus_temp_to_reg(-10.0625f));
    TEST_ASSERT_EQUAL_HEX16(0, modbus_temp_to_reg(0.0f));
}

void test_modbus_temp_clamps_and_never_returns_invalid_marker(void)
{
    TEST_ASSERT_EQUAL_HEX16(32767, modbus_temp_to_reg(1000.0f));
    TEST_ASSERT_EQUAL_HEX16((uint16_t)(int16_t)-32767, modbus_temp_to_reg(-1000.0f));
    TEST_ASSERT_EQUAL_HEX16(MB_TEMP_INVALID, modbus_temp_to_reg(NAN));
}

void test_modbus_age(void)
{
    TEST_ASSERT_EQUAL_UINT16(MB_AGE_NEVER, modbus_age_to_reg(0, 5000));
    TEST_ASSERT_EQUAL_UINT16(0, modbus_age_to_reg(5000, 5000));
    TEST_ASSERT_EQUAL_UINT16(0, modbus_age_to_reg(6000, 5000));
    TEST_ASSERT_EQUAL_UINT16(12, modbus_age_to_reg(1000, 13999));
    TEST_ASSERT_EQUAL_UINT16(MB_AGE_NEVER - 1, modbus_age_to_reg(1, 1 + 100000LL * 1000));
}

void test_modbus_stale_limit(void)
{
    TEST_ASSERT_EQUAL_INT64(30000, modbus_stale_limit_ms(5000));
    TEST_ASSERT_EQUAL_INT64(30000, modbus_stale_limit_ms(10000));
    TEST_ASSERT_EQUAL_INT64(180000, modbus_stale_limit_ms(60000));
}

void test_modbus_parse_version(void)
{
    uint16_t v[3];
    modbus_parse_version("2.10.3", v);
    TEST_ASSERT_EQUAL_UINT16(2, v[0]);
    TEST_ASSERT_EQUAL_UINT16(10, v[1]);
    TEST_ASSERT_EQUAL_UINT16(3, v[2]);
    modbus_parse_version("v1.4.0-beta.2", v);
    TEST_ASSERT_EQUAL_UINT16(1, v[0]);
    TEST_ASSERT_EQUAL_UINT16(4, v[1]);
    TEST_ASSERT_EQUAL_UINT16(0, v[2]);
    modbus_parse_version("dev", v);
    TEST_ASSERT_EQUAL_UINT16(0, v[0]);
    modbus_parse_version(NULL, v);
    TEST_ASSERT_EQUAL_UINT16(0, v[2]);
}

/* ---- status ---- */

void test_modbus_status_precedence(void)
{
    const int64_t now = 100000;
    modbus_slot_t slot = {0};
    modbus_sensor_input_t s = good_sensor(1, 20.0f, now);

    TEST_ASSERT_EQUAL(MB_STATUS_UNASSIGNED, modbus_slot_status(&slot, &s, MB_CYCLE_OK, now, 10000));

    slot.assigned = true;
    memcpy(slot.rom, s.rom, MODBUS_ROM_LEN);
    TEST_ASSERT_EQUAL(MB_STATUS_MISSING, modbus_slot_status(&slot, NULL, MB_CYCLE_OK, now, 10000));
    TEST_ASSERT_EQUAL(MB_STATUS_OK, modbus_slot_status(&slot, &s, MB_CYCLE_OK, now, 10000));

    /* Whole-cycle failure beats a sensor's own good reading */
    TEST_ASSERT_EQUAL(MB_STATUS_READ_ERROR, modbus_slot_status(&slot, &s, MB_CYCLE_FAILED, now, 10000));

    /* Partial cycle: only the failed sensor is READ_ERROR */
    TEST_ASSERT_EQUAL(MB_STATUS_OK, modbus_slot_status(&slot, &s, MB_CYCLE_PARTIAL, now, 10000));
    s.valid = false;
    TEST_ASSERT_EQUAL(MB_STATUS_READ_ERROR, modbus_slot_status(&slot, &s, MB_CYCLE_PARTIAL, now, 10000));
}

void test_modbus_status_never_read_is_stale(void)
{
    const int64_t now = 100000;
    modbus_slot_t slot = {0};
    modbus_sensor_input_t s;
    memset(&s, 0, sizeof(s));
    make_rom(s.rom, 3);
    slot.assigned = true;
    memcpy(slot.rom, s.rom, MODBUS_ROM_LEN);
    /* Just discovered, no read attempt yet */
    TEST_ASSERT_EQUAL(MB_STATUS_STALE, modbus_slot_status(&slot, &s, MB_CYCLE_OK, now, 10000));
}

void test_modbus_status_stale_after_limit(void)
{
    const int64_t now = 1000000;
    modbus_slot_t slot = {0};
    modbus_sensor_input_t s = good_sensor(1, 20.0f, now);
    slot.assigned = true;
    memcpy(slot.rom, s.rom, MODBUS_ROM_LEN);

    s.last_read_ms = now - 30000;
    TEST_ASSERT_EQUAL(MB_STATUS_OK, modbus_slot_status(&slot, &s, MB_CYCLE_OK, now, 10000));
    s.last_read_ms = now - 30001;
    TEST_ASSERT_EQUAL(MB_STATUS_STALE, modbus_slot_status(&slot, &s, MB_CYCLE_OK, now, 10000));
    /* A 60 s interval allows 180 s */
    s.last_read_ms = now - 170000;
    TEST_ASSERT_EQUAL(MB_STATUS_OK, modbus_slot_status(&slot, &s, MB_CYCLE_OK, now, 60000));
}

/* ---- register image ---- */

void test_modbus_build_regs_info_block(void)
{
    modbus_slot_table_t table;
    memset(&table, 0, sizeof(table));
    modbus_sensor_input_t sensors[2] = {good_sensor(1, 20.0f, 50000), good_sensor(2, 21.0f, 50000)};
    int assigned = 0;
    uint8_t roms[2][MODBUS_ROM_LEN];
    memcpy(roms[0], sensors[0].rom, MODBUS_ROM_LEN);
    memcpy(roms[1], sensors[1].rom, MODBUS_ROM_LEN);
    modbus_slots_auto_assign(&table, (const uint8_t (*)[MODBUS_ROM_LEN])roms, 2, &assigned);

    modbus_info_input_t info = {
        .fw_version = {2, 11, 4},
        .uptime_s = 0x00012345,
        .mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
        .max_sensors = 20,
        .cycle_count = 0x10005,
        .last_result = MB_CYCLE_PARTIAL,
        .read_interval_ms = 10000,
        .now_ms = 50000,
    };
    modbus_regs_t regs;
    modbus_build_regs(&regs, &table, sensors, 2, &info);

    TEST_ASSERT_EQUAL_UINT16(MODBUS_MAP_VERSION, regs.info[MB_INFO_MAP_VERSION]);
    TEST_ASSERT_EQUAL_UINT16(2, regs.info[MB_INFO_FW_MAJOR]);
    TEST_ASSERT_EQUAL_UINT16(11, regs.info[MB_INFO_FW_MINOR]);
    TEST_ASSERT_EQUAL_UINT16(4, regs.info[MB_INFO_FW_PATCH]);
    TEST_ASSERT_EQUAL_UINT16(100, regs.info[MB_INFO_SLOT_CAPACITY]);
    TEST_ASSERT_EQUAL_UINT16(5, regs.info[MB_INFO_CYCLE_COUNT]);
    TEST_ASSERT_EQUAL_HEX16(0x0001, regs.info[MB_INFO_UPTIME_HI]);
    TEST_ASSERT_EQUAL_HEX16(0x2345, regs.info[MB_INFO_UPTIME_LO]);
    TEST_ASSERT_EQUAL_HEX16(0xAABB, regs.info[MB_INFO_MAC_0]);
    TEST_ASSERT_EQUAL_HEX16(0xCCDD, regs.info[MB_INFO_MAC_0 + 1]);
    TEST_ASSERT_EQUAL_HEX16(0xEEFF, regs.info[MB_INFO_MAC_0 + 2]);
    TEST_ASSERT_EQUAL_UINT16(20, regs.info[MB_INFO_MAX_SENSORS]);
    TEST_ASSERT_EQUAL_UINT16(2, regs.info[MB_INFO_SLOTS_ASSIGNED]);
    TEST_ASSERT_EQUAL_UINT16(2, regs.info[MB_INFO_SENSORS_PRESENT]);
    TEST_ASSERT_EQUAL_UINT16(MB_CYCLE_PARTIAL, regs.info[MB_INFO_LAST_RESULT]);
    TEST_ASSERT_EQUAL_UINT16(10, regs.info[MB_INFO_READ_INTERVAL_S]);
}

void test_modbus_build_regs_slots(void)
{
    const int64_t now = 50000;
    modbus_slot_table_t table;
    memset(&table, 0, sizeof(table));

    modbus_sensor_input_t present = good_sensor(1, 21.5f, now);
    modbus_sensor_input_t failed = good_sensor(2, 99.0f, now);
    failed.valid = false;

    table.slots[0].assigned = true;
    memcpy(table.slots[0].rom, present.rom, MODBUS_ROM_LEN);
    table.slots[1].assigned = true;
    memcpy(table.slots[1].rom, failed.rom, MODBUS_ROM_LEN);
    table.slots[99].assigned = true;
    make_rom(table.slots[99].rom, 77); /* not on the bus */

    modbus_sensor_input_t sensors[2] = {failed, present}; /* bus order != slot order */
    modbus_info_input_t info = {.read_interval_ms = 10000, .now_ms = now, .last_result = MB_CYCLE_PARTIAL};
    modbus_regs_t regs;
    modbus_build_regs(&regs, &table, sensors, 2, &info);

    TEST_ASSERT_EQUAL_UINT16(MB_STATUS_OK, regs.status[0]);
    TEST_ASSERT_EQUAL_HEX16(2150, regs.temp[0]);
    TEST_ASSERT_EQUAL_UINT16(1, regs.age[0]);
    TEST_ASSERT_EQUAL_HEX16(0x2801, regs.rom[0]);
    TEST_ASSERT_EQUAL_HEX16(0x005B, regs.rom[3]);

    TEST_ASSERT_EQUAL_UINT16(MB_STATUS_READ_ERROR, regs.status[1]);
    TEST_ASSERT_EQUAL_HEX16(MB_TEMP_INVALID, regs.temp[1]);

    TEST_ASSERT_EQUAL_UINT16(MB_STATUS_UNASSIGNED, regs.status[50]);
    TEST_ASSERT_EQUAL_HEX16(MB_TEMP_INVALID, regs.temp[50]);
    TEST_ASSERT_EQUAL_UINT16(MB_AGE_NEVER, regs.age[50]);
    TEST_ASSERT_EQUAL_HEX16(0, regs.rom[50 * MB_REGS_PER_ROM]);

    TEST_ASSERT_EQUAL_UINT16(MB_STATUS_MISSING, regs.status[99]);
    TEST_ASSERT_EQUAL_HEX16(MB_TEMP_INVALID, regs.temp[99]);
    TEST_ASSERT_EQUAL_UINT16(MB_AGE_NEVER, regs.age[99]);
    TEST_ASSERT_EQUAL_HEX16(0x284D, regs.rom[99 * MB_REGS_PER_ROM]);
}

void test_modbus_build_regs_bus_failure_invalidates_all(void)
{
    const int64_t now = 50000;
    modbus_slot_table_t table;
    memset(&table, 0, sizeof(table));
    modbus_sensor_input_t s = good_sensor(1, 20.0f, now);
    table.slots[0].assigned = true;
    memcpy(table.slots[0].rom, s.rom, MODBUS_ROM_LEN);
    modbus_info_input_t info = {.read_interval_ms = 10000, .now_ms = now, .last_result = MB_CYCLE_FAILED};
    modbus_regs_t regs;
    modbus_build_regs(&regs, &table, &s, 1, &info);
    TEST_ASSERT_EQUAL_UINT16(MB_STATUS_READ_ERROR, regs.status[0]);
    TEST_ASSERT_EQUAL_HEX16(MB_TEMP_INVALID, regs.temp[0]);
    /* The age still reports the last good reading */
    TEST_ASSERT_EQUAL_UINT16(1, regs.age[0]);
}

/* ---- slot table ---- */

void test_modbus_auto_assign_is_order_independent(void)
{
    uint8_t a[3][MODBUS_ROM_LEN], b[3][MODBUS_ROM_LEN];
    make_rom(a[0], 9); make_rom(a[1], 2); make_rom(a[2], 5);
    make_rom(b[0], 5); make_rom(b[1], 9); make_rom(b[2], 2);

    modbus_slot_table_t ta, tb;
    memset(&ta, 0, sizeof(ta));
    memset(&tb, 0, sizeof(tb));
    int na = 0, nb = 0;
    TEST_ASSERT_EQUAL_INT(0, modbus_slots_auto_assign(&ta, (const uint8_t (*)[MODBUS_ROM_LEN])a, 3, &na));
    TEST_ASSERT_EQUAL_INT(0, modbus_slots_auto_assign(&tb, (const uint8_t (*)[MODBUS_ROM_LEN])b, 3, &nb));
    TEST_ASSERT_EQUAL_INT(3, na);
    TEST_ASSERT_EQUAL_MEMORY(&ta, &tb, sizeof(ta));
    TEST_ASSERT_EQUAL_UINT8(2, ta.slots[0].rom[1]);
    TEST_ASSERT_EQUAL_UINT8(5, ta.slots[1].rom[1]);
    TEST_ASSERT_EQUAL_UINT8(9, ta.slots[2].rom[1]);
}

void test_modbus_auto_assign_keeps_existing_and_fills_gaps(void)
{
    modbus_slot_table_t t;
    memset(&t, 0, sizeof(t));
    t.slots[0].assigned = true;
    make_rom(t.slots[0].rom, 50);
    t.slots[2].assigned = true;
    make_rom(t.slots[2].rom, 60);

    uint8_t roms[4][MODBUS_ROM_LEN];
    make_rom(roms[0], 60); make_rom(roms[1], 7); make_rom(roms[2], 50); make_rom(roms[3], 7); /* dup */
    int n = 0;
    TEST_ASSERT_EQUAL_INT(0, modbus_slots_auto_assign(&t, (const uint8_t (*)[MODBUS_ROM_LEN])roms, 4, &n));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT8(50, t.slots[0].rom[1]);
    TEST_ASSERT_EQUAL_UINT8(7, t.slots[1].rom[1]);
    TEST_ASSERT_EQUAL_UINT8(60, t.slots[2].rom[1]);
    TEST_ASSERT_EQUAL_INT(3, modbus_slots_assigned_count(&t));
}

void test_modbus_auto_assign_full_table(void)
{
    modbus_slot_table_t t;
    memset(&t, 0, sizeof(t));
    for (int s = 0; s < MODBUS_SLOT_COUNT - 1; s++) {
        t.slots[s].assigned = true;
        make_rom(t.slots[s].rom, (uint8_t)s);
        t.slots[s].rom[2] = 0xEE;
    }
    uint8_t roms[3][MODBUS_ROM_LEN];
    make_rom(roms[0], 1); make_rom(roms[1], 2); make_rom(roms[2], 3);
    int n = 0;
    TEST_ASSERT_EQUAL_INT(2, modbus_slots_auto_assign(&t, (const uint8_t (*)[MODBUS_ROM_LEN])roms, 3, &n));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT8(1, t.slots[99].rom[1]);
}

void test_modbus_move_swaps_and_release_clears(void)
{
    modbus_slot_table_t t;
    memset(&t, 0, sizeof(t));
    t.slots[0].assigned = true;
    make_rom(t.slots[0].rom, 1);
    t.slots[5].assigned = true;
    make_rom(t.slots[5].rom, 2);

    TEST_ASSERT_TRUE(modbus_slots_move(&t, 0, 5));
    TEST_ASSERT_EQUAL_UINT8(2, t.slots[0].rom[1]);
    TEST_ASSERT_EQUAL_UINT8(1, t.slots[5].rom[1]);

    TEST_ASSERT_TRUE(modbus_slots_move(&t, 5, 42)); /* onto an empty slot */
    TEST_ASSERT_FALSE(t.slots[5].assigned);
    TEST_ASSERT_EQUAL_INT(42, modbus_slots_find(&t, (uint8_t[]){0x28, 1, 0, 0, 0, 0, 0, 1 ^ 0x5A}));

    TEST_ASSERT_FALSE(modbus_slots_move(&t, 0, MODBUS_SLOT_COUNT));
    TEST_ASSERT_FALSE(modbus_slots_move(&t, -1, 3));

    TEST_ASSERT_TRUE(modbus_slots_release(&t, 42));
    TEST_ASSERT_FALSE(t.slots[42].assigned);
    TEST_ASSERT_FALSE(modbus_slots_release(&t, 100));
}

void test_modbus_slot_blob_round_trip(void)
{
    modbus_slot_table_t t, back;
    memset(&t, 0, sizeof(t));
    t.slots[0].assigned = true;
    make_rom(t.slots[0].rom, 11);
    t.slots[99].assigned = true;
    make_rom(t.slots[99].rom, 12);

    uint8_t blob[MODBUS_SLOT_BLOB_SIZE];
    modbus_slots_serialize(&t, blob);
    TEST_ASSERT_TRUE(modbus_slots_deserialize(&back, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_MEMORY(&t, &back, sizeof(t));
}

void test_modbus_slot_blob_rejects_corruption(void)
{
    modbus_slot_table_t t, back;
    memset(&t, 0, sizeof(t));
    t.slots[3].assigned = true;
    make_rom(t.slots[3].rom, 1);
    uint8_t blob[MODBUS_SLOT_BLOB_SIZE];

    modbus_slots_serialize(&t, blob);
    blob[10] ^= 0x01;
    TEST_ASSERT_FALSE(modbus_slots_deserialize(&back, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_INT(0, modbus_slots_assigned_count(&back));

    modbus_slots_serialize(&t, blob);
    TEST_ASSERT_FALSE(modbus_slots_deserialize(&back, blob, sizeof(blob) - 1));

    /* Wrong version, CRC recomputed so only the version check can reject it */
    modbus_slots_serialize(&t, blob);
    blob[0] = 99;
    uint32_t crc = modbus_crc32(blob, MODBUS_SLOT_BLOB_SIZE - 4);
    memcpy(&blob[MODBUS_SLOT_BLOB_SIZE - 4], (uint8_t[]){crc & 0xFF, (crc >> 8) & 0xFF, (crc >> 16) & 0xFF, crc >> 24}, 4);
    TEST_ASSERT_FALSE(modbus_slots_deserialize(&back, blob, sizeof(blob)));
}

void test_modbus_slot_blob_rejects_duplicate_rom(void)
{
    modbus_slot_table_t t, back;
    memset(&t, 0, sizeof(t));
    t.slots[0].assigned = true;
    make_rom(t.slots[0].rom, 1);
    t.slots[1] = t.slots[0];
    uint8_t blob[MODBUS_SLOT_BLOB_SIZE];
    modbus_slots_serialize(&t, blob);
    TEST_ASSERT_FALSE(modbus_slots_deserialize(&back, blob, sizeof(blob)));
}

void test_modbus_crc32_known_value(void)
{
    const uint8_t msg[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, modbus_crc32(msg, 9));
}

void test_modbus_unit_id_accepted(void)
{
    TEST_ASSERT_TRUE(modbus_unit_id_accepted(1, 1));
    TEST_ASSERT_TRUE(modbus_unit_id_accepted(0, 1));
    TEST_ASSERT_TRUE(modbus_unit_id_accepted(255, 1));
    TEST_ASSERT_TRUE(modbus_unit_id_accepted(247, 247));
    TEST_ASSERT_FALSE(modbus_unit_id_accepted(2, 1));
    TEST_ASSERT_FALSE(modbus_unit_id_accepted(1, 247));
}

void run_modbus_map_tests(void)
{
    RUN_TEST(test_modbus_unit_id_accepted);
    RUN_TEST(test_modbus_temp_rounding_and_sign);
    RUN_TEST(test_modbus_temp_clamps_and_never_returns_invalid_marker);
    RUN_TEST(test_modbus_age);
    RUN_TEST(test_modbus_stale_limit);
    RUN_TEST(test_modbus_parse_version);
    RUN_TEST(test_modbus_status_precedence);
    RUN_TEST(test_modbus_status_never_read_is_stale);
    RUN_TEST(test_modbus_status_stale_after_limit);
    RUN_TEST(test_modbus_build_regs_info_block);
    RUN_TEST(test_modbus_build_regs_slots);
    RUN_TEST(test_modbus_build_regs_bus_failure_invalidates_all);
    RUN_TEST(test_modbus_auto_assign_is_order_independent);
    RUN_TEST(test_modbus_auto_assign_keeps_existing_and_fills_gaps);
    RUN_TEST(test_modbus_auto_assign_full_table);
    RUN_TEST(test_modbus_move_swaps_and_release_clears);
    RUN_TEST(test_modbus_slot_blob_round_trip);
    RUN_TEST(test_modbus_slot_blob_rejects_corruption);
    RUN_TEST(test_modbus_slot_blob_rejects_duplicate_rom);
    RUN_TEST(test_modbus_crc32_known_value);
}
