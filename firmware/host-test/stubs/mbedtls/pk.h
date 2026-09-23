/*
 * mbedtls/pk.h - host-test stub (backed by stubs/host_mbedtls.c).
 *
 * WHAT THE FAKE MODELS, AND WHY IT IS FAIR. db_tls.c is under test for its
 * DECISIONS about keys and certificates — shadow vs discard vs activate —
 * never for the cryptography itself (that is mbedTLS's own test suite's
 * job). So the fake makes the verdict a pure function of the PEM-looking
 * TEXT it is handed: parsing succeeds iff the right BEGIN marker is present,
 * a `pair=<tag>` line names which keypair the blob belongs to (check_pair
 * compares tags), and optional `type=rsa` / `bits=<n>` lines let a test
 * store exactly the key the strength floor must refuse. Same bytes, same
 * verdict — which is precisely the property the shadow tests lean on.
 */
#ifndef DB_HOSTTEST_MBEDTLS_PK_H
#define DB_HOSTTEST_MBEDTLS_PK_H

#include <stddef.h>

typedef enum {
    MBEDTLS_PK_NONE = 0,
    MBEDTLS_PK_RSA,
    MBEDTLS_PK_ECKEY,
    MBEDTLS_PK_ECDSA,
} mbedtls_pk_type_t;

typedef struct { int which; } mbedtls_pk_info_t;

/* Forward-declared here, defined in mbedtls/ecp.h — mirrors the real
 * pointer-holding shape closely enough that mbedtls_pk_ec() taking the
 * context BY VALUE (as the real inline does) still hands back a pointer to
 * the one shared keypair. */
struct mbedtls_ecp_keypair;

typedef struct mbedtls_pk_context {
    mbedtls_pk_type_t type;
    size_t            bits;
    char              pair_tag[32];
    struct mbedtls_ecp_keypair *ec;   /* allocated by pk_setup, freed by pk_free */
} mbedtls_pk_context;

void mbedtls_pk_init(mbedtls_pk_context *ctx);
void mbedtls_pk_free(mbedtls_pk_context *ctx);
int  mbedtls_pk_setup(mbedtls_pk_context *ctx, const mbedtls_pk_info_t *info);
const mbedtls_pk_info_t *mbedtls_pk_info_from_type(mbedtls_pk_type_t type);

int mbedtls_pk_parse_key(mbedtls_pk_context *ctx,
                         const unsigned char *key, size_t keylen,
                         const unsigned char *pwd, size_t pwdlen,
                         int (*f_rng)(void *, unsigned char *, size_t),
                         void *p_rng);
int mbedtls_pk_check_pair(const mbedtls_pk_context *pub,
                          const mbedtls_pk_context *prv,
                          int (*f_rng)(void *, unsigned char *, size_t),
                          void *p_rng);
mbedtls_pk_type_t mbedtls_pk_get_type(const mbedtls_pk_context *ctx);
size_t            mbedtls_pk_get_bitlen(const mbedtls_pk_context *ctx);

struct mbedtls_ecp_keypair *mbedtls_pk_ec(mbedtls_pk_context pk);
int mbedtls_pk_write_key_pem(const mbedtls_pk_context *ctx,
                             unsigned char *buf, size_t size);

#endif
