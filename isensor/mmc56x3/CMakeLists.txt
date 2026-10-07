# mmc56x3: sensor driver, a class template over its transport.
#   inc/mmc56x3.h  the class, for any transport, run from your own loop
#   src/mmc56x3.tpp  its member definitions, included by inc/mmc56x3.h
#   inc/xmmc56x3.h  the same driver in its own CMSIS-RTOS2 task
#
# Added by isensor/CMakeLists.txt. Needs itransport's sensor_transport
# and isensor's sensor_state_machine.
add_library(mmc56x3_sensor INTERFACE)

target_include_directories(mmc56x3_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(mmc56x3_sensor INTERFACE cxx_std_17)

target_link_libraries(mmc56x3_sensor INTERFACE sensor_transport sensor_state_machine)

# xmmc56x3.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(mmc56x3_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xmmc56x3.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(mmc56x3_test test/mmc56x3_test.cpp)
    target_include_directories(mmc56x3_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(mmc56x3_test PRIVATE mmc56x3_sensor)
    add_test(NAME mmc56x3_test COMMAND mmc56x3_test)
endif()
