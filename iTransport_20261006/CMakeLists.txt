cmake_minimum_required(VERSION 3.16)
project(sensor_transport LANGUAGES CXX)

# --- Source layout ------------------------------------------------------
# Every module has the same shape:
#   <module>/{inc,src,lib}              platform-independent code
#   <module>/hw/<platform>/{inc,src}    stm32, freertos, arduino, linux
#
#   itransport/   transport interfaces and bus implementations
#   isensor/      SensorBase, BMP280, BNO085 and their RTOS/POSIX variants
#   isafety/      Safe interface, relays, SafeDevice, SafeZone, CIP Safety CRCs
#   iplc/         PLC tag registry, CIP tag codec, CIP tag TCP server
#   ievent/       Event and EventQueue (header-only)
#   app/          entry points: hw/stm32/src/main.cpp, hw/linux/src/main_linux.cpp
#   docs/         HTML visualizers and the conversation transcript
#
# Sources still use flat #include "Foo.h" -- each target's
# target_include_directories() below is what puts the right inc/
# folders on the include path. Only itransport and isensor have CMake
# targets so far; isafety, iplc, ievent and app are not built by this
# file yet.
#
# Static libraries are written to their module's lib/ folder rather
# than the build tree.
set(ITRANSPORT_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/itransport/lib)
set(ISENSOR_LIB_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/isensor/lib)

# --- Hardware platform selection ----------------------------------------
# Selects which concrete transport implementation gets built:
#   STM32    -> Stm32HalI2CTransport, Stm32HalSPITransport, and their interrupt callbacks
#               (real CMSIS-RTOS2 mutex + notification, via FreeRtosTransport<TBus>)
#   ARDUINO  -> ArduinoWireTransport, for ANY Arduino-API target — including
#               a plain AVR Uno/Mega/Nano with no RTOS at all. Derives from
#               I2CTransport directly, not FreeRtosTransport<I2CTransport>; its
#               busy-wait/delay(1) fallback is what makes that possible.
#
#   cmake -B build -DSENSOR_FW_HARDWARE=ARDUINO ...
#
# Every hardware-specific source file carries its platform in the
# filename (Stm32*, Arduino*) precisely so it's obvious at a glance
# which files this flag controls and which (ISensorTransport.h,
# I2CTransport, SPITransport, SensorBase, BMP280Sensor) are shared.
set(SENSOR_FW_HARDWARE "STM32" CACHE STRING
    "Target hardware platform for the transport implementation: STM32 or ARDUINO")
set_property(CACHE SENSOR_FW_HARDWARE PROPERTY STRINGS STM32 ARDUINO)

if(NOT SENSOR_FW_HARDWARE STREQUAL "STM32" AND NOT SENSOR_FW_HARDWARE STREQUAL "ARDUINO")
    message(FATAL_ERROR
        "SENSOR_FW_HARDWARE is '${SENSOR_FW_HARDWARE}' — expected STM32 or ARDUINO.\n"
        "  cmake -B build -DSENSOR_FW_HARDWARE=STM32 ...\n"
        "  cmake -B build -DSENSOR_FW_HARDWARE=ARDUINO ...")
endif()

# --- CMSIS-RTOS2 header ----------------------------------------------
# Required unconditionally, but NOT because of sensor_transport or
# bmp280_driver — neither has any RTOS dependency at all now (see
# I2CTransport.h's own comment, and BMP280Sensor's sleep()/startThread(),
# both left at genuinely RTOS-free defaults). This is declared here
# because xbmp280_driver's xBMP280 directly calls osDelay()/osThreadNew(),
# and Stm32TransportLibrary's FreeRtosTransport<TBus> calls osMutexAcquire()/
# osThreadFlagsWait() — both real RTOS dependencies, just scoped to
# those specific libraries rather than the whole build.
set(CMSIS_RTOS_INCLUDE_DIR "" CACHE PATH "Directory containing cmsis_os2.h")
if(CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR
        "CMSIS_RTOS_INCLUDE_DIR is not set. Point it at the directory "
        "containing cmsis_os2.h, e.g.:\n"
        "  cmake -B build -DCMSIS_RTOS_INCLUDE_DIR=/path/to/Middlewares/.../CMSIS_RTOS_V2 ...\n"
        "(Needed by xbmp280_driver's xBMP280, and by Stm32TransportLibrary's\n"
        " FreeRtosTransport — NOT by sensor_transport or bmp280_driver\n"
        " themselves, neither of which has any RTOS dependency anymore.)")
endif()

# --- sensor_transport ----------------------------------------------------
# I2CTransport/SPITransport are now fully hardware- AND platform-agnostic:
# no STM32 types, no RTOS header, nothing. Every real primitive
# (mutex acquire/release, transfer-completion wait) is a virtual hook
# with a working, if less efficient, default — see I2CTransport.h.
add_library(sensor_transport STATIC
    itransport/inc/ISensorTransport.h
    itransport/inc/BusTransport.h
    itransport/src/BusTransport.cpp
    itransport/inc/I2CTransport.h
    itransport/src/I2CTransport.cpp
    itransport/inc/SPITransport.h
    itransport/src/SPITransport.cpp
    itransport/inc/iTransportOneWire.h
    itransport/inc/OneWireUartTransport.h
    itransport/src/OneWireUartTransport.cpp
)

target_include_directories(sensor_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/itransport/inc
)

target_compile_features(sensor_transport PUBLIC cxx_std_17)

set_target_properties(sensor_transport PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ITRANSPORT_LIB_DIR})

# --- Hardware transport library -----------------------------------------
# Exactly one of these two blocks runs, based on SENSOR_FW_HARDWARE.
# Either way, HARDWARE_TRANSPORT_LIBRARY ends up set to whichever
# target was actually built, so anything further down (or a top-level
# project) can write target_link_libraries(app PUBLIC
# ${HARDWARE_TRANSPORT_LIBRARY}) without caring which hardware it is.
if(SENSOR_FW_HARDWARE STREQUAL "STM32")

    # stm32f4xx_hal.h now lives entirely inside Stm32HalI2CTransport.h/
    # Stm32HalSPITransport.h/Stm32I2CItCallbacks.cpp — sensor_transport
    # itself never needs it. This is the only place STM32_HAL_INCLUDE_DIR
    # is required.
    set(STM32_HAL_INCLUDE_DIR "" CACHE PATH "Directory containing stm32f4xx_hal.h (or your STM32 family's HAL header)")
    if(STM32_HAL_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR
            "STM32_HAL_INCLUDE_DIR is not set. Point it at the directory "
            "containing stm32f4xx_hal.h, e.g.:\n"
            "  cmake -B build -DSENSOR_FW_HARDWARE=STM32 -DSTM32_HAL_INCLUDE_DIR=/path/to/Drivers/STM32F4xx_HAL_Driver/Inc ...")
    endif()

    # FreeRtosTransport<TBus> (header-only template) supplies the real
    # osMutexAcquire()/osMutexRelease()/osThreadFlagsSet()/
    # osThreadFlagsWait() calls that BusTransport's default hooks are
    # RTOS-free fallbacks for, once for I2C and SPI alike.
    # Stm32HalI2CTransport and Stm32HalSPITransport derive from
    # FreeRtosTransport<I2CTransport>/FreeRtosTransport<SPITransport>
    # to get real cross-task exclusion and to be woken by the bus
    # interrupt, rather than the polling/no-op defaults. Needs
    # CMSIS_RTOS_INCLUDE_DIR (declared unconditionally above) on top
    # of STM32_HAL_INCLUDE_DIR.
    #
    # The STM32 transport headers include the project's own main.h
    # (which pulls in the right HAL header for the target family), so
    # the directory holding main.h must be on the include path too.
    #
    # Stm32I2CItCallbacks.cpp/Stm32SpiItCallbacks.cpp/Stm32UartItCallbacks.cpp
    # override the HAL's weak callbacks. Listed here they end up inside
    # a static library, where the linker will not pull them in over the
    # HAL's own weak defaults — compile them into the application
    # itself (or link this library whole-archive) for them to take effect.
    #
    # Stm32HalI2CTransport/Stm32HalSPITransport are the only classes in
    # this codebase that call HAL_I2C_*/HAL_SPI_* functions directly —
    # everything generic lives in I2CTransport/SPITransport, which this
    # links against. Stm32I2CItCallbacks.cpp is the HAL ISR callback
    # glue (HAL_I2C_MemTxCpltCallback etc.) that feeds
    # I2CTransport::onTransferComplete() — bundled here since it only
    # makes sense alongside Stm32HalI2CTransport.
    add_library(Stm32TransportLibrary STATIC
        itransport/hw/freertos/inc/FreeRtosTransport.h
        itransport/hw/stm32/inc/Stm32HalI2CTransport.h
        itransport/hw/stm32/src/Stm32HalI2CTransport.cpp
        itransport/hw/stm32/inc/Stm32HalSPITransport.h
        itransport/hw/stm32/src/Stm32HalSPITransport.cpp
        itransport/hw/stm32/src/Stm32I2CItCallbacks.cpp
        itransport/hw/stm32/src/Stm32SpiItCallbacks.cpp
        itransport/hw/stm32/inc/Stm32HalOneWireTransport.h
        itransport/hw/stm32/src/Stm32HalOneWireTransport.cpp
        itransport/inc/iTransport.h
        itransport/hw/stm32/inc/Stm32HalUartTransport.h
        itransport/hw/stm32/src/Stm32HalUartTransport.cpp
        itransport/hw/stm32/src/Stm32UartItCallbacks.cpp
    )

    target_include_directories(Stm32TransportLibrary PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/itransport/hw/stm32/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/itransport/hw/freertos/inc
        ${STM32_HAL_INCLUDE_DIR}
        ${CMSIS_RTOS_INCLUDE_DIR}
    )

    target_compile_features(Stm32TransportLibrary PUBLIC cxx_std_17)

    set_target_properties(Stm32TransportLibrary PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ITRANSPORT_LIB_DIR})

    target_link_libraries(Stm32TransportLibrary PUBLIC sensor_transport)

    set(HARDWARE_TRANSPORT_LIBRARY Stm32TransportLibrary)

elseif(SENSOR_FW_HARDWARE STREQUAL "ARDUINO")

    # ArduinoWireTransport.h #includes <Arduino.h> and <Wire.h> directly
    # — that's the Arduino core, not part of this repo. NOT included:
    # cmsis_os2.h — this transport derives from I2CTransport directly,
    # not FreeRtosTransport<I2CTransport>, so it needs no RTOS at all, working on
    # a plain AVR Uno/Mega/Nano as much as on something FreeRTOS-backed.
    set(ARDUINO_CORE_INCLUDE_DIR "" CACHE PATH "Directory containing Wire.h and the rest of the Arduino core headers")
    if(ARDUINO_CORE_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR
            "ARDUINO_CORE_INCLUDE_DIR is not set. Point it at the directory "
            "containing Wire.h, e.g.:\n"
            "  cmake -B build -DSENSOR_FW_HARDWARE=ARDUINO -DARDUINO_CORE_INCLUDE_DIR=/path/to/cores/esp32 ...")
    endif()

    add_library(ArduinoTransportLibrary STATIC
        itransport/hw/arduino/inc/ArduinoWireTransport.h
        itransport/hw/arduino/src/ArduinoWireTransport.cpp
    )

    target_include_directories(ArduinoTransportLibrary PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/itransport/hw/arduino/inc
        ${ARDUINO_CORE_INCLUDE_DIR}
    )

    target_compile_features(ArduinoTransportLibrary PUBLIC cxx_std_17)

    set_target_properties(ArduinoTransportLibrary PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ITRANSPORT_LIB_DIR})

    target_link_libraries(ArduinoTransportLibrary PUBLIC sensor_transport)

    set(HARDWARE_TRANSPORT_LIBRARY ArduinoTransportLibrary)

endif()

# --- sensor_base -------------------------------------------------------
# SensorBase.h/.cpp only need ISensorTransport.h — not the HAL/RTOS
# headers directly, since it talks to hardware exclusively through the
# ISensorTransport& it's given, never a concrete transport. Linking
# against sensor_transport (rather than duplicating an include path)
# is what actually gets ISensorTransport.h onto sensor_base's include
# path. Hardware-agnostic: unaffected by SENSOR_FW_HARDWARE.
add_library(sensor_base STATIC
    isensor/inc/SensorBase.h
    isensor/src/SensorBase.cpp
)

target_include_directories(sensor_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/isensor/inc
)

target_compile_features(sensor_base PUBLIC cxx_std_17)

set_target_properties(sensor_base PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ISENSOR_LIB_DIR})

target_link_libraries(sensor_base PUBLIC sensor_transport)

# --- WiringPiTransportLibrary --------------------------------------------
# WiringPiTransport/WiringPiI2cTransport extend ISensorTransport
# directly, not I2CTransport — WiringPi's calls are all synchronous,
# so none of I2CTransport's mutex-arbitration/transfer-notification
# machinery would do anything useful here (see WiringPiTransport.h).
# Not gated behind SENSOR_FW_HARDWARE: this isn't competing with the
# STM32/ARDUINO choice above for the same role, it's a separate
# concrete option a Raspberry Pi project links against directly.
set(WIRINGPI_INCLUDE_DIR "" CACHE PATH "Directory containing wiringPiI2C.h (and the rest of WiringPi's headers)")
if(WIRINGPI_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR
        "WIRINGPI_INCLUDE_DIR is not set. Point it at the directory "
        "containing wiringPiI2C.h, e.g.:\n"
        "  cmake -B build -DWIRINGPI_INCLUDE_DIR=/usr/include ...")
endif()

add_library(WiringPiTransportLibrary STATIC
    itransport/hw/linux/inc/IFileTransport.h
    itransport/hw/linux/src/IFileTransport.cpp
    itransport/hw/linux/inc/WiringPiTransport.h
    itransport/hw/linux/inc/WiringPiI2cTransport.h
    itransport/hw/linux/src/WiringPiI2cTransport.cpp
)

target_include_directories(WiringPiTransportLibrary PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/itransport/hw/linux/inc
    ${WIRINGPI_INCLUDE_DIR}
)

target_compile_features(WiringPiTransportLibrary PUBLIC cxx_std_17)

set_target_properties(WiringPiTransportLibrary PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ITRANSPORT_LIB_DIR})

# sem_wait()/sem_post() (IFileTransport) live in libpthread on older
# glibc versions — merged into libc directly on 2.34+, but not
# guaranteed on every target. find_package(Threads) + Threads::Threads
# is the portable way to get whichever is actually needed.
find_package(Threads REQUIRED)
target_link_libraries(WiringPiTransportLibrary PUBLIC Threads::Threads)

target_link_libraries(WiringPiTransportLibrary PUBLIC sensor_transport)

# --- bmp280_driver -------------------------------------------------------
# BMP280Sensor.h only needs SensorBase.h. No RTOS dependency anywhere
# in this library anymore: sleep()/startThread() are left at
# SensorBase's genuinely RTOS-free defaults (see BMP280Sensor.h's own
# comment) — this only makes forward progress when something calls
# main() repeatedly, i.e. the user's own loop(). Note this does NOT
# link HARDWARE_TRANSPORT_LIBRARY — BMP280Sensor only ever references
# ISensorTransport&, never a concrete transport type, so that link
# happens at the application level, not here.
add_library(bmp280_driver STATIC
    isensor/inc/BMP280Sensor.h
    isensor/src/BMP280Sensor.cpp
    isensor/inc/Barometer.h
    isensor/inc/Altimeter.h
    isensor/inc/Thermometer.h
)

target_include_directories(bmp280_driver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/isensor/inc
)

target_compile_features(bmp280_driver PUBLIC cxx_std_17)

set_target_properties(bmp280_driver PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ISENSOR_LIB_DIR})

target_link_libraries(bmp280_driver PUBLIC sensor_base)

# --- xbmp280_driver --------------------------------------------------------
# xBMP280 extends BMP280Sensor, overriding only sleep()/startThread()
# with real osDelay()/osThreadNew() calls — everything else (the state
# machine itself) is inherited unchanged. Build this in addition to
# bmp280_driver when a real RTOS is available and you want xBMP280 to
# run in its own dedicated task rather than needing a manual loop().
add_library(xbmp280_driver STATIC
    isensor/hw/freertos/inc/xBMP280.h
    isensor/hw/freertos/src/xBMP280.cpp
    isensor/hw/freertos/inc/xSensorBase.h
)

target_include_directories(xbmp280_driver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/isensor/hw/freertos/inc
    ${CMSIS_RTOS_INCLUDE_DIR}
)

target_compile_features(xbmp280_driver PUBLIC cxx_std_17)

set_target_properties(xbmp280_driver PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ISENSOR_LIB_DIR})

target_link_libraries(xbmp280_driver PUBLIC bmp280_driver)

# --- pbmp280_driver --------------------------------------------------------
# pBMP280 extends BMP280Sensor, overriding sleep()/startThread()/
# StopThread() with real POSIX pthread_create()/nanosleep() calls —
# everything else (the state machine itself) is inherited unchanged.
# No CMSIS-RTOS dependency at all here: this is genuinely just POSIX
# (pthread, nanosleep, std::atomic), needing only Threads::Threads to
# link — unlike xbmp280_driver, this never needs CMSIS_RTOS_INCLUDE_DIR.
add_library(pbmp280_driver STATIC
    isensor/hw/linux/inc/pBMP280.h
    isensor/hw/linux/src/pBMP280.cpp
    isensor/hw/linux/inc/pSensorBase.h
)

target_include_directories(pbmp280_driver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/isensor/hw/linux/inc
)

target_compile_features(pbmp280_driver PUBLIC cxx_std_17)

set_target_properties(pbmp280_driver PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ISENSOR_LIB_DIR})

find_package(Threads REQUIRED)
target_link_libraries(pbmp280_driver PUBLIC Threads::Threads)

target_link_libraries(pbmp280_driver PUBLIC bmp280_driver)
