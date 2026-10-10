# Monocypher 4.0.2 (vendored)

Ed25519 for MeshCore identities: `monocypher.c`/`.h` and the optional
`monocypher-ed25519.c`/`.h` (SHA-512 based Ed25519, RFC 8032), copied
unchanged from https://github.com/LoupVaillant/Monocypher, tag 4.0.2
(commit 0d85f98c9d9b0227e42cf795cb527dff372b40a4).

Dual licensed, BSD-2-Clause or CC0-1.0 (see `LICENCE.md`). No heap; C99.

Used through `iRadio/meshcore/inc/MeshIdentity.h` only. Its Ed25519 keys
and signatures were checked against the orlp/ed25519 code MeshCore itself
uses (`iRadio/test/meshcore_crypto_test.cpp`).
