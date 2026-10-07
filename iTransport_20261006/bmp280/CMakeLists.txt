# bmp280: sensor driver, a class template over its transport.
#   inc/bmp280.h  the class, for any transport, run from your own loop
#   src/bmp280.tpp  its member definitions, included by inc/bmp280.h
#   inc/xbmp280.h  the same driver in its own CMSIS-RTOS2 task
#
# Needs itransport's sensor_transport and isensor's
# sensor_state_machine, so add_subdirectory() those first.
add_library(bmp280_sensor INTERFACE)

target_include_directories(bmp280_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(bmp280_sensor INTERFACE cxx_std_17)

target_link_libraries(bmp280_sensor INTERFACE sensor_transport sensor_state_machine)

# xbmp280.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(bmp280_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xbmp280.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(bmp280_test test/bmp280_test.cpp)
    target_include_directories(bmp280_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(bmp280_test PRIVATE bmp280_sensor)
    add_test(NAME bmp280_test COMMAND bmp280_test)
endif()
