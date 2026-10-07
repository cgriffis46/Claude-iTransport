# si7021: sensor driver, a class template over its transport.
#   inc/Si7021.h  the class, for any transport, run from your own loop
#   src/Si7021.tpp  its member definitions, included by inc/Si7021.h
#   inc/xSi7021.h  the same driver in its own CMSIS-RTOS2 task
#
# Needs itransport's sensor_transport and isensor's
# sensor_state_machine, so add_subdirectory() those first.
add_library(si7021_sensor INTERFACE)

target_include_directories(si7021_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(si7021_sensor INTERFACE cxx_std_17)

target_link_libraries(si7021_sensor INTERFACE sensor_transport sensor_state_machine)

# xSi7021.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(si7021_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xSi7021.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(Si7021_test test/Si7021_test.cpp)
    target_include_directories(Si7021_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(Si7021_test PRIVATE si7021_sensor)
    add_test(NAME Si7021_test COMMAND Si7021_test)
endif()
