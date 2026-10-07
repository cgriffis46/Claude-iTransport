# isensor: SensorBase, BMP280 and their RTOS/POSIX variants, the
# SensorStateMachine every sensor driver builds on, and the drivers
# themselves, one folder each (added at the end of this file).
#
# Needs itransport's sensor_transport target, so add_subdirectory()
# itransport before this folder.

# Static libraries are written to isensor/lib rather than the build tree.
set(ISENSOR_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# --- sensor_state_machine ------------------------------------------------
# Header-only. Every driver in the sensor folders below (aht20/,
# bme280/, ...) links this rather than naming isensor/inc itself.
add_library(sensor_state_machine INTERFACE)

target_include_directories(sensor_state_machine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(sensor_state_machine INTERFACE cxx_std_17)

# --- sensor_base -------------------------------------------------------
# SensorBase.h/.cpp only need ISensorTransport.h — not the HAL/RTOS
# headers directly, since it talks to hardware exclusively through the
# ISensorTransport& it's given, never a concrete transport. Linking
# against sensor_transport (rather than duplicating an include path)
# is what actually gets ISensorTransport.h onto sensor_base's include
# path. Hardware-agnostic: unaffected by SENSOR_FW_HARDWARE.
add_library(sensor_base STATIC
    inc/SensorBase.h
    src/SensorBase.cpp
)

target_include_directories(sensor_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(sensor_base PUBLIC cxx_std_17)

set_target_properties(sensor_base PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ISENSOR_LIB_DIR})

target_link_libraries(sensor_base PUBLIC sensor_transport)

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
    inc/BMP280Sensor.h
    src/BMP280Sensor.cpp
    inc/Barometer.h
    inc/Altimeter.h
    inc/Thermometer.h
)

target_include_directories(bmp280_driver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
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
# Built whenever CMSIS_RTOS_INCLUDE_DIR is set (an STM32 build always
# sets it).
set(CMSIS_RTOS_INCLUDE_DIR "" CACHE PATH "Directory containing cmsis_os2.h")
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    add_library(xbmp280_driver STATIC
        hw/freertos/inc/xBMP280.h
        hw/freertos/src/xBMP280.cpp
        hw/freertos/inc/xSensorBase.h
    )

    target_include_directories(xbmp280_driver PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
        ${CMSIS_RTOS_INCLUDE_DIR}
    )

    target_compile_features(xbmp280_driver PUBLIC cxx_std_17)

    set_target_properties(xbmp280_driver PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ISENSOR_LIB_DIR})

    target_link_libraries(xbmp280_driver PUBLIC bmp280_driver)
endif()

# --- pbmp280_driver --------------------------------------------------------
# pBMP280 extends BMP280Sensor, overriding sleep()/startThread()/
# StopThread() with real POSIX pthread_create()/nanosleep() calls —
# everything else (the state machine itself) is inherited unchanged.
# No CMSIS-RTOS dependency at all here: this is genuinely just POSIX
# (pthread, nanosleep, std::atomic), needing only Threads::Threads to
# link — unlike xbmp280_driver, this never needs CMSIS_RTOS_INCLUDE_DIR.
add_library(pbmp280_driver STATIC
    hw/linux/inc/pBMP280.h
    hw/linux/src/pBMP280.cpp
    hw/linux/inc/pSensorBase.h
)

target_include_directories(pbmp280_driver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/hw/linux/inc
)

target_compile_features(pbmp280_driver PUBLIC cxx_std_17)

set_target_properties(pbmp280_driver PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${ISENSOR_LIB_DIR})

find_package(Threads REQUIRED)
target_link_libraries(pbmp280_driver PUBLIC Threads::Threads)

target_link_libraries(pbmp280_driver PUBLIC bmp280_driver)

# --- Sensor drivers --------------------------------------------------------
# One folder each, with its own CMakeLists.txt defining <name>_sensor.
foreach(sensor_dir
        aht20 bme280 bmp280 DS18B20 HMC6352 htu21df
        lps35hw mpl3115a2 PM25 sht31 si7021)
    add_subdirectory(${sensor_dir})
endforeach()
