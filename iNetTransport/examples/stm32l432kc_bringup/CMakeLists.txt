# Bring-up firmware for the iNetTransport drivers on a NUCLEO-L432KC: W5500
# Ethernet on SPI1 and/or an ESP-AT Wi-Fi module on USART1. Standalone:
# it needs the GNU Arm toolchain and ST's STM32CubeL4 firmware package
# (which CubeIDE/CubeMX download to ~/STM32Cube/Repository), no CubeMX
# project. See BRINGUP.md.
#
#   cmake -S . -B build -DSTM32CUBE_L4_DIR=$HOME/STM32Cube/Repository/STM32Cube_FW_L4_V1.18.1
#   cmake --build build
#   -> build/inet_bringup.bin, .hex, .elf
#
# Options:
#   -DBRINGUP_ETH=ON|OFF        W5500 (default ON)
#   -DBRINGUP_WIFI=ON|OFF       ESP-AT (default OFF), with
#   -DWIFI_SSID=... -DWIFI_PASS=... -DESP_BAUD=115200
#   -DETH_DHCP=ON|OFF           static address otherwise: edit Core/Inc/config.h
#
# The package's folders can also be given one by one (STM32_HAL_DIR,
# STM32_CMSIS_DIR, STM32_DEVICE_DIR, FREERTOS_DIR, FREERTOS_RTOS2_DIR)
# when they don't follow the package layout.
cmake_minimum_required(VERSION 3.16)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/arm-none-eabi.cmake)
endif()

project(inet_bringup C CXX ASM)

set(STM32CUBE_L4_DIR "" CACHE PATH "STM32CubeL4 firmware package (STM32Cube_FW_L4_V1.x.y)")
set(STM32_HAL_DIR      "${STM32CUBE_L4_DIR}/Drivers/STM32L4xx_HAL_Driver" CACHE PATH "")
set(STM32_CMSIS_DIR    "${STM32CUBE_L4_DIR}/Drivers/CMSIS" CACHE PATH "")
set(STM32_DEVICE_DIR   "${STM32_CMSIS_DIR}/Device/ST/STM32L4xx" CACHE PATH "")
set(FREERTOS_DIR       "${STM32CUBE_L4_DIR}/Middlewares/Third_Party/FreeRTOS/Source" CACHE PATH "")
set(FREERTOS_RTOS2_DIR "${FREERTOS_DIR}/CMSIS_RTOS_V2" CACHE PATH "")

if(NOT EXISTS ${STM32_HAL_DIR}/Inc/stm32l4xx_hal.h)
    message(FATAL_ERROR "STM32L4 HAL not found at ${STM32_HAL_DIR}.\n"
        "Point STM32CUBE_L4_DIR at the STM32CubeL4 package, e.g.\n"
        "  -DSTM32CUBE_L4_DIR=$HOME/STM32Cube/Repository/STM32Cube_FW_L4_V1.18.1")
endif()

option(BRINGUP_ETH  "W5500 Ethernet on SPI1" ON)
option(BRINGUP_WIFI "ESP-AT Wi-Fi on USART1" OFF)
option(ETH_DHCP     "Ethernet address by DHCP" ON)
set(WIFI_SSID "" CACHE STRING "Wi-Fi network to join")
set(WIFI_PASS "" CACHE STRING "Its passphrase")
set(ESP_BAUD 115200 CACHE STRING "ESP-AT module's UART baud rate")

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(IT ${REPO}/iTransport/itransport)
set(INET ${REPO}/iNetTransport)

set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)

# --- ST's HAL, CMSIS and FreeRTOS: built as they come ---------------------
set(HAL_MODULES hal hal_cortex hal_dma hal_dma_ex hal_exti hal_flash hal_flash_ex hal_flash_ramfunc
                hal_gpio hal_pwr hal_pwr_ex hal_rcc hal_rcc_ex hal_spi hal_spi_ex hal_uart hal_uart_ex)
set(VENDOR_SOURCES
    ${STM32_DEVICE_DIR}/Source/Templates/system_stm32l4xx.c
    ${STM32_DEVICE_DIR}/Source/Templates/gcc/startup_stm32l432xx.s
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_DIR}/event_groups.c
    ${FREERTOS_DIR}/stream_buffer.c
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F/port.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
    ${FREERTOS_RTOS2_DIR}/cmsis_os2.c
)
foreach(m ${HAL_MODULES})
    list(APPEND VENDOR_SOURCES ${STM32_HAL_DIR}/Src/stm32l4xx_${m}.c)
endforeach()

set(INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc
    ${STM32_HAL_DIR}/Inc
    ${STM32_CMSIS_DIR}/Include
    ${STM32_DEVICE_DIR}/Include
    ${STM32_CMSIS_DIR}/RTOS2/Include
    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F
    ${FREERTOS_RTOS2_DIR}
    ${IT}/inc ${IT}/hw/stm32/inc ${IT}/hw/freertos/inc
    ${INET}/inc ${INET}/hw/freertos/inc ${INET}/dhcp/inc ${INET}/dns/inc ${INET}/sntp/inc
    ${INET}/w5500/inc ${INET}/espat/inc
)

set(DEFS STM32L432xx USE_HAL_DRIVER
    BRINGUP_ETH=$<BOOL:${BRINGUP_ETH}> BRINGUP_WIFI=$<BOOL:${BRINGUP_WIFI}>
    ETH_DHCP=$<BOOL:${ETH_DHCP}> ESP_BAUD=${ESP_BAUD})
if(NOT WIFI_SSID STREQUAL "")
    list(APPEND DEFS "WIFI_SSID=\"${WIFI_SSID}\"" "WIFI_PASS=\"${WIFI_PASS}\"")
endif()

add_library(vendor OBJECT ${VENDOR_SOURCES})
target_include_directories(vendor PRIVATE ${INCLUDES})
target_compile_definitions(vendor PRIVATE ${DEFS})
target_compile_options(vendor PRIVATE ${CPU_FLAGS} -Os -g -ffunction-sections -fdata-sections)

# --- this repository and the firmware: warnings on ------------------------
add_executable(inet_bringup.elf
    Core/Src/main.cpp
    Core/Src/board.cpp
    Core/Src/it.cpp
    Core/Src/log.cpp
    Core/Src/bringup.cpp
    Core/Src/services.cpp
    Core/Src/syscalls.c
    ${IT}/src/BusTransport.cpp
    ${IT}/src/SpiBlockTransport.cpp
    ${IT}/src/OneWireUartTransport.cpp
    ${IT}/hw/stm32/src/Stm32HalSpiBlockTransport.cpp
    ${IT}/hw/stm32/src/Stm32SpiItCallbacks.cpp
    ${IT}/hw/stm32/src/Stm32HalUartTransport.cpp
    ${IT}/hw/stm32/src/Stm32UartItCallbacks.cpp
    ${INET}/hw/freertos/src/xNetInterface.cpp
    ${INET}/hw/freertos/src/xClient.cpp
    ${INET}/hw/freertos/src/xWifi.cpp
    ${INET}/dhcp/src/DhcpClient.cpp
    ${INET}/dns/src/DnsClient.cpp
    ${INET}/sntp/src/SntpClient.cpp
    $<TARGET_OBJECTS:vendor>
)
target_include_directories(inet_bringup.elf PRIVATE ${INCLUDES})
target_compile_definitions(inet_bringup.elf PRIVATE ${DEFS})
target_compile_features(inet_bringup.elf PRIVATE cxx_std_17)
target_compile_options(inet_bringup.elf PRIVATE ${CPU_FLAGS} -Os -g -ffunction-sections -fdata-sections
    -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
target_link_options(inet_bringup.elf PRIVATE ${CPU_FLAGS}
    -T${CMAKE_CURRENT_SOURCE_DIR}/STM32L432KCUx_FLASH.ld
    --specs=nano.specs --specs=nosys.specs
    -Wl,--gc-sections -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/inet_bringup.map -Wl,--print-memory-usage)

add_custom_command(TARGET inet_bringup.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary inet_bringup.elf inet_bringup.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex inet_bringup.elf inet_bringup.hex
    COMMAND ${CMAKE_SIZE} inet_bringup.elf
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
