// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

// HMAC-SHA-256 (RFC 2104) for app-layer authentication (PROTOCOL.md §3), on
// Mbed TLS: the copy precompiled into the mbed core on the unit (hashing on
// the nRF52840's CryptoCell 310), Homebrew's mbedtls@3 in the host tests.
namespace HmacSha256 {

constexpr size_t kMacBytes = 32;

// HMAC over the concatenation of up to three message parts (any may be null
// with length 0), so callers need no scratch buffer. False when the crypto
// backend fails (no memory, CryptoCell not starting); mac is then zeroed and
// must not be trusted.
bool compute(const uint8_t* key, size_t keyLength, const uint8_t* part1,
             size_t length1, const uint8_t* part2, size_t length2,
             const uint8_t* part3, size_t length3, uint8_t mac[kMacBytes]);

// Constant-time comparison, for checking a proof. Mbed TLS 2.x has no public
// one (mbedtls_ct_memcmp arrived in 3.x).
bool equal(const uint8_t* a, const uint8_t* b, size_t length);

}  // namespace HmacSha256
