set(BACNET_ROOT "${CMAKE_CURRENT_LIST_DIR}/bacnet-stack")
set(BACNET_SRC "${BACNET_ROOT}/src")

idf_component_register(
    SRCS
        "port/bip_espidf.c"
        "port/mstimer_espidf.c"
        "bacnet-stack/src/bacnet/basic/binding/address.c"
        "bacnet-stack/src/bacnet/basic/datetime/datetime_mstimer.c"
        "bacnet-stack/src/bacnet/basic/npdu/h_npdu.c"
        "bacnet-stack/src/bacnet/basic/object/ai.c"
        "bacnet-stack/src/bacnet/basic/server/bacnet_device.c"
        "bacnet-stack/src/bacnet/basic/service/h_apdu.c"
        "bacnet-stack/src/bacnet/basic/service/h_cov.c"
        "bacnet-stack/src/bacnet/basic/service/h_iam.c"
        "bacnet-stack/src/bacnet/basic/service/h_noserv.c"
        "bacnet-stack/src/bacnet/basic/service/h_rd.c"
        "bacnet-stack/src/bacnet/basic/service/h_rp.c"
        "bacnet-stack/src/bacnet/basic/service/h_rpm.c"
        "bacnet-stack/src/bacnet/basic/service/h_whohas.c"
        "bacnet-stack/src/bacnet/basic/service/h_whois.c"
        "bacnet-stack/src/bacnet/basic/service/s_iam.c"
        "bacnet-stack/src/bacnet/basic/service/s_ihave.c"
        "bacnet-stack/src/bacnet/basic/sys/days.c"
        "bacnet-stack/src/bacnet/basic/sys/debug.c"
        "bacnet-stack/src/bacnet/basic/sys/dst.c"
        "bacnet-stack/src/bacnet/basic/sys/fifo.c"
        "bacnet-stack/src/bacnet/basic/sys/keylist.c"
        "bacnet-stack/src/bacnet/basic/sys/ringbuf.c"
        "bacnet-stack/src/bacnet/basic/sys/mstimer.c"
        "bacnet-stack/src/bacnet/basic/tsm/tsm.c"
        "bacnet-stack/src/bacnet/abort.c"
        "bacnet-stack/src/bacnet/bacaddr.c"
        "bacnet-stack/src/bacnet/bacapp.c"
        "bacnet-stack/src/bacnet/bacdcode.c"
        "bacnet-stack/src/bacnet/bacdest.c"
        "bacnet-stack/src/bacnet/bacdevobjpropref.c"
        "bacnet-stack/src/bacnet/bacerror.c"
        "bacnet-stack/src/bacnet/bacint.c"
        "bacnet-stack/src/bacnet/bacreal.c"
        "bacnet-stack/src/bacnet/bacstr.c"
        "bacnet-stack/src/bacnet/cov.c"
        "bacnet-stack/src/bacnet/datalink/bvlc.c"
        "bacnet-stack/src/bacnet/datetime.c"
        "bacnet-stack/src/bacnet/dcc.c"
        "bacnet-stack/src/bacnet/hostnport.c"
        "bacnet-stack/src/bacnet/iam.c"
        "bacnet-stack/src/bacnet/ihave.c"
        "bacnet-stack/src/bacnet/indtext.c"
        "bacnet-stack/src/bacnet/memcopy.c"
        "bacnet-stack/src/bacnet/npdu.c"
        "bacnet-stack/src/bacnet/proplist.c"
        "bacnet-stack/src/bacnet/rd.c"
        "bacnet-stack/src/bacnet/reject.c"
        "bacnet-stack/src/bacnet/rp.c"
        "bacnet-stack/src/bacnet/rpm.c"
        "bacnet-stack/src/bacnet/timestamp.c"
        "bacnet-stack/src/bacnet/whohas.c"
        "bacnet-stack/src/bacnet/whois.c"
        "bacnet-stack/src/bacnet/wp.c"
    INCLUDE_DIRS
        "bacnet-stack/src"
    REQUIRES
        esp_netif
        esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    BACDL_BIP=1
    BACNET_PROTOCOL_REVISION=16
    BACNET_STACK_DEPRECATED_DISABLE=1
    BACAPP_MINIMAL=1
    BACAPP_TIMESTAMP=1
    MAX_APDU=1476
    MAX_TSM_TRANSACTIONS=1
    MAX_ADDRESS_CACHE=8
    # Every BACNET_APPLICATION_DATA_VALUE (COV, RP, RPM handlers keep them on the
    # stack) reserves room for the longest string; the default is a whole APDU
    # (~1.5 KB each). Thermux strings are <= 63 bytes.
    MAX_CHARACTER_STRING_BYTES=128
    MAX_OCTET_STRING_BYTES=32
    PRINT_ENABLED=0
    CONFIG_BACNET_BASIC_OBJECT_ANALOG_INPUT=1
    CONFIG_BACNET_BASIC_COV_SUBSCRIPTIONS_SIZE=16
    BACNET_VENDOR_NAME="Thermux"
    BACNET_VENDOR_ID=260
    BACNET_DEVICE_MODEL_NAME="Thermux"
    BACNET_DEVICE_DESCRIPTION="Thermux temperature hub"
    BACNET_DEVICE_VERSION="3.5.0"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-format
    -Wno-missing-field-initializers
    -Wno-unused-function
    -Wno-unused-variable
)
