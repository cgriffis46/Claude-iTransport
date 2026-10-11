# winc1500: Microchip ATWINC1500 Wi-Fi driver, a class template over its
# transport.
#   inc/WincProtocol.h  the host interface's numbers (from Microchip's 19.5.2 driver)
#   inc/Winc1500.h      the class, for any iBlockTransport, driven by poll()
#   src/Winc1500.tpp    its member definitions, included by inc/Winc1500.h
#
# Added by iNetTransport/CMakeLists.txt. Needs itransport's sensor_transport
# (iBlockTransport) and inet_core (iNetDevice).
#
# WINC_REF_DIR: Arduino's WiFi101 library (git clone
# https://github.com/arduino-libraries/WiFi101), which carries Microchip's
# host driver. With host tests on, WincRef_test runs that driver against
# the simulated module the other tests use, and checks WincProtocol.h's
# numbers against its headers.
add_library(winc1500_driver INTERFACE)

target_include_directories(winc1500_driver INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_features(winc1500_driver INTERFACE cxx_std_17)

target_link_libraries(winc1500_driver INTERFACE sensor_transport inet_core)

set(WINC_REF_DIR "" CACHE PATH "Arduino's WiFi101 library (Microchip's ATWINC1500 host driver), for WincRef_test")

# --- Host tests ----------------------------------------------------------
if(SENSOR_FW_BUILD_TESTS)
    # The state machine alone, on a simulated clock, against the simulated
    # module in iNetTransport/test/sim.
    add_executable(Winc1500_test test/Winc1500_test.cpp)
    target_include_directories(Winc1500_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
    target_link_libraries(Winc1500_test PRIVATE winc1500_driver)
    add_test(NAME Winc1500_test COMMAND Winc1500_test)

    # Microchip's driver against the same simulated module.
    if(NOT WINC_REF_DIR STREQUAL "")
        enable_language(C)
        set(_ref ${WINC_REF_DIR}/src)
        if(NOT EXISTS ${_ref}/driver/source/m2m_hif.c)
            message(FATAL_ERROR "WINC_REF_DIR: no driver/source/m2m_hif.c under ${_ref}")
        endif()
        # Its uint32/sint32 are "long": 8 bytes on a 64-bit PC, which would
        # change the message layouts. A copy of its nm_bsp.h with "int",
        # found before the original.
        file(READ ${_ref}/bsp/include/nm_bsp.h _bsp)
        string(REPLACE "typedef unsigned long\tuint32;" "typedef unsigned int\tuint32;" _bsp "${_bsp}")
        string(REPLACE "typedef signed long\t\tsint32;" "typedef signed int\t\tsint32;" _bsp "${_bsp}")
        if(NOT _bsp MATCHES "typedef unsigned int\tuint32;")
            message(FATAL_ERROR "WINC_REF_DIR: nm_bsp.h's uint32 typedef not found; a different driver version?")
        endif()
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/winc_ref_override/bsp/include/nm_bsp.h "${_bsp}")
        # Its socket functions renamed: on the PC they'd replace the C
        # library's close(), connect(), send()...
        set(_rename socket=winc_socket close=winc_close connect=winc_connect send=winc_send recv=winc_recv
            bind=winc_bind listen=winc_listen accept=winc_accept sendto=winc_sendto recvfrom=winc_recvfrom
            setsockopt=winc_setsockopt getsockopt=winc_getsockopt gethostbyname=winc_gethostbyname)
        set(_refinc ${CMAKE_CURRENT_BINARY_DIR}/winc_ref_override ${_ref})
        add_library(winc_ref STATIC
            ${_ref}/driver/source/nmspi.c ${_ref}/driver/source/nmbus.c ${_ref}/driver/source/nmasic.c
            ${_ref}/driver/source/nmdrv.c ${_ref}/driver/source/m2m_hif.c ${_ref}/driver/source/m2m_wifi.c
            ${_ref}/socket/source/socket.c ${_ref}/spi_flash/source/spi_flash.c ${_ref}/common/source/nm_common.c
            test/ref/ref_app.c test/ref/ref_layout.cpp)
        target_include_directories(winc_ref PUBLIC ${_refinc})
        target_include_directories(winc_ref PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)
        target_compile_definitions(winc_ref PRIVATE ${_rename})
        target_compile_options(winc_ref PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)   # their code, as it comes
        target_compile_features(winc_ref PRIVATE cxx_std_17)

        add_executable(WincRef_test test/WincRef_test.cpp)
        target_include_directories(WincRef_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../test/sim)
        target_link_libraries(WincRef_test PRIVATE winc_ref winc1500_driver)
        add_test(NAME WincRef_test COMMAND WincRef_test)
    endif()
endif()
