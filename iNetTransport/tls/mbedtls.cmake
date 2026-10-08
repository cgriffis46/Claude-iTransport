# mbedtls_inet: mbedTLS built from its source tree (MBEDTLS_DIR, 3.6) with
# tls/config/inet_mbedtls_config.h. Included by tls/CMakeLists.txt and by
# PLCTransport; defines the target once.
#   INET_TLS_HARDWARE_RNG  ON for a target whose entropy is
#                          mbedtls_hardware_poll() (hw/stm32/src/TlsStm32Rng.cpp)
if(NOT TARGET mbedtls_inet)
    enable_language(C)
    option(INET_TLS_HARDWARE_RNG "mbedTLS entropy from mbedtls_hardware_poll() only" OFF)
    file(GLOB MBEDTLS_INET_SOURCES ${MBEDTLS_DIR}/library/*.c)
    add_library(mbedtls_inet STATIC ${MBEDTLS_INET_SOURCES})
    target_include_directories(mbedtls_inet PUBLIC ${MBEDTLS_DIR}/include ${CMAKE_CURRENT_LIST_DIR}/config
                                            PRIVATE ${MBEDTLS_DIR}/library)
    target_compile_definitions(mbedtls_inet PUBLIC "MBEDTLS_CONFIG_FILE=\"inet_mbedtls_config.h\"")
    if(INET_TLS_HARDWARE_RNG)
        target_compile_definitions(mbedtls_inet PUBLIC INET_TLS_HARDWARE_RNG)
    endif()
endif()
