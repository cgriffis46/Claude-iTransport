# dhcp: a DHCP client as pure logic (packets in, packets out, lease
# timers), for any chip driver that can move a UDP datagram. The W5500
# driver uses it; a module that does its own DHCP (ESP-AT) doesn't.
#
# Added by iNetTransport/CMakeLists.txt. Needs only inet_core.
add_library(inet_dhcp STATIC
    inc/DhcpClient.h
    src/DhcpClient.cpp
)

target_include_directories(inet_dhcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(inet_dhcp PUBLIC cxx_std_17)
set_target_properties(inet_dhcp PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})
target_link_libraries(inet_dhcp PUBLIC inet_core)

# --- Host test -----------------------------------------------------------
# Against the simulated server in iNetTransport/test/sim, on a simulated clock.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(DhcpClient_test test/DhcpClient_test.cpp)
    target_include_directories(DhcpClient_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
    target_link_libraries(DhcpClient_test PRIVATE inet_dhcp)
    add_test(NAME DhcpClient_test COMMAND DhcpClient_test)
endif()
