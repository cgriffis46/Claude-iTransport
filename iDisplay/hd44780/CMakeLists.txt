# hd44780: character LCD driver behind an I2C port expander (PCF8574 or
# MCP23008), a class template over its transport.
#   inc/hd44780.h   the class, run from your own loop
#   src/hd44780.tpp its member definitions, included by inc/hd44780.h
#   inc/xhd44780.h  the same, sleeping with osDelay() while it waits
#
# Added by iDisplay/CMakeLists.txt. Needs itransport's sensor_transport,
# sensor_state_machine and display_core.
add_library(hd44780_display INTERFACE)

target_include_directories(hd44780_display INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(hd44780_display INTERFACE cxx_std_17)

target_link_libraries(hd44780_display INTERFACE sensor_transport sensor_state_machine display_core)

# xhd44780.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(hd44780_display INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xhd44780.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(hd44780_test test/hd44780_test.cpp)
    target_include_directories(hd44780_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(hd44780_test PRIVATE hd44780_display)
    add_test(NAME hd44780_test COMMAND hd44780_test)
endif()
