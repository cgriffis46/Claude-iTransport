# iDisplay: displays and the GUI that runs on them.
#
#   inc/, src/          display_core: iTextSurface (a grid of character
#                       cells), MonoCanvas (a 1 bit frame buffer that is
#                       also one), the 5x7 font, and iDisplayDevice, the
#                       interface every display driver provides
#   ssd1306/            SSD1306 OLED driver, a class template over its
#                       transport (I2C or 4-wire SPI), with a host test
#   gui/                idisplay_gui: screens, the screen stack, menus,
#                       fields and button debouncing. No RTOS
#   hw/freertos/        xGui, the GUI task, and xGuiButton (CMSIS-RTOS2),
#                       with a host test
#
# Needs itransport's sensor_transport and isensor's SensorStateMachine
# (for the drivers). Built on its own, this file adds itransport from
# ITRANSPORT_DIR and finds SensorStateMachine.h in ISENSOR_DIR/inc.
#
# Building and running the host tests on a PC:
#   cmake -B build -DSENSOR_FW_HARDWARE=HOST -DITRANSPORT_BUILD_WIRINGPI=OFF \
#         -DSENSOR_FW_BUILD_TESTS=ON
#   cmake --build build
#   ctest --test-dir build
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(idisplay LANGUAGES CXX)

    option(SENSOR_FW_BUILD_TESTS "Build the host tests (no hardware, HAL or RTOS needed)" OFF)
    if(SENSOR_FW_BUILD_TESTS)
        enable_testing()
    endif()
endif()

set(ITRANSPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iTransport/itransport
    CACHE PATH "The itransport folder to build against")
set(ISENSOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../isensor
    CACHE PATH "The isensor folder (for SensorStateMachine.h)")

if(NOT TARGET sensor_transport)
    add_subdirectory(${ITRANSPORT_DIR} itransport)
endif()

# isensor defines this target when it is in the same build.
if(NOT TARGET sensor_state_machine)
    add_library(sensor_state_machine INTERFACE)
    target_include_directories(sensor_state_machine INTERFACE ${ISENSOR_DIR}/inc)
    target_compile_features(sensor_state_machine INTERFACE cxx_std_17)
endif()

set(CMSIS_RTOS_INCLUDE_DIR "" CACHE PATH "Directory containing cmsis_os2.h")

# Static libraries are written to iDisplay/lib rather than the build tree.
set(IDISPLAY_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# --- display_core ----------------------------------------------------------
add_library(display_core STATIC
    inc/iTextSurface.h
    inc/MonoCanvas.h
    inc/Font5x7.h
    inc/iDisplayDevice.h
    src/iTextSurface.cpp
    src/MonoCanvas.cpp
    src/Font5x7.cpp
)
target_include_directories(display_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_features(display_core PUBLIC cxx_std_17)
set_target_properties(display_core PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${IDISPLAY_LIB_DIR})

# --- idisplay_gui ----------------------------------------------------------
add_library(idisplay_gui STATIC
    gui/inc/xGuiEvent.h
    gui/inc/xScreen.h
    gui/inc/xGuiCore.h
    gui/inc/xMenu.h
    gui/inc/xFields.h
    gui/inc/xButton.h
    gui/inc/xTime.h
    gui/src/xGuiCore.cpp
    gui/src/xMenu.cpp
    gui/src/xFields.cpp
    gui/src/xButton.cpp
    gui/src/xTime.cpp
)
target_include_directories(idisplay_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gui/inc)
target_link_libraries(idisplay_gui PUBLIC display_core)
set_target_properties(idisplay_gui PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${IDISPLAY_LIB_DIR})

# --- xgui ------------------------------------------------------------------
# The GUI task. Built whenever CMSIS_RTOS_INCLUDE_DIR is set.
if(NOT CMSIS_RTOS_INCLUDE_DIR STREQUAL "")
    add_library(xgui STATIC
        hw/freertos/inc/xGui.h
        hw/freertos/src/xGui.cpp
    )
    target_include_directories(xgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
        ${CMSIS_RTOS_INCLUDE_DIR}
    )
    target_link_libraries(xgui PUBLIC idisplay_gui)
    set_target_properties(xgui PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${IDISPLAY_LIB_DIR})
endif()

# --- Display drivers ---------------------------------------------------------
foreach(display_dir ssd1306 hd44780)
    add_subdirectory(${display_dir})
endforeach()

# --- Host tests --------------------------------------------------------------
# gui_test runs the GUI core, the widgets, the buttons and xGui over a
# simulated queue (hw/freertos/test/stub/cmsis_os2.h) and a fake display.
if(SENSOR_FW_BUILD_TESTS)
    add_executable(gui_test hw/freertos/test/gui_test.cpp hw/freertos/src/xGui.cpp)
    target_include_directories(gui_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/test/stub
        ${CMAKE_CURRENT_SOURCE_DIR}/hw/freertos/inc
    )
    target_link_libraries(gui_test PRIVATE idisplay_gui)
    add_test(NAME gui_test COMMAND gui_test)
endif()
