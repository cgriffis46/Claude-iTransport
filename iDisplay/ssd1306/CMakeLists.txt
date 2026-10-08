# ssd1306: OLED driver, a class template over its transport.
#   inc/ssd1306.h   the class (I2C, and ssd1306_spi for 4-wire SPI), run from your own loop
#   src/ssd1306.tpp its member definitions, included by inc/ssd1306.h
#   inc/xssd1306.h  the same, sleeping with osDelay() while a transfer is in flight
#
# Added by iDisplay/CMakeLists.txt. Needs itransport's sensor_transport,
# sensor_state_machine and display_core.
add_library(ssd1306_display INTERFACE)

target_include_directories(ssd1306_display INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(ssd1306_display INTERFACE cxx_std_17)

target_link_libraries(ssd1306_display INTERFACE sensor_transport sensor_state_machine display_core)

# xssd1306.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(ssd1306_display INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xssd1306.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(ssd1306_test test/ssd1306_test.cpp)
    target_include_directories(ssd1306_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(ssd1306_test PRIVATE ssd1306_display)
    add_test(NAME ssd1306_test COMMAND ssd1306_test)
endif()
