# iNetTransport: network interfaces built on itransport.
#
#   inc/                     NetTypes.h, iNetDevice.h: the seam between a
#                            chip driver and the interface on it. No RTOS.
#   hw/freertos/{inc,src}    xNetInterface, xEthernet, xWifi, xClient:
#                            a driver thread, FreeRTOS queue, stream
#                            buffers and event groups
#   dhcp/ dns/ sntp/         DHCP, DNS and SNTP clients as pure logic, each
#                            with a host test
#   w5500/                   WIZnet W5500 driver (SPI), a class template
#                            over its transport, with a host test
#   espat/                   ESP-AT Wi-Fi module driver (UART), likewise
#   examples/                a W5500 echo server for the STM32L432KC
#   test/                    the FreeRTOS layer's threaded host test,
#                            the FreeRTOS simulation it runs on (stub/),
#                            and the simulated W5500 (sim/)
#
# Needs itransport's sensor_transport target. Built on its own, this
# file adds itransport from ITRANSPORT_DIR itself; a project that
# includes this folder with add_subdirectory() adds itransport first.
#
# Building and running the host tests on a PC (no FreeRTOS needed):
#   cmake -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF \
#         -DSENSOR_FW_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build
#
# For a target build, point FREERTOS_INCLUDE_DIRS at FreeRTOS's
# include/ folder, the port's folder (portable/GCC/ARM_CM4F for the
# STM32L4) and the folder holding FreeRTOSConfig.h (Core/Inc), and
# xnet is built too. The other options are itransport's.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(iNetTransport LANGUAGES CXX)

    option(SENSOR_FW_BUILD_TESTS "Build the host tests (no hardware, HAL or RTOS needed)" OFF)
    if(SENSOR_FW_BUILD_TESTS)
        enable_testing()
    endif()
endif()

set(ITRANSPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iTransport/itransport
    CACHE PATH "The itransport folder to build against")

if(NOT TARGET sensor_transport)
    add_subdirectory(${ITRANSPORT_DIR} itransport)
endif()

# Static libraries are written to iNetTransport/lib rather than the build tree.
set(INET_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# --- inet_core -----------------------------------------------------------
# Header-only: the value types and the device interface every chip
# driver implements and every interface uses.
add_library(inet_core INTERFACE)
target_include_directories(inet_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(inet_core INTERFACE cxx_std_17)

# --- xnet ----------------------------------------------------------------
# xEthernet/xWifi/xClient call FreeRTOS directly (queues, stream
# buffers, event groups), so they need its headers. Built only when
# FREERTOS_INCLUDE_DIRS is set; the host test compiles the same sources
# against test/stub instead.
set(FREERTOS_INCLUDE_DIRS "" CACHE STRING
    "FreeRTOS include/, the port folder, and the folder holding FreeRTOSConfig.h (;-separated)")
set(XNET_SOURCES
    hw/freertos/inc/xNetInterface.h
    hw/freertos/src/xNetInterface.cpp
    hw/freertos/inc/xEthernet.h
    hw/freertos/inc/xWifi.h
    hw/freertos/src/xWifi.cpp
    hw/freertos/inc/xClient.h
    hw/freertos/src/xClient.cpp
)
if(NOT FREERTOS_INCLUDE_DIRS STREQUAL "")
    add_library(xnet STATIC ${XNET_SOURCES})
    target_include_directories(xnet PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
        ${FREERTOS_INCLUDE_DIRS}
    )
    target_compile_features(xnet PUBLIC cxx_std_17)
    set_target_properties(xnet PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})
    target_link_libraries(xnet PUBLIC inet_core)
endif()

# --- DHCP, DNS, SNTP and the chip drivers ----------------------------------
add_subdirectory(dhcp)
add_subdirectory(dns)
add_subdirectory(sntp)
add_subdirectory(w5500)
add_subdirectory(espat)

# --- Host test -------------------------------------------------------------
# The real xnet sources on real threads, against test/stub (FreeRTOS
# simulated with std::thread) and the simulated W5500 in test/sim.
if(SENSOR_FW_BUILD_TESTS)
    find_package(Threads REQUIRED)
    list(TRANSFORM XNET_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
    add_executable(xNet_test test/xNet_test.cpp ${XNET_SOURCES})
    target_include_directories(xNet_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test/stub
        ${CMAKE_CURRENT_SOURCE_DIR}/test/sim
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
    )
    target_link_libraries(xNet_test PRIVATE w5500_driver espat_driver Threads::Threads)
    add_test(NAME xNet_test COMMAND xNet_test)
endif()
