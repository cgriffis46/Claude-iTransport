# PM25: header-only driver.
#   inc/PM25.h  the driver, for any transport, run from your own loop
#   inc/xPM25.h  the same driver in its own CMSIS-RTOS2 task
#
# Needs itransport's sensor_transport and isensor's
# sensor_state_machine, so add_subdirectory() those first.
add_library(pm25_sensor INTERFACE)

target_include_directories(pm25_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(pm25_sensor INTERFACE cxx_std_17)

target_link_libraries(pm25_sensor INTERFACE sensor_transport sensor_state_machine)

# xPM25.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(pm25_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xPM25.h makes.
if(SENSOR_FW_BUILD_TESTS)
    # Runs the driver on a bare iTransport, and on itransport's real
    # Stm32HalUartTransport over the simulated HAL UART in
    # itransport/test/stub_hal.
    set(STUB_HAL ${CMAKE_CURRENT_SOURCE_DIR}/../itransport/test/stub_hal)
    set(STM32_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../itransport/hw/stm32/src)
    add_executable(PM25_test
        test/PM25_test.cpp
        ${STUB_HAL}/fake_hal.cpp
        ${STM32_SRC}/Stm32HalUartTransport.cpp
        ${STM32_SRC}/Stm32UartItCallbacks.cpp
    )
    target_include_directories(PM25_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test/stub
        ${STUB_HAL}
        ${CMAKE_CURRENT_SOURCE_DIR}/../itransport/hw/stm32/inc
    )
    target_link_libraries(PM25_test PRIVATE pm25_sensor)
    add_test(NAME PM25_test COMMAND PM25_test)
endif()
