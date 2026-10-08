#include "WebPassword.h"
#include <cstring>
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

bool WebPassword::hash(const char* password, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                       uint8_t out[32]) {
    if (password == nullptr || iterations == 0) return false;
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password),
                                         std::strlen(password), salt, saltLen, iterations, 32, out) == 0;
}

bool WebPassword::verify(const WebUser& user, const char* password, void*) {
    uint8_t h[32];
    if (!hash(password, user.salt, sizeof user.salt, user.iterations, h)) return false;
    uint8_t d = 0;
    for (size_t i = 0; i < sizeof h; ++i) d |= static_cast<uint8_t>(h[i] ^ user.hash[i]);
    std::memset(h, 0, sizeof h);
    return d == 0;
}
