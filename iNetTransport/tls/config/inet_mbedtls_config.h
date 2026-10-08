// mbedTLS 3.6 configuration for iNetTransport's HTTPS server: just what a
// TLS 1.2 server with an ECDSA P-256 certificate needs, sized for an
// STM32F207 (Cortex-M3, 128 KB RAM). Built with
//   -DMBEDTLS_CONFIG_FILE="\"inet_mbedtls_config.h\""
//
// Browsers all speak TLS 1.2 with ECDHE-ECDSA-AES128-GCM-SHA256, so that
// (and its AES-256 and CHACHA20 cousins) is all that's offered. TLS 1.3
// would need mbedTLS's PSA crypto as well: more flash, no gain here.
//
// On the target, define INET_TLS_HARDWARE_RNG: entropy then comes only
// from mbedtls_hardware_poll(), which the application provides from the
// MCU's true random number generator (the F207 has one; see
// TlsStm32Rng.h). On a PC the operating system's entropy is used.
#pragma once

// ---- system ----
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY            // mbedtls_platform_set_calloc_free(): the FreeRTOS heap
#if defined(INET_TLS_HARDWARE_RNG)
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#endif
// No time() on the target: certificate dates aren't checked by the server
// (it doesn't verify client certificates), and ticket keys don't rotate.

// ---- TLS ----
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_SESSION_TICKETS         // a returning browser skips the ECC work
#define MBEDTLS_SSL_TICKET_C
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
// A browser may send records of up to 16 KB; ours are at most 4 KB.
#define MBEDTLS_SSL_IN_CONTENT_LEN          16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN         4096

// ---- certificates and keys ----
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

// ---- public-key crypto ----
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC         // signatures don't depend on the RNG's quality
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED   // browsers' first choice for ECDHE
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECP_WINDOW_SIZE             4
#define MBEDTLS_ECP_FIXED_POINT_OPTIM       1

// ---- symmetric crypto and hashes ----
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES              // tables in flash, not 8 KB of RAM
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_PKCS5_C                     // PBKDF2, for the web login's password hashes

// ---- randomness ----
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
