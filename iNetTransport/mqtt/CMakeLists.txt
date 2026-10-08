# mqtt: an MQTT 3.1.1 client as pure logic (packets in, packets out,
# session timers), QoS 0 and 1. hw/freertos/xMqttClient runs it over an
# xClient TCP connection.
#
# Added by iNetTransport/CMakeLists.txt. Needs nothing else.
add_library(inet_mqtt STATIC
    inc/MqttClient.h
    src/MqttClient.cpp
)

target_include_directories(inet_mqtt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(inet_mqtt PUBLIC cxx_std_17)
set_target_properties(inet_mqtt PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})

# --- Host test -----------------------------------------------------------
# Against the simulated broker in iNetTransport/test/sim/SimMqttBroker.h,
# on a simulated clock.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(MqttClient_test test/MqttClient_test.cpp)
    target_include_directories(MqttClient_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
    target_link_libraries(MqttClient_test PRIVATE inet_mqtt)
    add_test(NAME MqttClient_test COMMAND MqttClient_test)
endif()
