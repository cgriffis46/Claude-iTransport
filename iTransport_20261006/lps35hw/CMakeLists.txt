# lps35hw: sensor driver, a class template over its transport.
#   inc/lps35hw.h  the class, for any transport, run from your own loop
#   src/lps35hw.tpp  its member definitions, included by inc/lps35hw.h
#   inc/xlps35hw.h  the same driver in its own CMSIS-RTOS2 task
#
# Needs itransport's sensor_transport and isensor's
# sensor_state_machine, so add_subdirectory() those first.
add_library(lps35hw_sensor INTERFACE)

target_include_directories(lps35hw_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(lps35hw_sensor INTERFACE cxx_std_17)

target_link_libraries(lps35hw_sensor INTERFACE sensor_transport sensor_state_machine)

# xlps35hw.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(lps35hw_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xlps35hw.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(lps35hw_test test/lps35hw_test.cpp)
    target_include_directories(lps35hw_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(lps35hw_test PRIVATE lps35hw_sensor)
    add_test(NAME lps35hw_test COMMAND lps35hw_test)
endif()
