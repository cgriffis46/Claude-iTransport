# htu21df: sensor driver, a class template over its transport.
#   inc/HTU21DF.h  the class, for any transport, run from your own loop
#   src/HTU21DF.tpp  its member definitions, included by inc/HTU21DF.h
#   inc/xHTU21DF.h  the same driver in its own CMSIS-RTOS2 task
#
# Needs itransport's sensor_transport and isensor's
# sensor_state_machine, so add_subdirectory() those first.
add_library(htu21df_sensor INTERFACE)

target_include_directories(htu21df_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(htu21df_sensor INTERFACE cxx_std_17)

target_link_libraries(htu21df_sensor INTERFACE sensor_transport sensor_state_machine)

# xHTU21DF.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(htu21df_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xHTU21DF.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(HTU21DF_test test/HTU21DF_test.cpp)
    target_include_directories(HTU21DF_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(HTU21DF_test PRIVATE htu21df_sensor)
    add_test(NAME HTU21DF_test COMMAND HTU21DF_test)
endif()
