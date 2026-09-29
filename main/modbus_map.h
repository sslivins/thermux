/**
 * @file modbus_map.h
 * @brief Modbus TCP register map, slot status and slot table (host-testable)
 *
 * Pure logic with no ESP-IDF dependencies, compiled into both the firmware
 * and the host unit tests. The esp-modbus glue lives in modbus_server.c.
 *
 * Register map v1 (input registers, FC 0x04, zero-based):
 *   0..15       device info (see MB_INFO_*)
 *   100..199    temperature per slot, int16 in 0.01 degC, 0x8000 unless OK
 *   200..299    status per slot (modbus_slot_status_t)
 *   300..399    seconds since last successful read (65535 = never, or not connected)
 *   1000..1399  ROM ID, 4 registers per slot, big-endian byte pairs
 *
 * All MODBUS_SLOT_COUNT slots are always mapped, whatever CONFIG_MAX_SENSORS
 * is, so the address map never changes between builds.
 */

#ifndef MODBUS_MAP_H
#define MODBUS_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MODBUS_MAP_VERSION      1
#define MODBUS_SLOT_COUNT       100
#define MODBUS_ROM_LEN          8

#define MB_REG_INFO_START       0
#define MB_REG_INFO_COUNT       16
#define MB_REG_TEMP_START       100
#define MB_REG_STATUS_START     200
#define MB_REG_AGE_START        300
#define MB_REG_ROM_START        1000
#define MB_REGS_PER_ROM         (MODBUS_ROM_LEN / 2)
#define MB_REG_ROM_COUNT        (MODBUS_SLOT_COUNT * MB_REGS_PER_ROM)

/* Offsets inside the info block */
#define MB_INFO_MAP_VERSION     0
#define MB_INFO_FW_MAJOR        1
#define MB_INFO_FW_MINOR        2
#define MB_INFO_FW_PATCH        3
#define MB_INFO_SLOT_CAPACITY   4
#define MB_INFO_CYCLE_COUNT     5
#define MB_INFO_UPTIME_HI       6
#define MB_INFO_UPTIME_LO       7
#define MB_INFO_MAC_0           8   /* 8..10 */
#define MB_INFO_MAX_SENSORS     11
#define MB_INFO_SLOTS_ASSIGNED  12
#define MB_INFO_SENSORS_PRESENT 13
#define MB_INFO_LAST_RESULT     14
#define MB_INFO_READ_INTERVAL_S 15

#define MB_TEMP_INVALID         0x8000
#define MB_AGE_NEVER            0xFFFF

/** Minimum staleness limit, regardless of read interval */
#define MB_STALE_MIN_MS         30000
#define MB_STALE_INTERVALS      3

/** Last-cycle result codes (register 14); match sensor_cycle_result_t */
#define MB_CYCLE_OK             0
#define MB_CYCLE_PARTIAL        1
#define MB_CYCLE_FAILED         2
#define MB_CYCLE_NO_SENSORS     3

typedef enum {
    MB_STATUS_OK = 0,
    MB_STATUS_UNASSIGNED = 1,
    MB_STATUS_MISSING = 2,
    MB_STATUS_READ_ERROR = 3,
    MB_STATUS_STALE = 4,
} modbus_slot_status_t;

typedef struct {
    bool assigned;
    uint8_t rom[MODBUS_ROM_LEN];
} modbus_slot_t;

typedef struct {
    modbus_slot_t slots[MODBUS_SLOT_COUNT];
} modbus_slot_table_t;

/** One sensor currently present on the bus */
typedef struct {
    uint8_t rom[MODBUS_ROM_LEN];
    float temperature;
    bool valid;               /**< Latest read attempt succeeded */
    int64_t last_read_ms;     /**< Uptime of last successful read, 0 = never */
    int64_t last_attempt_ms;  /**< Uptime of last read attempt, 0 = never */
} modbus_sensor_input_t;

typedef struct {
    uint16_t fw_version[3];
    uint32_t uptime_s;
    uint8_t mac[6];
    uint16_t max_sensors;
    uint32_t cycle_count;
    uint16_t last_result;     /**< MB_CYCLE_* */
    uint32_t read_interval_ms;
    int64_t now_ms;
} modbus_info_input_t;

/** Full register image; each array is one esp-modbus area descriptor */
typedef struct {
    uint16_t info[MB_REG_INFO_COUNT];
    uint16_t temp[MODBUS_SLOT_COUNT];
    uint16_t status[MODBUS_SLOT_COUNT];
    uint16_t age[MODBUS_SLOT_COUNT];
    uint16_t rom[MB_REG_ROM_COUNT];
} modbus_regs_t;

/* ---- Register packing ---------------------------------------------------- */

/** Convert degC to int16 hundredths (rounded, clamped to +-327.67) as uint16 bits */
uint16_t modbus_temp_to_reg(float celsius);

/** Seconds since @p last_read_ms, capped at 65534; 65535 if never read */
uint16_t modbus_age_to_reg(int64_t last_read_ms, int64_t now_ms);

/** Staleness limit: max(3 x read interval, 30 s) */
int64_t modbus_stale_limit_ms(uint32_t read_interval_ms);

/**
 * @brief Status of one slot, checked in order:
 *        UNASSIGNED, MISSING, READ_ERROR, STALE, OK.
 * @param sensor The present sensor with this slot's ROM, or NULL if absent
 */
modbus_slot_status_t modbus_slot_status(const modbus_slot_t *slot,
                                        const modbus_sensor_input_t *sensor,
                                        uint16_t last_result,
                                        int64_t now_ms,
                                        uint32_t read_interval_ms);

/** Parse "1.2.3", "v1.2.3" or "1.2.3-beta.1" into major/minor/patch (missing parts = 0) */
void modbus_parse_version(const char *version, uint16_t out[3]);

/** Build the full register image */
void modbus_build_regs(modbus_regs_t *out,
                       const modbus_slot_table_t *table,
                       const modbus_sensor_input_t *sensors, int sensor_count,
                       const modbus_info_input_t *info);

/* ---- Settings ------------------------------------------------------------ */

/** Port 1-65535 other than @p reserved_port, unit ID 1-247 */
bool modbus_config_valid(uint32_t port, uint32_t unit_id, uint32_t reserved_port);

/** Whether a request addressed to @p request_uid is for this server.
 *  0 and 255 are the conventional "direct connection" unit IDs and are always
 *  accepted here, although esp-modbus 2.1.3 drops unit IDs above 247 at the
 *  socket layer, so 255 never reaches the handler in practice. */
bool modbus_unit_id_accepted(uint8_t request_uid, uint8_t configured_uid);

/* ---- Slot table ---------------------------------------------------------- */

/** Slot index holding @p rom, or -1 */
int modbus_slots_find(const modbus_slot_table_t *table, const uint8_t *rom);

/** Number of assigned slots */
int modbus_slots_assigned_count(const modbus_slot_table_t *table);

/**
 * @brief Give every present-but-unassigned ROM a slot
 *
 * The unassigned ROMs are sorted, then given the lowest free slots, so the
 * result doesn't depend on the order sensors were found on the bus.
 *
 * @param[out] assigned   Number of slots newly assigned (may be NULL)
 * @return Number of ROMs left without a slot because the table is full
 */
int modbus_slots_auto_assign(modbus_slot_table_t *table,
                             const uint8_t (*roms)[MODBUS_ROM_LEN], int rom_count,
                             int *assigned);

/** Move slot @p from to @p to, swapping with whatever is in @p to. False on bad index. */
bool modbus_slots_move(modbus_slot_table_t *table, int from, int to);

/** Clear a slot. False on bad index. */
bool modbus_slots_release(modbus_slot_table_t *table, int slot);

/** Result of a checked slot operation (move, release, place) */
typedef enum {
    MB_SLOT_OP_OK = 0,
    MB_SLOT_OP_BAD_INDEX,   /**< Slot number outside 0..MODBUS_SLOT_COUNT-1 */
    MB_SLOT_OP_EMPTY,       /**< No sensor assigned to the source slot */
    MB_SLOT_OP_PRESENT,     /**< Sensor is still on the bus, so it can't be released */
    MB_SLOT_OP_OCCUPIED,    /**< Target slot already holds a sensor */
    MB_SLOT_OP_DUPLICATE,   /**< ROM already assigned to another slot */
} modbus_slot_op_t;

/**
 * @brief Move the sensor in @p from to @p to
 *
 * If @p to holds a sensor, the two swap places. Moving a slot onto itself is
 * a no-op that succeeds.
 */
modbus_slot_op_t modbus_slots_move_sensor(modbus_slot_table_t *table, int from, int to);

/**
 * @brief Release a slot whose sensor is no longer on the bus
 *
 * A present sensor can't be released: the next read cycle would just give
 * it a slot again.
 *
 * @param present_roms ROMs found in the latest scan
 */
modbus_slot_op_t modbus_slots_release_missing(modbus_slot_table_t *table, int slot,
                                              const uint8_t (*present_roms)[MODBUS_ROM_LEN],
                                              int present_count);

/** Put @p rom into empty slot @p slot (used when restoring a backup) */
modbus_slot_op_t modbus_slots_place(modbus_slot_table_t *table, int slot, const uint8_t *rom);

/* ---- ROM ID text form ------------------------------------------------------ */

/** 16 uppercase hex characters, the same form as sensor addresses in the API.
 *  @p out must hold at least 17 bytes. */
void modbus_rom_to_hex(const uint8_t *rom, char *out);

/** Parse 16 hex characters (either case). False on wrong length or bad characters. */
bool modbus_rom_from_hex(const char *str, uint8_t *rom);

/* ---- Slot table persistence ---------------------------------------------- */

#define MODBUS_SLOT_BLOB_VERSION 1
/* u16 version + 100 x (u8 assigned + 8 rom) + u32 crc32 */
#define MODBUS_SLOT_BLOB_SIZE   (2 + MODBUS_SLOT_COUNT * (1 + MODBUS_ROM_LEN) + 4)

uint32_t modbus_crc32(const uint8_t *data, size_t len);

/** Serialize into @p buf (MODBUS_SLOT_BLOB_SIZE bytes) */
void modbus_slots_serialize(const modbus_slot_table_t *table, uint8_t *buf);

/** Parse a blob; false (and @p table cleared) on wrong size, version or CRC */
bool modbus_slots_deserialize(modbus_slot_table_t *table, const uint8_t *buf, size_t len);

#endif /* MODBUS_MAP_H */
