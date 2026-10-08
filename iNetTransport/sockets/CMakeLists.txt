# sockets: SocketNetDevice, an iNetDevice on a BSD socket API, so the
# interfaces and everything on them (xHttpServer, xHttpClient,
# xMqttClient) run over a host TCP/IP stack: lwIP on an MCU with its own
# Ethernet MAC (the STM32F207) or on an ESP32, or the operating system's.
#
# Options:
#   INET_SOCKETS_LWIP  OFF (default): the operating system's sockets.
#                      ON: lwIP's; needs LWIP_INCLUDE_DIRS, and adds
#                      hw/lwip's LwipNetif (the netif's link and address).
#   LWIP_INCLUDE_DIRS  lwIP's src/include, the port's include folder, and
#                      the folder holding lwipopts.h (;-separated)
#
# Added by iNetTransport/CMakeLists.txt, which also builds its host tests.
option(INET_SOCKETS_LWIP "SocketNetDevice on lwIP's socket API" OFF)
set(LWIP_INCLUDE_DIRS "" CACHE STRING "lwIP include folders, and lwipopts.h's (;-separated)")

add_library(inet_sockets STATIC
    inc/NetSockets.h
    inc/SocketNetDevice.h
    src/SocketNetDevice.cpp
)
target_include_directories(inet_sockets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(inet_sockets PUBLIC inet_core inet_dns inet_sntp)
target_compile_features(inet_sockets PUBLIC cxx_std_17)
set_target_properties(inet_sockets PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})

if(INET_SOCKETS_LWIP)
    if(LWIP_INCLUDE_DIRS STREQUAL "")
        message(FATAL_ERROR "INET_SOCKETS_LWIP is ON but LWIP_INCLUDE_DIRS is not set.")
    endif()
    target_sources(inet_sockets PRIVATE ../hw/lwip/inc/LwipNetif.h ../hw/lwip/src/LwipNetif.cpp)
    target_include_directories(inet_sockets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../hw/lwip/inc ${LWIP_INCLUDE_DIRS})
    target_compile_definitions(inet_sockets PUBLIC INET_SOCKETS_LWIP)
endif()
