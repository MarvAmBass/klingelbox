/* mbedtls/entropy.h - host-test stub. Sources are accepted and ignored; the
 * fake DRBG never needs real entropy. */
#ifndef DB_HOSTTEST_MBEDTLS_ENTROPY_H
#define DB_HOSTTEST_MBEDTLS_ENTROPY_H

#include <stddef.h>

#define MBEDTLS_ENTROPY_SOURCE_STRONG 1

typedef struct { int dummy; } mbedtls_entropy_context;

void mbedtls_entropy_init(mbedtls_entropy_context *ctx);
void mbedtls_entropy_free(mbedtls_entropy_context *ctx);
int  mbedtls_entropy_add_source(mbedtls_entropy_context *ctx,
                                int (*f_source)(void *, unsigned char *,
                                                size_t, size_t *),
                                void *p_source, size_t threshold, int strong);
int  mbedtls_entropy_func(void *data, unsigned char *output, size_t len);

#endif
