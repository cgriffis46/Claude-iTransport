# mtk3339: a MediaTek MT3339 GNSS module on a UART (Adafruit Ultimate
# GPS, GlobalTop PA6H/PA1616S, CDTop). Its NMEA parser is in ../nmea,
# shared with ublox_gps.
#   inc/Pmtk.h  the PMTK commands it sends (PMTK314/220/300, PGCMD 33)
#   inc/mtk3339.h  the driver, a class template over its iTransport,
#                  run from your own loop (src/mtk3339.tpp)
#   inc/xmtk3339.h  the same driver in its own CMSIS-RTOS2 task
#
# Added by isensor/CMakeLists.txt. Needs itransport's sensor_transport,
# isensor's sensor_state_machine and ../nmea's nmea_parser.
add_library(mtk3339_sensor INTERFACE)

target_include_directories(mtk3339_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(mtk3339_sensor INTERFACE cxx_std_17)

target_link_libraries(mtk3339_sensor INTERFACE sensor_transport sensor_state_machine nmea_parser)

# xmtk3339.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(mtk3339_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# mtk3339_test: the PMTK builders, and the driver against a simulated
# module; test/stub/cmsis_os2.h is a small simulation of the RTOS calls
# xmtk3339.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(mtk3339_test test/mtk3339_test.cpp)
    target_include_directories(mtk3339_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(mtk3339_test PRIVATE mtk3339_sensor)
    add_test(NAME mtk3339_test COMMAND mtk3339_test)
endif()
