// SPDX-License-Identifier: GPL-3.0-only
#include "HmacSha256.h"

#include <mbedtls/md.h>
#include <mbedtls/platform.h>
#include <string.h>

namespace HmacSha256 {

namespace {

bool update(mbedtls_md_context_t& context, const uint8_t* data,
            size_t length) {
  return length == 0 || mbedtls_md_hmac_update(&context, data, length) == 0;
}

}  // namespace

bool compute(const uint8_t* key, size_t keyLength, const uint8_t* part1,
             size_t length1, const uint8_t* part2, size_t length2,
             const uint8_t* part3, size_t length3, uint8_t mac[kMacBytes]) {
  memset(mac, 0, kMacBytes);
  // On the unit this powers up the CryptoCell (reference counted by Mbed OS);
  // it is powered down again below so it draws nothing between connections.
  // A no-op on the host.
  if (mbedtls_platform_setup(nullptr) != 0) {
    return false;
  }
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t context;
  mbedtls_md_init(&context);
  // Allocates the hash context and HMAC pads (~300 bytes), freed below.
  const bool ok = info != nullptr && mbedtls_md_setup(&context, info, 1) == 0 &&
                  mbedtls_md_hmac_starts(&context, key, keyLength) == 0 &&
                  update(context, part1, length1) &&
                  update(context, part2, length2) &&
                  update(context, part3, length3) &&
                  mbedtls_md_hmac_finish(&context, mac) == 0;
  mbedtls_md_free(&context);
  mbedtls_platform_teardown(nullptr);
  if (!ok) {
    memset(mac, 0, kMacBytes);
  }
  return ok;
}

bool equal(const uint8_t* a, const uint8_t* b, size_t length) {
  uint8_t diff = 0;
  for (size_t i = 0; i < length; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

}  // namespace HmacSha256
