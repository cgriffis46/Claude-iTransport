# bme280: sensor driver, a class template over its transport.
#   inc/BME280.h  the class, for any transport, run from your own loop
#   src/BME280.tpp  its member definitions, included by inc/BME280.h
#   inc/xBME280.h  the same driver in its own CMSIS-RTOS2 task
#
# Added by isensor/CMakeLists.txt. Needs itransport's sensor_transport
# and isensor's sensor_state_machine.
add_library(bme280_sensor INTERFACE)

target_include_directories(bme280_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(bme280_sensor INTERFACE cxx_std_17)

target_link_libraries(bme280_sensor INTERFACE sensor_transport sensor_state_machine)

# xBME280.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(bme280_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xBME280.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(BME280_test test/BME280_test.cpp)
    target_include_directories(BME280_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(BME280_test PRIVATE bme280_sensor)
    add_test(NAME BME280_test COMMAND BME280_test)
endif()
