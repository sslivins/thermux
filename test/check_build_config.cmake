# Build-configuration invariant checks.
#
# These guard low-level firmware settings that are easy to regress silently
# (e.g. an sdkconfig regeneration by a newer ESP-IDF, or an errant menuconfig
# save). They are NOT logic tests -- they assert that critical safety and
# flash-footprint settings stay as intended for this remote, serial-inaccessible
# POE device. Run as a CTest case via `cmake -P`.
#
# If you INTENTIONALLY change one of these, update the matching assertion here
# in the same commit so the reviewer sees the safety/space tradeoff explicitly.

cmake_minimum_required(VERSION 3.16)
cmake_policy(SET CMP0007 NEW) # list commands treat empty elements literally

# This script lives in test/, so repo root is one level up.
set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(SDKCONFIG "${REPO_ROOT}/sdkconfig")
set(MAIN_CMAKE "${REPO_ROOT}/main/CMakeLists.txt")
set(PARTITIONS "${REPO_ROOT}/partitions.csv")

set(_failures 0)

function(fail msg)
    message(WARNING "FAIL: ${msg}")
    math(EXPR _f "${_failures} + 1")
    set(_failures ${_f} PARENT_SCOPE)
endfunction()

# ---- Read files -------------------------------------------------------------
foreach(_f "${SDKCONFIG}" "${MAIN_CMAKE}" "${PARTITIONS}")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "check_build_config: required file missing: ${_f}")
    endif()
endforeach()
file(READ "${SDKCONFIG}" SDK)
file(READ "${MAIN_CMAKE}" MAINCM)

macro(assert_contains haystack needle why)
    string(FIND "${haystack}" "${needle}" _idx)
    if(_idx EQUAL -1)
        fail("expected to find `${needle}` -- ${why}")
    endif()
endmacro()

macro(assert_absent haystack needle why)
    string(FIND "${haystack}" "${needle}" _idx)
    if(NOT _idx EQUAL -1)
        fail("did NOT expect `${needle}` -- ${why}")
    endif()
endmacro()

# ---- 1. OTA rollback safety net (2.9.0) -------------------------------------
# Without this, a bad OTA that fails to boot is a permanent brick on this
# serial-inaccessible device. This is the single most important invariant.
assert_contains("${SDK}" "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y"
    "OTA rollback safety net must stay enabled; a bad OTA would otherwise brick the remote device with no serial recovery")

# ---- 2. CA certificate bundle (2.9.1) ---------------------------------------
# OTA to GitHub is the only TLS consumer (MQTT is plaintext). The bundle must
# exist (else OTA TLS can't validate GitHub and the update path silently dies),
# must be CMN (FULL wastes ~50-140 KB of scarce flash), and must not be NONE
# (which would break the GitHub TLS handshake entirely).
assert_contains("${SDK}" "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y"
    "a CA bundle must be attached or OTA TLS to GitHub cannot validate certs")
assert_contains("${SDK}" "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN=y"
    "CA bundle should be CMN: covers GitHub's roots (USERTrust ECC + ISRG Root X1) while saving flash vs FULL")
assert_absent("${SDK}" "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y"
    "FULL bundle re-embeds ~200 CA roots and wastes scarce flash; use CMN")
assert_absent("${SDK}" "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE=y"
    "NONE would break the GitHub TLS handshake and kill the OTA update path")

# ---- 3. Dead certificate embed must not return (2.9.1) ----------------------
# OTA uses esp_crt_bundle_attach, never an embedded PEM. Re-embedding the old
# certs/github_root_ca.pem just wastes flash.
assert_absent("${MAINCM}" "github_root_ca.pem"
    "the embedded github_root_ca.pem is dead weight (OTA uses the mbedtls bundle); do not re-add it")

# ---- 4. Partition layout: OTA slots ----------------------------------------
# Guard against accidentally shrinking the OTA slots or making them unequal.
# The app already uses ~90% of a slot, so shrinking risks an unflashable image.
# NOTE: issue #48 will GROW these (dropping the dead `factory` partition); a
# minimum-size check survives that, an exact-size check would not.
set(_MIN_OTA_SIZE 1245184) # 0x130000, the current slot size
file(STRINGS "${PARTITIONS}" _part_lines)
set(_ota0_size "")
set(_ota1_size "")
foreach(_line IN LISTS _part_lines)
    if(_line MATCHES "^[ \t]*#")
        continue()
    endif()
    # columns: Name, Type, SubType, Offset, Size, Flags
    string(REPLACE "," ";" _cols "${_line}")
    list(LENGTH _cols _ncols)
    if(_ncols LESS 5)
        continue()
    endif()
    list(GET _cols 0 _name)
    list(GET _cols 4 _size)
    string(STRIP "${_name}" _name)
    string(STRIP "${_size}" _size)
    if(_name STREQUAL "ota_0")
        set(_ota0_size "${_size}")
    elseif(_name STREQUAL "ota_1")
        set(_ota1_size "${_size}")
    endif()
endforeach()

if(_ota0_size STREQUAL "" OR _ota1_size STREQUAL "")
    fail("could not find both ota_0 and ota_1 partitions in partitions.csv")
else()
    math(EXPR _ota0_dec "${_ota0_size}")
    math(EXPR _ota1_dec "${_ota1_size}")
    if(NOT _ota0_dec EQUAL _ota1_dec)
        fail("ota_0 (${_ota0_size}) and ota_1 (${_ota1_size}) must be equal size for A/B OTA")
    endif()
    if(_ota0_dec LESS _MIN_OTA_SIZE)
        fail("ota_0 size ${_ota0_size} is below the ${_MIN_OTA_SIZE}-byte minimum; the app may no longer fit")
    endif()
endif()

# ---- Result -----------------------------------------------------------------
if(_failures GREATER 0)
    message(FATAL_ERROR "check_build_config: ${_failures} invariant(s) failed (see FAIL lines above)")
endif()
message(STATUS "check_build_config: all firmware config invariants hold")
