cmake_minimum_required(VERSION 3.16)
project(sensor_transport LANGUAGES CXX)

# --- Source layout ------------------------------------------------------
# Every module has the same shape:
#   <module>/{inc,src,lib}              platform-independent code
#   <module>/hw/<platform>/{inc,src}    stm32, freertos, arduino, linux
#
#   itransport/   transport interfaces and bus implementations
#
# The sensors live in the isensor/ folder at the top of the repository,
# a project of its own that builds itransport from here.
#
# Each folder has its own CMakeLists.txt holding its targets; this file
# only chooses the options and adds the folders.
# Sources use flat #include "Foo.h" -- each target's
# target_include_directories() is what puts the right inc/ folders on
# the include path. Static libraries are written to their module's
# lib/ folder rather than the build tree.
#
# Options (see itransport/CMakeLists.txt for the details):
#   SENSOR_FW_HARDWARE         STM32 (default), ARDUINO or HOST
#   CMSIS_RTOS_INCLUDE_DIR     directory holding cmsis_os2.h (STM32)
#   STM32_HAL_INCLUDE_DIR      directory holding the HAL headers (STM32)
#   STM32_PROJECT_INCLUDE_DIR  the CubeMX project's Core/Inc, holding main.h (STM32)
#   ARDUINO_CORE_INCLUDE_DIR   directory holding Wire.h (ARDUINO)
#   ITRANSPORT_BUILD_WIRINGPI  ON (default) needs WIRINGPI_INCLUDE_DIR
#   SENSOR_FW_BUILD_TESTS      OFF (default); ON builds the host tests
#
# Building and running the host tests on a PC:
#   cmake -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF \
#         -DSENSOR_FW_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build

option(SENSOR_FW_BUILD_TESTS "Build the host tests (no hardware, HAL or RTOS needed)" OFF)
if(SENSOR_FW_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(itransport)
