# aht20: header-only driver.
#   inc/AHT20.h  the driver, for any transport, run from your own loop
#   inc/xAHT20.h  the same driver in its own CMSIS-RTOS2 task
#
# Needs itransport's sensor_transport and isensor's
# sensor_state_machine, so add_subdirectory() those first.
add_library(aht20_sensor INTERFACE)

target_include_directories(aht20_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(aht20_sensor INTERFACE cxx_std_17)

target_link_libraries(aht20_sensor INTERFACE sensor_transport sensor_state_machine)

# xAHT20.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(aht20_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xAHT20.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(AHT20_test test/AHT20_test.cpp)
    target_include_directories(AHT20_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(AHT20_test PRIVATE aht20_sensor)
    add_test(NAME AHT20_test COMMAND AHT20_test)
endif()
