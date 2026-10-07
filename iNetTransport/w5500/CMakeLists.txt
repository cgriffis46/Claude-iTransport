# w5500: WIZnet W5500 Ethernet driver, a class template over its transport.
#   inc/W5500.h      the class, for any iBlockTransport, driven by poll()
#   inc/W5500Regs.h  register map and SPI frame format
#   src/W5500.tpp    its member definitions, included by inc/W5500.h
#
# Added by iNetTransport/CMakeLists.txt. Needs itransport's sensor_transport
# (iBlockTransport), inet_core (iNetDevice) and inet_dhcp.
add_library(w5500_driver INTERFACE)

target_include_directories(w5500_driver INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(w5500_driver INTERFACE cxx_std_17)

target_link_libraries(w5500_driver INTERFACE sensor_transport inet_core inet_dhcp)

# --- Host test -----------------------------------------------------------
# The state machine alone, on a simulated clock, against the simulated
# chip in iNetTransport/test/sim.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(W5500_test test/W5500_test.cpp)
    target_include_directories(W5500_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
    target_link_libraries(W5500_test PRIVATE w5500_driver)
    add_test(NAME W5500_test COMMAND W5500_test)
endif()
