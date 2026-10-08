# dns: a DNS client as pure logic (query out, answer in, retry timer), for one
# A-record lookup at a time. The W5500 driver runs it on its service
# socket; a module with its own resolver (ESP-AT) doesn't.
#
# Added by iNetTransport/CMakeLists.txt. Needs only inet_core.
add_library(inet_dns STATIC
    inc/DnsClient.h
    src/DnsClient.cpp
)

target_include_directories(inet_dns PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(inet_dns PUBLIC cxx_std_17)
set_target_properties(inet_dns PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})
target_link_libraries(inet_dns PUBLIC inet_core)

# --- Host test -----------------------------------------------------------
# Against the simulated server in iNetTransport/test/sim/SimDnsServer.h, on a
# simulated clock.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(DnsClient_test test/DnsClient_test.cpp)
    target_include_directories(DnsClient_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
    target_link_libraries(DnsClient_test PRIVATE inet_dns)
    add_test(NAME DnsClient_test COMMAND DnsClient_test)
endif()
