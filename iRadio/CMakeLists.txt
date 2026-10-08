# iRadio: radio transceivers and the protocols spoken over them.
#
#   rfm69/inc/          RFM69Regs.h: the SX1231 / RFM69 registers used here
#   davis/              davis_protocol: the Davis ISS protocol (hop tables,
#                       CRC, packet decode), DavisSchedule (which channel
#                       when) and DavisWeather (a station's latest readings),
#                       pure logic
#                       davis_rfm69: the receiver, a class template over its
#                       transport (header only, inc/davis_rfm69.h)
#   hw/freertos/        xdavis_rfm69: the receiver in its own FreeRTOS task,
#                       with a packet queue, a log stream buffer and DIO0
#                       wake-up (header only)
#   test/               host tests and simulations (an RFM69, ISS stations)
#
# Needs itransport's sensor_transport and isensor's SensorStateMachine.
# Built on its own, this file adds itransport from ITRANSPORT_DIR and finds
# SensorStateMachine.h in ISENSOR_DIR/inc.
#
# Building and running the host tests on a PC:
#   cmake -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF \
#         -DSENSOR_FW_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(iradio LANGUAGES CXX)

    option(SENSOR_FW_BUILD_TESTS "Build the host tests (no hardware, HAL or RTOS needed)" OFF)
    if(SENSOR_FW_BUILD_TESTS)
        enable_testing()
    endif()
endif()

set(ITRANSPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iTransport/itransport
    CACHE PATH "The itransport folder to build against")
set(ISENSOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../isensor
    CACHE PATH "The isensor folder (for SensorStateMachine.h)")

if(NOT TARGET sensor_transport)
    add_subdirectory(${ITRANSPORT_DIR} itransport)
endif()

if(NOT TARGET sensor_state_machine)
    add_library(sensor_state_machine INTERFACE)
    target_include_directories(sensor_state_machine INTERFACE ${ISENSOR_DIR}/inc)
    target_compile_features(sensor_state_machine INTERFACE cxx_std_17)
endif()

# FreeRTOS headers for hw/freertos (FreeRTOS.h, task.h, queue.h,
# stream_buffer.h and the port's portmacro.h).
set(FREERTOS_INCLUDE_DIRS "" CACHE STRING "FreeRTOS include directories, for xdavis_rfm69.h")

set(IRADIO_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# --- davis_protocol ----------------------------------------------------------
add_library(davis_protocol STATIC
    rfm69/inc/RFM69Regs.h
    davis/inc/DavisProtocol.h
    davis/inc/DavisSchedule.h
    davis/inc/DavisWeather.h
    davis/src/DavisProtocol.cpp
    davis/src/DavisSchedule.cpp
    davis/src/DavisWeather.cpp
)
target_include_directories(davis_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/davis/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/rfm69/inc
)
target_compile_features(davis_protocol PUBLIC cxx_std_17)
set_target_properties(davis_protocol PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${IRADIO_LIB_DIR})

# --- davis_rfm69 (header only) -------------------------------------------------
add_library(davis_rfm69 INTERFACE)
target_link_libraries(davis_rfm69 INTERFACE davis_protocol sensor_transport sensor_state_machine)

# --- xdavis_rfm69 (header only) ------------------------------------------------
add_library(xdavis_rfm69 INTERFACE)
target_include_directories(xdavis_rfm69 INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc)
if(NOT FREERTOS_INCLUDE_DIRS STREQUAL "")
    target_include_directories(xdavis_rfm69 INTERFACE ${FREERTOS_INCLUDE_DIRS})
endif()
target_link_libraries(xdavis_rfm69 INTERFACE davis_rfm69)

# --- Host tests ------------------------------------------------------------------
if(SENSOR_FW_BUILD_TESTS)
    add_executable(davis_test test/davis_test.cpp)
    target_link_libraries(davis_test PRIVATE davis_protocol)
    add_test(NAME davis_test COMMAND davis_test)

    add_executable(davis_rfm69_test test/davis_rfm69_test.cpp)
    target_include_directories(davis_rfm69_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/sim)
    target_link_libraries(davis_rfm69_test PRIVATE davis_rfm69)
    add_test(NAME davis_rfm69_test COMMAND davis_rfm69_test)

    # test/stub is a single threaded simulation of the FreeRTOS calls
    # xdavis_rfm69.h makes.
    add_executable(xdavis_rfm69_test test/xdavis_rfm69_test.cpp)
    target_include_directories(xdavis_rfm69_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test/sim
        ${CMAKE_CURRENT_SOURCE_DIR}/test/stub
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
    )
    target_link_libraries(xdavis_rfm69_test PRIVATE davis_rfm69)
    add_test(NAME xdavis_rfm69_test COMMAND xdavis_rfm69_test)
endif()
