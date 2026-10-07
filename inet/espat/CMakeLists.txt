# espat: an ESP-AT Wi-Fi module (ESP32, ESP32-C3, ESP8266) over a UART,
# a class template over its transport.
#   inc/EspAt.h    the class, for any iTransport, driven by poll()
#   src/EspAt.tpp  its member definitions, included by inc/EspAt.h
#
# Added by inet/CMakeLists.txt. Needs itransport's sensor_transport
# (iTransport) and inet_core (iWifiDevice). The module does its own DHCP.
add_library(espat_driver INTERFACE)

target_include_directories(espat_driver INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(espat_driver INTERFACE cxx_std_17)

target_link_libraries(espat_driver INTERFACE sensor_transport inet_core)

# --- Host test -----------------------------------------------------------
# The state machine alone, on a simulated clock, against the simulated
# module in inet/test/sim.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(EspAt_test test/EspAt_test.cpp)
    target_include_directories(EspAt_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
    target_link_libraries(EspAt_test PRIVATE espat_driver)
    add_test(NAME EspAt_test COMMAND EspAt_test)
endif()
