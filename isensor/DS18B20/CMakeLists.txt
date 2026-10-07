# DS18B20: sensor driver, a class template over its transport.
#   inc/DS18B20.h  the class, for any transport, run from your own loop
#   src/DS18B20.tpp  its member definitions, included by inc/DS18B20.h
#   inc/xDS18B20.h  the same driver in its own CMSIS-RTOS2 task
#
# Added by isensor/CMakeLists.txt. Needs itransport's sensor_transport
# and isensor's sensor_state_machine.
add_library(ds18b20_sensor INTERFACE)

target_include_directories(ds18b20_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(ds18b20_sensor INTERFACE cxx_std_17)

target_link_libraries(ds18b20_sensor INTERFACE sensor_transport sensor_state_machine)

# xDS18B20.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(ds18b20_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xDS18B20.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(DS18B20_test test/DS18B20_test.cpp)
    target_include_directories(DS18B20_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(DS18B20_test PRIVATE ds18b20_sensor)
    add_test(NAME DS18B20_test COMMAND DS18B20_test)
endif()
