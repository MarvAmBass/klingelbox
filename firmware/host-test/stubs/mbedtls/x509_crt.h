/* mbedtls/x509_crt.h - host-test stub. See mbedtls/pk.h for the model.
 * Parsing keeps a heap copy of the input as `raw` (that is what the
 * fingerprint hashes) and lifts the pair tag into `pk`; the x509write half
 * records just enough (subject key, serial) for crt_pem to emit a fake
 * certificate the parser round-trips. */
#ifndef DB_HOSTTEST_MBEDTLS_X509_CRT_H
#define DB_HOSTTEST_MBEDTLS_X509_CRT_H

#include <stddef.h>

#include "mbedtls/pk.h"

/* md.h surface db_tls.c reaches through the real x509_crt.h: */
typedef int mbedtls_md_type_t;
#define MBEDTLS_MD_SHA256 6

#define MBEDTLS_X509_CRT_VERSION_3           2
#define MBEDTLS_X509_KU_DIGITAL_SIGNATURE    0x80
#define MBEDTLS_X509_KU_KEY_AGREEMENT        0x08
#define MBEDTLS_X509_NS_CERT_TYPE_SSL_SERVER 0x40

typedef struct {
    unsigned char *p;
    size_t         len;
} mbedtls_x509_buf;

typedef struct mbedtls_x509_crt {
    mbedtls_x509_buf   raw;   /* heap copy of the parsed input */
    mbedtls_pk_context pk;    /* leaf public key (pair tag only) */
} mbedtls_x509_crt;

void mbedtls_x509_crt_init(mbedtls_x509_crt *crt);
void mbedtls_x509_crt_free(mbedtls_x509_crt *crt);
int  mbedtls_x509_crt_parse(mbedtls_x509_crt *chain,
                            const unsigned char *buf, size_t buflen);

typedef struct mbedtls_x509write_cert {
    const mbedtls_pk_context *subject_key;
    unsigned char             serial[16];
    size_t                    serial_len;
} mbedtls_x509write_cert;

void mbedtls_x509write_crt_init(mbedtls_x509write_cert *ctx);
void mbedtls_x509write_crt_free(mbedtls_x509write_cert *ctx);
void mbedtls_x509write_crt_set_version(mbedtls_x509write_cert *ctx, int version);
void mbedtls_x509write_crt_set_md_alg(mbedtls_x509write_cert *ctx,
                                      mbedtls_md_type_t md_alg);
void mbedtls_x509write_crt_set_subject_key(mbedtls_x509write_cert *ctx,
                                           mbedtls_pk_context *key);
void mbedtls_x509write_crt_set_issuer_key(mbedtls_x509write_cert *ctx,
                                          mbedtls_pk_context *key);
int  mbedtls_x509write_crt_set_subject_name(mbedtls_x509write_cert *ctx,
                                            const char *subject_name);
int  mbedtls_x509write_crt_set_issuer_name(mbedtls_x509write_cert *ctx,
                                           const char *issuer_name);
int  mbedtls_x509write_crt_set_serial_raw(mbedtls_x509write_cert *ctx,
                                          unsigned char *serial, size_t len);
int  mbedtls_x509write_crt_set_validity(mbedtls_x509write_cert *ctx,
                                        const char *not_before,
                                        const char *not_after);
int  mbedtls_x509write_crt_set_basic_constraints(mbedtls_x509write_cert *ctx,
                                                 int is_ca, int max_pathlen);
int  mbedtls_x509write_crt_set_subject_key_identifier(mbedtls_x509write_cert *ctx);
int  mbedtls_x509write_crt_set_authority_key_identifier(mbedtls_x509write_cert *ctx);
int  mbedtls_x509write_crt_set_key_usage(mbedtls_x509write_cert *ctx,
                                         unsigned int key_usage);
int  mbedtls_x509write_crt_set_ns_cert_type(mbedtls_x509write_cert *ctx,
                                            unsigned char ns_cert_type);
int  mbedtls_x509write_crt_pem(mbedtls_x509write_cert *ctx,
                               unsigned char *buf, size_t size,
                               int (*f_rng)(void *, unsigned char *, size_t),
                               void *p_rng);

#endif
