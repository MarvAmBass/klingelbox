/* mbedtls/sha256.h - host-test stub. NOT SHA-256: a deterministic 32-byte
 * mixer. The tests assert that fingerprints are stable for the same bytes
 * and different for different bytes — never that they match a published
 * vector (pw_hash's real SHA-256 is pinned to vectors in test_http_auth). */
#ifndef DB_HOSTTEST_MBEDTLS_SHA256_H
#define DB_HOSTTEST_MBEDTLS_SHA256_H

#include <stddef.h>

int mbedtls_sha256(const unsigned char *input, size_t ilen,
                   unsigned char output[32], int is224);

#endif
