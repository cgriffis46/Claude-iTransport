# ublox_gps: a u-blox GNSS receiver on a UART. Its NMEA parser is in
# ../nmea, shared with mtk3339.
#   inc/UbxProtocol.h  UBX frames: ACK/NAK, CFG-MSG, CFG-RATE, CFG-VALSET
#   inc/ublox_gps.h  the driver, a class template over its iTransport,
#                    run from your own loop (src/ublox_gps.tpp)
#   inc/xublox_gps.h  the same driver in its own CMSIS-RTOS2 task
#
# Added by isensor/CMakeLists.txt. Needs itransport's sensor_transport
# and isensor's sensor_state_machine.
add_library(ublox_gps_sensor INTERFACE)

target_include_directories(ublox_gps_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(ublox_gps_sensor INTERFACE cxx_std_17)

target_link_libraries(ublox_gps_sensor INTERFACE sensor_transport sensor_state_machine nmea_parser)

# xublox_gps.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(ublox_gps_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# ublox_gps_test: the UBX framing, and the driver against a simulated
# receiver; test/stub/cmsis_os2.h is a small simulation of the RTOS
# calls xublox_gps.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(ublox_gps_test test/ublox_gps_test.cpp)
    target_include_directories(ublox_gps_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(ublox_gps_test PRIVATE ublox_gps_sensor)
    add_test(NAME ublox_gps_test COMMAND ublox_gps_test)
endif()
