#pragma once
#include <cstddef>
#include <cstdint>
#include "WebAuth.h"

// Passwords for WebAuth: PBKDF2-HMAC-SHA256 through mbedTLS. Each
// iteration is two SHA-256 blocks, so the count sets how long a check
// takes: 20000 is about 0.1 s on a desktop and, by estimate, a second or
// so on a 120 MHz Cortex-M3. That's slow enough to make guessing from a
// stolen table expensive, and the login lockout limits guessing over
// the network.
namespace WebPassword {

// out = PBKDF2-HMAC-SHA256(password, salt, iterations), 32 bytes.
bool hash(const char* password, const uint8_t* salt, size_t saltLen, uint32_t iterations, uint8_t out[32]);

// WebAuth::Config::verify.
bool verify(const WebUser& user, const char* password, void* ctx);

}  // namespace WebPassword
