# tls: HTTPS for xHttpServer. MbedTlsServer (iTls.h's iTlsServer on
# mbedTLS), WebPassword (PBKDF2 for WebAuth), TlsFreeRtos.h (locks and the
# FreeRTOS heap), config/inet_mbedtls_config.h (mbedTLS sized for an
# STM32F207), and hw/stm32/src/TlsStm32Rng.cpp (entropy from the MCU's
# RNG, compiled into the application).
#
# Added by iNetTransport/CMakeLists.txt when MBEDTLS_DIR points at an
# mbedTLS 3.6 source tree:
#   git clone --branch mbedtls-3.6.2 https://github.com/Mbed-TLS/mbedtls
include(${CMAKE_CURRENT_SOURCE_DIR}/mbedtls.cmake)

add_library(inet_tls STATIC
    inc/MbedTlsServer.h
    inc/WebPassword.h
    inc/TlsFreeRtos.h
    src/MbedTlsServer.cpp
    src/WebPassword.cpp
)
target_include_directories(inet_tls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(inet_tls PUBLIC mbedtls_inet inet_core inet_http)
target_compile_features(inet_tls PUBLIC cxx_std_17)
set_target_properties(inet_tls PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${INET_LIB_DIR})
