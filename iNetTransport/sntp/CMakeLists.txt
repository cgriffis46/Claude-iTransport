# sntp: an SNTP client as pure logic (request out, answer in, retry timer).
# The W5500 driver runs it on its service socket; a module with its own
# SNTP (ESP-AT) doesn't.
#
# Added by iNetTransport/CMakeLists.txt. Needs only inet_core.
add_library(inet_sntp STATIC
    inc/SntpClient.h
    src/SntpClient.cpp
)

target_include_directories(inet_sntp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(inet_sntp PUBLIC cxx_std_17)
set_target_properties(inet_sntp PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})
target_link_libraries(inet_sntp PUBLIC inet_core)

# --- Host test -----------------------------------------------------------
# Against the simulated server in iNetTransport/test/sim/SimNtpServer.h, on a
# simulated clock.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(SntpClient_test test/SntpClient_test.cpp)
    target_include_directories(SntpClient_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
    target_link_libraries(SntpClient_test PRIVATE inet_sntp)
    add_test(NAME SntpClient_test COMMAND SntpClient_test)
endif()
