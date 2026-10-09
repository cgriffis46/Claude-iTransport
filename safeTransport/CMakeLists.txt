cmake_minimum_required(VERSION 3.16)
project(safe_transport LANGUAGES CXX)

# safeTransport: the Safe interface, safety devices and zones, CIP
# Safety codecs, events, and the STM32 safety relays. The PLC tag
# database and CIP tag server are in ../PLCTransport.
#
# The transports (iTransport.h, Stm32HalUartTransport, ...) are no
# longer copied into this folder. They come from
# iTransport/itransport, whose own CMakeLists.txt is added
# below; its targets put the right inc/ folders on the include path,
# so sources here keep their flat #include "iTransport.h".
#
# Options:
#   SENSOR_FW_HARDWARE        STM32 (default) or HOST, passed on to
#                             itransport (see its CMakeLists.txt)
#   CMSIS_RTOS_INCLUDE_DIR    directory holding cmsis_os2.h (STM32)
#   STM32_HAL_INCLUDE_DIR     directory holding the HAL headers (STM32)
#   STM32_PROJECT_INCLUDE_DIR the CubeMX project's Core/Inc, holding
#                             main.h (STM32)
#   FREERTOS_INCLUDE_DIRS     FreeRTOS include/ and portable/ folders,
#                             and FreeRTOSConfig.h's folder (STM32)
#
# Host build of everything that needs no hardware, and the tests:
#   cmake -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF \
#         -DSENSOR_FW_BUILD_TESTS=ON
#   cmake --build build && ctest --test-dir build

set(ITRANSPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iTransport/itransport
    CACHE PATH "The itransport folder to build against")

# A Raspberry Pi transport has no use here.
set(ITRANSPORT_BUILD_WIRINGPI OFF CACHE BOOL "Build WiringPiTransportLibrary")

# Before itransport, so its tests are run by this ctest too.
option(SENSOR_FW_BUILD_TESTS "Build the host tests" OFF)
if(SENSOR_FW_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(${ITRANSPORT_DIR} itransport)

# --- safe_event ----------------------------------------------------------
# Event and EventQueue. Header-only.
add_library(safe_event INTERFACE)

target_include_directories(safe_event INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(safe_event INTERFACE cxx_std_17)

# --- safety_core ---------------------------------------------------------
# The Safe interface, SafeDevice, SafeZone, and the CIP Safety codecs
# and CRCs. Plain C++: no transport, HAL or RTOS.
add_library(safety_core STATIC
    Safe.h
    SafeInput.h
    SafeOutput.h
    SafeInterlock.h
    SafeDevice.h
    SafeDevice.cpp
    SafeZone.h
    SafeZone.cpp
    SafeZoneMember.h
    SafeToEventBridge.h
    LightCurtain.h
    SafetyProtocolCodec.h
    CipSafetyCodec.h
    CipSafetyBaseFormatCrc.h
    Crc8.h
    CrcS3.h
)

target_include_directories(safety_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(safety_core PUBLIC cxx_std_17)

target_link_libraries(safety_core PUBLIC safe_event)

# --- safety_transport ----------------------------------------------------
# Safe inputs and outputs carried over an iTransport, and the UART
# loopback check. Only the iTransport interface is needed, so this
# links sensor_transport, not a hardware library.
add_library(safety_transport STATIC
    SafeTransport.h
    TransportSafeInput.h
    TransportSafeOutput.h
    UartLoopbackSafe.h
    UartLoopbackSafe.cpp
    UartLoopbackChannelSafeInput.h
    UartLoopbackChannelSafeInput.cpp
    DualChannelLink.h
    DualChannelLink.cpp
    iTransportCan.h
    iTransportEthernet.h
)

target_link_libraries(safety_transport PUBLIC safety_core sensor_transport)

# --- tests ---------------------------------------------------------------
if(SENSOR_FW_BUILD_TESTS)
    # Two simulated MCUs joined by the heartbeat link.
    add_executable(dual_channel_link_test test/dual_channel_link_test.cpp)
    target_link_libraries(dual_channel_link_test PRIVATE safety_transport)
    add_test(NAME dual_channel_link_test COMMAND dual_channel_link_test)
endif()

# --- safezone_json -------------------------------------------------------
# Saves and loads SafeZones as JSON. Built only where nlohmann/json is
# installed.
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    add_library(safezone_json STATIC
        SafeZoneJsonPersistence.h
        SafeZoneJsonPersistence.cpp
    )

    target_link_libraries(safezone_json PUBLIC safety_core PRIVATE nlohmann_json::nlohmann_json)
else()
    message(STATUS "nlohmann_json not found: safezone_json is not built")
endif()

if(SENSOR_FW_HARDWARE STREQUAL "STM32")

    set(FREERTOS_INCLUDE_DIRS "" CACHE STRING
        "FreeRTOS include folders: Source/include, the portable/ port folder, and FreeRTOSConfig.h's folder")

    # The STM32 code reaches the HAL, CMSIS-RTOS2 and the project's
    # main.h through the include folders Stm32TransportLibrary already
    # carries, plus these.
    set(SAFETRANSPORT_STM32_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FREERTOS_INCLUDE_DIRS}
    )

    # --- stm32_safety ------------------------------------------------------
    # GPIO safety I/O, the UART safety transport, the CAN safety
    # broadcaster and the safety relays, on the STM32 HAL.
    add_library(stm32_safety STATIC
        GpioSafeInput.h
        GpioSafeInput.cpp
        GpioSafeOutput.h
        GpioSafeOutput.cpp
        Stm32CanSafetyBroadcaster.h
        Stm32CanSafetyBroadcaster.cpp
        Stm32SafeUartTransport.h
        Stm32SafeUartTransport.cpp
        Stm32_Safe_Relay.h
        Stm32_Safe_Relay.cpp
        Stm32L4SafetyRelay.h
        Stm32L4SafetyRelay.cpp
        xBNO085.h
        xBNO085.cpp
    )

    target_include_directories(stm32_safety PUBLIC ${SAFETRANSPORT_STM32_INCLUDES})

    target_link_libraries(stm32_safety PUBLIC safety_transport Stm32TransportLibrary)

    # --- stm32_can_transport -----------------------------------------------
    # iTransportCan on the STM32 HAL CAN driver. Separate from
    # stm32_safety because it needs a CubeMX project with CAN enabled
    # (HAL_CAN_MODULE_ENABLED); so does Stm32CanItCallbacks.cpp below.
    add_library(stm32_can_transport STATIC
        Stm32HalCanTransport.h
        Stm32HalCanTransport.cpp
    )

    target_include_directories(stm32_can_transport PUBLIC ${SAFETRANSPORT_STM32_INCLUDES})

    target_link_libraries(stm32_can_transport PUBLIC safety_transport Stm32TransportLibrary)

    # --- Application sources -----------------------------------------------
    # HAL callbacks and the STM32F207ZG_SafeRelay wiring. Like
    # itransport's *ItCallbacks.cpp, these must be compiled into the
    # application itself — inside a static library the linker would
    # keep the HAL's weak defaults instead. An application adds them
    # with:
    #   target_sources(app PRIVATE ${SAFETRANSPORT_APP_SOURCES})
    set(SAFETRANSPORT_APP_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/Stm32CanItCallbacks.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/CipSafeRelayUartLoopback.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/CipSafeRelayWatchdogFeed.cpp
        ${ITRANSPORT_DIR}/hw/stm32/src/Stm32I2CItCallbacks.cpp
        ${ITRANSPORT_DIR}/hw/stm32/src/Stm32SpiItCallbacks.cpp
        ${ITRANSPORT_DIR}/hw/stm32/src/Stm32UartItCallbacks.cpp
    )

endif()

# --- linux_udp_transport -------------------------------------------------
# iTransportEthernet over a POSIX UDP socket (Linux/KR260).
if(UNIX AND NOT SENSOR_FW_HARDWARE STREQUAL "STM32")
    add_library(linux_udp_transport STATIC
        LinuxUdpTransport.h
        LinuxUdpTransport.cpp
    )

    find_package(Threads REQUIRED)
    target_link_libraries(linux_udp_transport PUBLIC safety_transport Threads::Threads)
endif()
