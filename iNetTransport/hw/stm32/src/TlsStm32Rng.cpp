// mbedTLS's entropy from an STM32's true random number generator (the
// F2, F4, F7, L4, H7 have one), for builds with INET_TLS_HARDWARE_RNG.
// Compile it into the application, which has the CubeMX RNG handle:
// enable RNG in CubeMX (its clock comes from the 48 MHz PLL output).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "main.h"

extern RNG_HandleTypeDef hrng;

#define MBEDTLS_ERR_ENTROPY_SOURCE_FAILED -0x003C

extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, size_t len, size_t* olen) {
    size_t done = 0;
    while (done < len) {
        uint32_t r;
        if (HAL_RNG_GenerateRandomNumber(&hrng, &r) != HAL_OK) {   // seed or clock error
            *olen = done;
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        const size_t n = len - done < sizeof r ? len - done : sizeof r;
        std::memcpy(output + done, &r, n);
        done += n;
    }
    *olen = done;
    return 0;
}
