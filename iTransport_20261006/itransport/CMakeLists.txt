# itransport: transport interfaces and bus implementations.
#
# Pulled in with add_subdirectory() — by iTransport_20261006's own
# top-level CMakeLists.txt, or by another project that wants the
# transports, e.g. isensor/ or safeTransport_20261004:
#
#   add_subdirectory(../iTransport_20261006/itransport itransport)
#
# Every option and cache variable it needs is declared here, so it
# does not depend on which project includes it.
#
# Sources use flat #include "Foo.h" — target_include_directories()
# below is what puts the right inc/ folders on the include path.

# Static libraries are written to itransport/lib rather than the build tree.
set(ITRANSPORT_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# --- Hardware platform selection ----------------------------------------
# Selects which concrete transport implementation gets built:
#   STM32    -> Stm32HalI2CTransport, Stm32HalSPITransport, and their interrupt callbacks
#               (real CMSIS-RTOS2 mutex + notification, via FreeRtosTransport<TBus>)
#   ARDUINO  -> ArduinoWireTransport, for ANY Arduino-API target — including
#               a plain AVR Uno/Mega/Nano with no RTOS at all. Derives from
#               I2CTransport directly, not FreeRtosTransport<I2CTransport>; its
#               busy-wait/delay(1) fallback is what makes that possible.
#   HOST     -> no hardware transport at all: only the platform-independent
#               libraries, e.g. for building the host tests on a PC.
#
#   cmake -B build -DSENSOR_FW_HARDWARE=ARDUINO ...
#
# Every hardware-specific source file carries its platform in the
# filename (Stm32*, Arduino*) precisely so it's obvious at a glance
# which files this flag controls and which (ISensorTransport.h,
# I2CTransport, SPITransport, SensorBase, BMP280Sensor) are shared.
set(SENSOR_FW_HARDWARE "STM32" CACHE STRING
    "Target hardware platform for the transport implementation: STM32, ARDUINO or HOST")
set_property(CACHE SENSOR_FW_HARDWARE PROPERTY STRINGS STM32 ARDUINO HOST)

if(NOT SENSOR_FW_HARDWARE MATCHES "^(STM32|ARDUINO|HOST)$")
    message(FATAL_ERROR
        "SENSOR_FW_HARDWARE is '${SENSOR_FW_HARDWARE}' — expected STM32, ARDUINO or HOST.\n"
        "  cmake -B build -DSENSOR_FW_HARDWARE=STM32 ...\n"
        "  cmake -B build -DSENSOR_FW_HARDWARE=ARDUINO ...\n"
        "  cmake -B build -DSENSOR_FW_HARDWARE=HOST ...")
endif()

option(ITRANSPORT_BUILD_WIRINGPI
    "Build WiringPiTransportLibrary (Raspberry Pi; needs WIRINGPI_INCLUDE_DIR)" ON)
option(SENSOR_FW_BUILD_TESTS
    "Build the host tests (no hardware, HAL or RTOS needed)" OFF)

# --- sensor_transport ----------------------------------------------------
# I2CTransport/SPITransport are now fully hardware- AND platform-agnostic:
# no STM32 types, no RTOS header, nothing. Every real primitive
# (mutex acquire/release, transfer-completion wait) is a virtual hook
# with a working, if less efficient, default — see I2CTransport.h.
add_library(sensor_transport STATIC
    inc/ISensorTransport.h
    inc/iTransport.h
    inc/BusTransport.h
    src/BusTransport.cpp
    inc/I2CTransport.h
    src/I2CTransport.cpp
    inc/SPITransport.h
    src/SPITransport.cpp
    inc/iTransportOneWire.h
    inc/OneWireUartTransport.h
    src/OneWireUartTransport.cpp
)

target_include_directories(sensor_transport PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(sensor_transport PUBLIC cxx_std_17)

set_target_properties(sensor_transport PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ITRANSPORT_LIB_DIR})

# --- Hardware transport library -----------------------------------------
# At most one of these two blocks runs, based on SENSOR_FW_HARDWARE.
# Either way, HARDWARE_TRANSPORT_LIBRARY ends up set to whichever
# target was actually built, so anything further down (or a top-level
# project) can write target_link_libraries(app PUBLIC
# ${HARDWARE_TRANSPORT_LIBRARY}) without caring which hardware it is.
# For HOST it stays empty.
set(HARDWARE_TRANSPORT_LIBRARY "")

if(SENSOR_FW_HARDWARE STREQUAL "STM32")

    # --- CMSIS-RTOS2 header ----------------------------------------------
    # FreeRtosTransport<TBus> calls osMutexAcquire()/osThreadFlagsWait() —
    # a real RTOS dependency, scoped to this library rather than the
    # whole build. sensor_transport itself needs no RTOS at all.
    set(CMSIS_RTOS_INCLUDE_DIR "" CACHE PATH "Directory containing cmsis_os2.h")
    if(CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR
            "CMSIS_RTOS_INCLUDE_DIR is not set. Point it at the directory "
            "containing cmsis_os2.h, e.g.:\n"
            "  cmake -B build -DCMSIS_RTOS_INCLUDE_DIR=/path/to/Middlewares/.../CMSIS_RTOS_V2 ...\n"
            "(Needed by Stm32TransportLibrary's FreeRtosTransport — NOT by\n"
            " sensor_transport itself, which has no RTOS dependency.)")
    endif()

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

    # The STM32 transport headers include the project's own main.h
    # (which pulls in the right HAL header for the target family), so
    # the directory holding main.h must be on the include path too.
    set(STM32_PROJECT_INCLUDE_DIR "" CACHE PATH
        "Directory holding the CubeMX project's main.h and *_hal_conf.h (Core/Inc)")
    if(STM32_PROJECT_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR
            "STM32_PROJECT_INCLUDE_DIR is not set. Point it at the CubeMX "
            "project's Core/Inc, the directory containing main.h, e.g.:\n"
            "  cmake -B build -DSENSOR_FW_HARDWARE=STM32 -DSTM32_PROJECT_INCLUDE_DIR=/path/to/project/Core/Inc ...")
    endif()

    # FreeRtosTransport<TBus> (header-only template) supplies the real
    # osMutexAcquire()/osMutexRelease()/osThreadFlagsSet()/
    # osThreadFlagsWait() calls that BusTransport's default hooks are
    # RTOS-free fallbacks for, once for I2C and SPI alike.
    # Stm32HalI2CTransport and Stm32HalSPITransport derive from
    # FreeRtosTransport<I2CTransport>/FreeRtosTransport<SPITransport>
    # to get real cross-task exclusion and to be woken by the bus
    # interrupt, rather than the polling/no-op defaults. Needs
    # CMSIS_RTOS_INCLUDE_DIR on top of STM32_HAL_INCLUDE_DIR.
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
        hw/freertos/inc/FreeRtosTransport.h
        hw/stm32/inc/Stm32HalI2CTransport.h
        hw/stm32/src/Stm32HalI2CTransport.cpp
        hw/stm32/inc/Stm32HalSPITransport.h
        hw/stm32/src/Stm32HalSPITransport.cpp
        hw/stm32/src/Stm32I2CItCallbacks.cpp
        hw/stm32/src/Stm32SpiItCallbacks.cpp
        hw/stm32/inc/Stm32HalOneWireTransport.h
        hw/stm32/src/Stm32HalOneWireTransport.cpp
        hw/stm32/inc/Stm32HalUartTransport.h
        hw/stm32/src/Stm32HalUartTransport.cpp
        hw/stm32/src/Stm32UartItCallbacks.cpp
    )

    target_include_directories(Stm32TransportLibrary PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/stm32/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
        ${STM32_PROJECT_INCLUDE_DIR}
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
        hw/arduino/inc/ArduinoWireTransport.h
        hw/arduino/src/ArduinoWireTransport.cpp
    )

    target_include_directories(ArduinoTransportLibrary PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/arduino/inc
        ${ARDUINO_CORE_INCLUDE_DIR}
    )

    target_compile_features(ArduinoTransportLibrary PUBLIC cxx_std_17)

    set_target_properties(ArduinoTransportLibrary PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ITRANSPORT_LIB_DIR})

    target_link_libraries(ArduinoTransportLibrary PUBLIC sensor_transport)

    set(HARDWARE_TRANSPORT_LIBRARY ArduinoTransportLibrary)

endif()

# Visible to whichever project add_subdirectory()'d this one.
set(HARDWARE_TRANSPORT_LIBRARY ${HARDWARE_TRANSPORT_LIBRARY} PARENT_SCOPE)

# --- WiringPiTransportLibrary --------------------------------------------
# WiringPiTransport/WiringPiI2cTransport extend ISensorTransport
# directly, not I2CTransport — WiringPi's calls are all synchronous,
# so none of I2CTransport's mutex-arbitration/transfer-notification
# machinery would do anything useful here (see WiringPiTransport.h).
# Not gated behind SENSOR_FW_HARDWARE: this isn't competing with the
# STM32/ARDUINO choice above for the same role, it's a separate
# concrete option a Raspberry Pi project links against directly.
# Turn it off with -DITRANSPORT_BUILD_WIRINGPI=OFF when there is no
# WiringPi to build against.
if(ITRANSPORT_BUILD_WIRINGPI)
    set(WIRINGPI_INCLUDE_DIR "" CACHE PATH "Directory containing wiringPiI2C.h (and the rest of WiringPi's headers)")
    if(WIRINGPI_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR
            "WIRINGPI_INCLUDE_DIR is not set. Point it at the directory "
            "containing wiringPiI2C.h, e.g.:\n"
            "  cmake -B build -DWIRINGPI_INCLUDE_DIR=/usr/include ...\n"
            "or leave WiringPiTransportLibrary out with -DITRANSPORT_BUILD_WIRINGPI=OFF.")
    endif()

    add_library(WiringPiTransportLibrary STATIC
        hw/linux/inc/IFileTransport.h
        hw/linux/src/IFileTransport.cpp
        hw/linux/inc/WiringPiTransport.h
        hw/linux/inc/WiringPiI2cTransport.h
        hw/linux/src/WiringPiI2cTransport.cpp
    )

    target_include_directories(WiringPiTransportLibrary PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/linux/inc
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
endif()

# --- Host tests ----------------------------------------------------------
# Each test fakes only the hardware calls (stub/ for CMSIS-RTOS2,
# stub_hal/ for the STM32 HAL UART), so they build and run on a PC
# whatever SENSOR_FW_HARDWARE is set to. Run them with ctest.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(itransport_test test/itransport_test.cpp)
    target_link_libraries(itransport_test PRIVATE sensor_transport)
    add_test(NAME itransport_test COMMAND itransport_test)

    add_executable(onewire_test test/onewire_test.cpp)
    target_link_libraries(onewire_test PRIVATE sensor_transport)
    add_test(NAME onewire_test COMMAND onewire_test)

    add_executable(freertos_transport_test test/freertos_transport_test.cpp)
    target_include_directories(freertos_transport_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/test/stub
    )
    target_link_libraries(freertos_transport_test PRIVATE sensor_transport)
    add_test(NAME freertos_transport_test COMMAND freertos_transport_test)

    # Builds the real Stm32HalUartTransport/Stm32UartItCallbacks sources
    # against the simulated HAL in stub_hal/.
    add_executable(stm32_uart_transport_test
        test/stm32_uart_transport_test.cpp
        test/stub_hal/fake_hal.cpp
        hw/stm32/src/Stm32HalUartTransport.cpp
        hw/stm32/src/Stm32UartItCallbacks.cpp
    )
    target_include_directories(stm32_uart_transport_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/test/stub_hal
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/stm32/inc
    )
    target_link_libraries(stm32_uart_transport_test PRIVATE sensor_transport)
    add_test(NAME stm32_uart_transport_test COMMAND stm32_uart_transport_test)
endif()
