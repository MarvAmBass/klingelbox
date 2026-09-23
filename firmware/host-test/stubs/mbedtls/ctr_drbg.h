/* mbedtls/ctr_drbg.h - host-test stub. The DRBG exists in db_tls.c only to
 * satisfy mbedTLS 3.x's RNG-callback plumbing; the fake seeds trivially and
 * draws from the deterministic host RNG. */
#ifndef DB_HOSTTEST_MBEDTLS_CTR_DRBG_H
#define DB_HOSTTEST_MBEDTLS_CTR_DRBG_H

#include <stddef.h>

typedef struct { int dummy; } mbedtls_ctr_drbg_context;

void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *ctx);
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *ctx);
int  mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *ctx,
                           int (*f_entropy)(void *, unsigned char *, size_t),
                           void *p_entropy,
                           const unsigned char *custom, size_t len);
int  mbedtls_ctr_drbg_random(void *p_rng, unsigned char *output, size_t len);

#endif
