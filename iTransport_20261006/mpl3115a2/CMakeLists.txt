# mpl3115a2: sensor driver, a class template over its transport.
#   inc/mpl3115a2.h  the class, for any transport, run from your own loop
#   src/mpl3115a2.tpp  its member definitions, included by inc/mpl3115a2.h
#   inc/xmpl3115a2.h  the same driver in its own CMSIS-RTOS2 task
#
# Needs itransport's sensor_transport and isensor's
# sensor_state_machine, so add_subdirectory() those first.
add_library(mpl3115a2_sensor INTERFACE)

target_include_directories(mpl3115a2_sensor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(mpl3115a2_sensor INTERFACE cxx_std_17)

target_link_libraries(mpl3115a2_sensor INTERFACE sensor_transport sensor_state_machine)

# xmpl3115a2.h includes cmsis_os2.h.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    target_include_directories(mpl3115a2_sensor INTERFACE ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- Host test -----------------------------------------------------------
# test/stub/cmsis_os2.h is a small simulation of the RTOS calls xmpl3115a2.h makes.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(mpl3115a2_test test/mpl3115a2_test.cpp)
    target_include_directories(mpl3115a2_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/stub)
    target_link_libraries(mpl3115a2_test PRIVATE mpl3115a2_sensor)
    add_test(NAME mpl3115a2_test COMMAND mpl3115a2_test)
endif()
