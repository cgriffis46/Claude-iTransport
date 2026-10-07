# lsm303dlhc: sensor drivers, class templates over their transport: one
# for the accelerometer and one for the magnetometer.
#   inc/lsm303dlhc.h  the classes, for any transport, run from your own loop
#   src/lsm303dlhc.tpp  their member definitions, included by inc/lsm303dlhc.h
#   inc/xlsm303dlhc.h  the same drivers, each in its own CMSIS-RTOS2 task
#
# Added by isensor/CMakeLists.txt. Needs itransport's sensor_transport
# and isensor's sensor_state_machine.
add_library(lsm303dlhc_sensor INTERFACE)

target_include_directories(lsm303dlhc_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(lsm303dlhc_sensor INTERFACE cxx_std_17)

target_link_libraries(lsm303dlhc_sensor INTERFACE sensor_transport sensor_state_machine)

# xlsm303dlhc.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(lsm303dlhc_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xlsm303dlhc.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(lsm303dlhc_test test/lsm303dlhc_test.cpp)
    target_include_directories(lsm303dlhc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(lsm303dlhc_test PRIVATE lsm303dlhc_sensor)
    add_test(NAME lsm303dlhc_test COMMAND lsm303dlhc_test)
endif()
