cmake_minimum_required(VERSION 3.16)
project(sensor_transport LANGUAGES CXX)

# --- Hardware platform selection ----------------------------------------
# Selects which concrete transport implementation gets built:
#   STM32    -> Stm32HalI2CTransport, Stm32HalSPITransport, Stm32I2CItCallbacks
#               (real CMSIS-RTOS2 mutex + notification, via FreeRtosI2CTransport)
#   ARDUINO  -> ArduinoWireTransport, for ANY Arduino-API target — including
#               a plain AVR Uno/Mega/Nano with no RTOS at all. Derives from
#               I2CTransport directly, not FreeRtosI2CTransport; its
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
# and Stm32TransportLibrary's FreeRtosI2CTransport calls osMutexAcquire()/
# osThreadFlagsWait() — both real RTOS dependencies, just scoped to
# those specific libraries rather than the whole build.
set(CMSIS_RTOS_INCLUDE_DIR "" CACHE PATH "Directory containing cmsis_os2.h")
if(CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR
        "CMSIS_RTOS_INCLUDE_DIR is not set. Point it at the directory "
        "containing cmsis_os2.h, e.g.:\n"
        "  cmake -B build -DCMSIS_RTOS_INCLUDE_DIR=/path/to/Middlewares/.../CMSIS_RTOS_V2 ...\n"
        "(Needed by xbmp280_driver's xBMP280, and by Stm32TransportLibrary's\n"
        " FreeRtosI2CTransport — NOT by sensor_transport or bmp280_driver\n"
        " themselves, neither of which has any RTOS dependency anymore.)")
endif()

# --- sensor_transport ----------------------------------------------------
# I2CTransport/SPITransport are now fully hardware- AND platform-agnostic:
# no STM32 types, no RTOS header, nothing. Every real primitive
# (mutex acquire/release, transfer-completion wait) is a virtual hook
# with a working, if less efficient, default — see I2CTransport.h.
add_library(sensor_transport STATIC
    ISensorTransport.h
    I2CTransport.h
    I2CTransport.cpp
    SPITransport.h
    SPITransport.cpp
)

target_include_directories(sensor_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sensor_transport PUBLIC cxx_std_17)

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

    # FreeRtosI2CTransport supplies the real osMutexAcquire()/
    # osMutexRelease()/osThreadFlagsSet()/osThreadFlagsWait() calls that
    # I2CTransport's default hooks are RTOS-free fallbacks for.
    # Stm32HalI2CTransport derives from it (not I2CTransport directly)
    # to get real cross-task exclusion and a real notification wait,
    # rather than the busy-wait/no-op defaults. Needs CMSIS_RTOS_INCLUDE_DIR
    # (declared unconditionally above) on top of STM32_HAL_INCLUDE_DIR.
    #
    # Stm32HalI2CTransport/Stm32HalSPITransport are the only classes in
    # this codebase that call HAL_I2C_*/HAL_SPI_* functions directly —
    # everything generic lives in I2CTransport/SPITransport, which this
    # links against. Stm32I2CItCallbacks.cpp is the HAL ISR callback
    # glue (HAL_I2C_MemTxCpltCallback etc.) that feeds
    # I2CTransport::onTransferComplete() — bundled here since it only
    # makes sense alongside Stm32HalI2CTransport.
    add_library(Stm32TransportLibrary STATIC
        FreeRtosI2CTransport.h
        FreeRtosI2CTransport.cpp
        Stm32HalI2CTransport.h
        Stm32HalI2CTransport.cpp
        Stm32HalSPITransport.h
        Stm32HalSPITransport.cpp
        Stm32I2CItCallbacks.cpp
    )

    target_include_directories(Stm32TransportLibrary PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${STM32_HAL_INCLUDE_DIR}
        ${CMSIS_RTOS_INCLUDE_DIR}
    )

    target_compile_features(Stm32TransportLibrary PUBLIC cxx_std_17)

    target_link_libraries(Stm32TransportLibrary PUBLIC sensor_transport)

    set(HARDWARE_TRANSPORT_LIBRARY Stm32TransportLibrary)

elseif(SENSOR_FW_HARDWARE STREQUAL "ARDUINO")

    # ArduinoWireTransport.h #includes <Arduino.h> and <Wire.h> directly
    # — that's the Arduino core, not part of this repo. NOT included:
    # cmsis_os2.h — this transport derives from I2CTransport directly,
    # not FreeRtosI2CTransport, so it needs no RTOS at all, working on
    # a plain AVR Uno/Mega/Nano as much as on something FreeRTOS-backed.
    set(ARDUINO_CORE_INCLUDE_DIR "" CACHE PATH "Directory containing Wire.h and the rest of the Arduino core headers")
    if(ARDUINO_CORE_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR
            "ARDUINO_CORE_INCLUDE_DIR is not set. Point it at the directory "
            "containing Wire.h, e.g.:\n"
            "  cmake -B build -DSENSOR_FW_HARDWARE=ARDUINO -DARDUINO_CORE_INCLUDE_DIR=/path/to/cores/esp32 ...")
    endif()

    add_library(ArduinoTransportLibrary STATIC
        ArduinoWireTransport.h
        ArduinoWireTransport.cpp
    )

    target_include_directories(ArduinoTransportLibrary PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ARDUINO_CORE_INCLUDE_DIR}
    )

    target_compile_features(ArduinoTransportLibrary PUBLIC cxx_std_17)

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
    SensorBase.h
    SensorBase.cpp
)

target_include_directories(sensor_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sensor_base PUBLIC cxx_std_17)

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
    IFileTransport.h
    IFileTransport.cpp
    WiringPiTransport.h
    WiringPiI2cTransport.h
    WiringPiI2cTransport.cpp
)

target_include_directories(WiringPiTransportLibrary PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WIRINGPI_INCLUDE_DIR}
)

target_compile_features(WiringPiTransportLibrary PUBLIC cxx_std_17)

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
    BMP280Sensor.h
    BMP280Sensor.cpp
    Barometer.h
    Altimeter.h
    Thermometer.h
)

target_include_directories(bmp280_driver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(bmp280_driver PUBLIC cxx_std_17)

target_link_libraries(bmp280_driver PUBLIC sensor_base)

# --- xbmp280_driver --------------------------------------------------------
# xBMP280 extends BMP280Sensor, overriding only sleep()/startThread()
# with real osDelay()/osThreadNew() calls — everything else (the state
# machine itself) is inherited unchanged. Build this in addition to
# bmp280_driver when a real RTOS is available and you want xBMP280 to
# run in its own dedicated task rather than needing a manual loop().
add_library(xbmp280_driver STATIC
    xBMP280.h
    xBMP280.cpp
    xSensorBase.h
)

target_include_directories(xbmp280_driver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMSIS_RTOS_INCLUDE_DIR}
)

target_compile_features(xbmp280_driver PUBLIC cxx_std_17)

target_link_libraries(xbmp280_driver PUBLIC bmp280_driver)

# --- pbmp280_driver --------------------------------------------------------
# pBMP280 extends BMP280Sensor, overriding sleep()/startThread()/
# StopThread() with real POSIX pthread_create()/nanosleep() calls —
# everything else (the state machine itself) is inherited unchanged.
# No CMSIS-RTOS dependency at all here: this is genuinely just POSIX
# (pthread, nanosleep, std::atomic), needing only Threads::Threads to
# link — unlike xbmp280_driver, this never needs CMSIS_RTOS_INCLUDE_DIR.
add_library(pbmp280_driver STATIC
    pBMP280.h
    pBMP280.cpp
    pSensorBase.h
)

target_include_directories(pbmp280_driver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(pbmp280_driver PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(pbmp280_driver PUBLIC Threads::Threads)

target_link_libraries(pbmp280_driver PUBLIC bmp280_driver)
