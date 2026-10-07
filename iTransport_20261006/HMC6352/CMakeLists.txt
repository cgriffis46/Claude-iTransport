# HMC6352: sensor driver, a class template over its transport.
#   inc/hmc6352.h  the class, for any transport, run from your own loop
#   src/hmc6352.tpp  its member definitions, included by inc/hmc6352.h
#   inc/xhmc6352.h  the same driver in its own CMSIS-RTOS2 task
#
# Needs itransport's sensor_transport and isensor's
# sensor_state_machine, so add_subdirectory() those first.
add_library(hmc6352_sensor INTERFACE)

target_include_directories(hmc6352_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(hmc6352_sensor INTERFACE cxx_std_17)

target_link_libraries(hmc6352_sensor INTERFACE sensor_transport sensor_state_machine)

# xhmc6352.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(hmc6352_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xhmc6352.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(hmc6352_test test/hmc6352_test.cpp)
    target_include_directories(hmc6352_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(hmc6352_test PRIVATE hmc6352_sensor)
    add_test(NAME hmc6352_test COMMAND hmc6352_test)
endif()
