# PLCTransport: the PLC tag database (PlcTagRegistry and its tag
# descriptors and data types), the CIP tag message codec, and the CIP
# tag TCP server.
#
#   inc/, src/          platform-independent: the tag registry, the
#                       codec and CipFrame
#   hw/freertos/        CipTagTcpServer, on FreeRTOS and lwIP
#   web/                plc_web: the tags as JSON for iNetTransport's web
#                       server (PlcTagWebApi), and a built-in page
#
# Builds on its own, or inside another project with add_subdirectory().
# Sources use flat #include "Foo.h" -- target_include_directories()
# below is what puts the right inc/ folders on the include path.
#
# Options:
#   PLC_MUTEX_USE_CMSIS_RTOS2  OFF (default): PlcMutex is std::mutex.
#                              ON: a CMSIS-RTOS2 mutex, for toolchains
#                              with no std::mutex (STM32 — see PlcMutex.h).
#                              Needs CMSIS_RTOS_INCLUDE_DIR.
#   CMSIS_RTOS_INCLUDE_DIR     directory holding cmsis_os2.h
#   FREERTOS_INCLUDE_DIRS      FreeRTOS include folders: Source/include,
#                              the portable/ port folder, and the folder
#                              holding FreeRTOSConfig.h
#   LWIP_INCLUDE_DIRS          lwIP include folders: src/include, system,
#                              and the project's LWIP/App and LWIP/Target
#
#   INET_DIR                   iNetTransport, for plc_web (default ../iNetTransport;
#                              plc_web is skipped if it isn't there)
#   SENSOR_FW_BUILD_TESTS      the host tests: plc_web's, the second of
#                              them over real sockets with curl
#   MBEDTLS_DIR                an mbedTLS 3.6 source tree: adds
#                              PlcWebSecure_test (HTTPS, logins, writes)
#
# cip_tag_server is built when FREERTOS_INCLUDE_DIRS and
# LWIP_INCLUDE_DIRS are both set, and needs PLC_MUTEX_USE_CMSIS_RTOS2.
#
# Host build and tests:
#   cmake -B build -DSENSOR_FW_BUILD_TESTS=ON
#   cmake --build build && ctest --test-dir build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(PLCTransport LANGUAGES CXX)
    option(SENSOR_FW_BUILD_TESTS "Build the host tests" OFF)
    if(SENSOR_FW_BUILD_TESTS)
        enable_testing()
    endif()
endif()

option(PLC_MUTEX_USE_CMSIS_RTOS2
    "Make PlcMutex a CMSIS-RTOS2 mutex instead of std::mutex" OFF)
set(CMSIS_RTOS_INCLUDE_DIR "" CACHE PATH "Directory containing cmsis_os2.h")
set(FREERTOS_INCLUDE_DIRS "" CACHE STRING
    "FreeRTOS include folders: Source/include, the portable/ port folder, and FreeRTOSConfig.h's folder")
set(LWIP_INCLUDE_DIRS "" CACHE STRING
    "lwIP include folders: src/include, system, and the project's LWIP/App and LWIP/Target")

# --- plc_tags ------------------------------------------------------------
# PLC tag registry and the CIP tag message codec.
add_library(plc_tags STATIC
    inc/PlcDataType.h
    inc/PlcMutex.h
    inc/PlcTagDescriptor.h
    inc/PlcTagRegistry.h
    inc/CipFrame.h
    inc/CipTagMessageCodec.h
    src/CipTagMessageCodec.cpp
)

target_include_directories(plc_tags PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)

target_compile_features(plc_tags PUBLIC cxx_std_17)

# Set on plc_tags itself, so every file that sees PlcTagRegistry agrees
# on which mutex it holds.
if(PLC_MUTEX_USE_CMSIS_RTOS2)
    if(CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR
            "PLC_MUTEX_USE_CMSIS_RTOS2 is ON but CMSIS_RTOS_INCLUDE_DIR is not set. "
            "Point it at the directory containing cmsis_os2.h.")
    endif()
    target_compile_definitions(plc_tags PUBLIC PLC_MUTEX_USE_CMSIS_RTOS2)
    target_include_directories(plc_tags PUBLIC ${CMSIS_RTOS_INCLUDE_DIR})
endif()

# --- cip_tag_server ------------------------------------------------------
# The CIP tag TCP server, on lwIP and FreeRTOS.
if(NOT FREERTOS_INCLUDE_DIRS STREQUAL "" AND NOT LWIP_INCLUDE_DIRS STREQUAL "")
    if(NOT PLC_MUTEX_USE_CMSIS_RTOS2)
        message(FATAL_ERROR
            "cip_tag_server runs on FreeRTOS, whose toolchain has no std::mutex: "
            "turn on PLC_MUTEX_USE_CMSIS_RTOS2 (and set CMSIS_RTOS_INCLUDE_DIR).")
    endif()

    add_library(cip_tag_server STATIC
        hw/freertos/inc/CipTagTcpServer.h
        hw/freertos/src/CipTagTcpServer.cpp
    )

    target_include_directories(cip_tag_server PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
        ${FREERTOS_INCLUDE_DIRS}
        ${LWIP_INCLUDE_DIRS}
    )

    target_link_libraries(cip_tag_server PUBLIC plc_tags)
endif()

# --- plc_web -------------------------------------------------------------
# The tag database as JSON (PlcTagWebApi) and a built-in page, on
# iNetTransport's HTTP code (inet_http: pure logic, no RTOS).
set(INET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iNetTransport CACHE PATH "iNetTransport, for plc_web")
if(EXISTS ${INET_DIR}/http/CMakeLists.txt)
    if(NOT TARGET inet_http)
        set(INET_LIB_DIR ${CMAKE_CURRENT_BINARY_DIR}/lib)
        set(PLC_SAVED_TESTS ${SENSOR_FW_BUILD_TESTS})
        set(SENSOR_FW_BUILD_TESTS OFF)   # inet_http's own tests belong to iNetTransport
        add_subdirectory(${INET_DIR}/http ${CMAKE_CURRENT_BINARY_DIR}/inet_http)
        set(SENSOR_FW_BUILD_TESTS ${PLC_SAVED_TESTS})
    endif()

    add_library(plc_web STATIC
        web/inc/PlcTagWebApi.h
        web/inc/PlcWebDefaultPage.h
        web/src/PlcTagWebApi.cpp
        web/src/PlcWebDefaultPage.cpp
    )
    target_include_directories(plc_web PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/web/inc)
    target_link_libraries(plc_web PUBLIC plc_tags inet_http)

    if(SENSOR_FW_BUILD_TESTS)
        find_package(Threads REQUIRED)
        add_executable(PlcTagWebApi_test web/test/PlcTagWebApi_test.cpp)
        target_link_libraries(PlcTagWebApi_test PRIVATE plc_web Threads::Threads)
        add_test(NAME PlcTagWebApi_test COMMAND PlcTagWebApi_test)

        # End to end: xHttpServer over SocketNetDevice on the host's
        # sockets, curl, and Python's JSON parser. FreeRTOS is
        # iNetTransport's host simulation.
        if(UNIX)
            add_executable(PlcWebServer_test web/test/PlcWebServer_test.cpp
                ${INET_DIR}/sockets/src/SocketNetDevice.cpp
                ${INET_DIR}/hw/freertos/src/xNetInterface.cpp
                ${INET_DIR}/hw/freertos/src/xClient.cpp
                ${INET_DIR}/hw/freertos/src/xHttpServer.cpp
                ${INET_DIR}/dns/src/DnsClient.cpp
                ${INET_DIR}/sntp/src/SntpClient.cpp)
            target_include_directories(PlcWebServer_test PRIVATE
                ${INET_DIR}/test/stub ${INET_DIR}/inc ${INET_DIR}/hw/freertos/inc
                ${INET_DIR}/dns/inc ${INET_DIR}/sntp/inc ${INET_DIR}/sockets/inc)
            target_link_libraries(PlcWebServer_test PRIVATE plc_web Threads::Threads)
            add_test(NAME PlcWebServer_test COMMAND PlcWebServer_test)

            # The same with HTTPS, logins and writes, when mbedTLS is given.
            set(MBEDTLS_DIR "" CACHE PATH "An mbedTLS 3.6 source tree, for PlcWebSecure_test")
            if(NOT MBEDTLS_DIR STREQUAL "")
                include(${INET_DIR}/tls/mbedtls.cmake)
                add_executable(PlcWebSecure_test web/test/PlcWebSecure_test.cpp
                    ${INET_DIR}/tls/src/MbedTlsServer.cpp
                    ${INET_DIR}/tls/src/WebPassword.cpp
                    ${INET_DIR}/sockets/src/SocketNetDevice.cpp
                    ${INET_DIR}/hw/freertos/src/xNetInterface.cpp
                    ${INET_DIR}/hw/freertos/src/xClient.cpp
                    ${INET_DIR}/hw/freertos/src/xHttpServer.cpp
                    ${INET_DIR}/dns/src/DnsClient.cpp
                    ${INET_DIR}/sntp/src/SntpClient.cpp)
                target_include_directories(PlcWebSecure_test PRIVATE
                    ${INET_DIR}/test/stub ${INET_DIR}/inc ${INET_DIR}/hw/freertos/inc
                    ${INET_DIR}/dns/inc ${INET_DIR}/sntp/inc ${INET_DIR}/sockets/inc
                    ${INET_DIR}/tls/inc ${INET_DIR}/tls/test)
                target_link_libraries(PlcWebSecure_test PRIVATE plc_web mbedtls_inet Threads::Threads)
                add_test(NAME PlcWebSecure_test COMMAND PlcWebSecure_test)
            endif()
        endif()
    endif()
endif()
