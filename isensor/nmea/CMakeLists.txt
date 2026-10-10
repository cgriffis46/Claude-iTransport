# nmea: what the GNSS drivers share. Header-only, pure logic.
#   inc/NmeaParser.h  NMEA 0183 parser (src/NmeaParser.tpp)
#   inc/ByteRing.h  interrupt -> thread byte ring
#
# Added by isensor/CMakeLists.txt, before the drivers that use it
# (ublox_gps, mtk3339).
add_library(nmea_parser INTERFACE)

target_include_directories(nmea_parser INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(nmea_parser INTERFACE cxx_std_17)

# --- Host test -----------------------------------------------------------
if(SENSOR_FW_BUILD_TESTS)
    add_executable(nmea_parser_test test/nmea_parser_test.cpp)
    target_link_libraries(nmea_parser_test PRIVATE nmea_parser)
    add_test(NAME nmea_parser_test COMMAND nmea_parser_test)
endif()
