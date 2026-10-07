# sht31: sensor driver, a class template over its transport.
#   inc/SHT31.h  the class, for any transport, run from your own loop
#   src/SHT31.tpp  its member definitions, included by inc/SHT31.h
#   inc/xSHT31.h  the same driver in its own CMSIS-RTOS2 task
#
# Added by isensor/CMakeLists.txt. Needs itransport's sensor_transport
# and isensor's sensor_state_machine.
add_library(sht31_sensor INTERFACE)

target_include_directories(sht31_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(sht31_sensor INTERFACE cxx_std_17)

target_link_libraries(sht31_sensor INTERFACE sensor_transport sensor_state_machine)

# xSHT31.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(sht31_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xSHT31.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(SHT31_test test/SHT31_test.cpp)
    target_include_directories(SHT31_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(SHT31_test PRIVATE sht31_sensor)
    add_test(NAME SHT31_test COMMAND SHT31_test)
endif()
