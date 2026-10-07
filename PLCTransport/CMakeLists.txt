# PLCTransport: the PLC tag database (PlcTagRegistry and its tag
# descriptors and data types), the CIP tag message codec, and the CIP
# tag TCP server.
#
#   inc/, src/          platform-independent: the tag registry, the
#                       codec and CipFrame
#   hw/freertos/        CipTagTcpServer, on FreeRTOS and lwIP
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
# cip_tag_server is built when FREERTOS_INCLUDE_DIRS and
# LWIP_INCLUDE_DIRS are both set, and needs PLC_MUTEX_USE_CMSIS_RTOS2.
#
# Host build (plc_tags only):
#   cmake -B build
#   cmake --build build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(PLCTransport LANGUAGES CXX)
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
