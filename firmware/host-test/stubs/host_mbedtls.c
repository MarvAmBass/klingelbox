/*
 * host_mbedtls.c - the fake mbedTLS behind the stubs/mbedtls/ headers.
 *
 * CONTENT-DRIVEN, NO TEST KNOBS: every verdict is a pure function of the
 * PEM-looking text (see stubs/mbedtls/pk.h for the grammar). That is
 * deliberate — db_tls.c's shadow logic hinges on "the same stored bytes are
 * judged the same at every boot", and a fake whose verdict depended on
 * mutable test state could green-light a test while hiding exactly the
 * judgment drift the real bug (a new firmware refusing old bytes) is made
 * of. A test that wants a different verdict stores different bytes, the way
 * a different firmware version effectively does.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

#include "esp_random.h"

/* ---- text scanning -------------------------------------------------------- */

/* Copy the value of a `name=value` line into out (empty string if absent).
 * Inputs are NUL-terminated by contract (db_tls.c counts the NUL). */
static void scan_field(const char *text, const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    const char *p = strstr(text, name);
    if (!p) return;
    p += strlen(name);
    size_t i = 0;
    while (p[i] && p[i] != '\n' && p[i] != '\r' && p[i] != ' ' && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

/* ---- pk ------------------------------------------------------------------- */

void mbedtls_pk_init(mbedtls_pk_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }

void mbedtls_pk_free(mbedtls_pk_context *ctx)
{
    if (!ctx) return;
    free(ctx->ec);
    memset(ctx, 0, sizeof(*ctx));
}

static const mbedtls_pk_info_t s_eckey_info = { MBEDTLS_PK_ECKEY };

const mbedtls_pk_info_t *mbedtls_pk_info_from_type(mbedtls_pk_type_t type)
{
    return (type == MBEDTLS_PK_ECKEY) ? &s_eckey_info : NULL;
}

int mbedtls_pk_setup(mbedtls_pk_context *ctx, const mbedtls_pk_info_t *info)
{
    if (!info) return -1;
    ctx->type = (mbedtls_pk_type_t)info->which;
    ctx->ec = calloc(1, sizeof(*ctx->ec));
    return ctx->ec ? 0 : -1;
}

int mbedtls_pk_parse_key(mbedtls_pk_context *ctx,
                         const unsigned char *key, size_t keylen,
                         const unsigned char *pwd, size_t pwdlen,
                         int (*f_rng)(void *, unsigned char *, size_t),
                         void *p_rng)
{
    (void)keylen; (void)pwd; (void)pwdlen; (void)f_rng; (void)p_rng;
    const char *text = (const char *)key;
    if (!strstr(text, "-----BEGIN PRIVATE KEY-----") &&
        !strstr(text, "-----BEGIN EC PRIVATE KEY-----") &&
        !strstr(text, "-----BEGIN RSA PRIVATE KEY-----"))
        return -0x3d00;   /* any nonzero: "did not parse" */

    char field[32];
    scan_field(text, "pair=", ctx->pair_tag, sizeof(ctx->pair_tag));
    scan_field(text, "type=", field, sizeof(field));
    ctx->type = (strcmp(field, "rsa") == 0) ? MBEDTLS_PK_RSA : MBEDTLS_PK_ECKEY;
    scan_field(text, "bits=", field, sizeof(field));
    ctx->bits = (field[0] != '\0')
        ? (size_t)strtoul(field, NULL, 10)
        : (ctx->type == MBEDTLS_PK_RSA ? 2048u : 256u);
    return 0;
}

int mbedtls_pk_check_pair(const mbedtls_pk_context *pub,
                          const mbedtls_pk_context *prv,
                          int (*f_rng)(void *, unsigned char *, size_t),
                          void *p_rng)
{
    (void)f_rng; (void)p_rng;
    return strcmp(pub->pair_tag, prv->pair_tag) == 0 ? 0 : -0x6e00;
}

mbedtls_pk_type_t mbedtls_pk_get_type(const mbedtls_pk_context *ctx)
{
    return ctx->type;
}

size_t mbedtls_pk_get_bitlen(const mbedtls_pk_context *ctx)
{
    return ctx->bits;
}

mbedtls_ecp_keypair *mbedtls_pk_ec(mbedtls_pk_context pk)
{
    return pk.ec;   /* the copy still carries the shared pointer */
}

int mbedtls_pk_write_key_pem(const mbedtls_pk_context *ctx,
                             unsigned char *buf, size_t size)
{
    int gen = ctx->ec ? ctx->ec->gen_id : 0;
    int n = snprintf((char *)buf, size,
                     "-----BEGIN EC PRIVATE KEY-----\n"
                     "pair=gen%d\n"
                     "-----END EC PRIVATE KEY-----\n", gen);
    return (n > 0 && (size_t)n < size) ? 0 : -1;
}

/* ---- ecp ------------------------------------------------------------------ */

int mbedtls_ecp_gen_key(mbedtls_ecp_group_id grp_id, mbedtls_ecp_keypair *key,
                        int (*f_rng)(void *, unsigned char *, size_t),
                        void *p_rng)
{
    (void)grp_id; (void)f_rng; (void)p_rng;
    /* A fresh generation id per keygen: two generated identities must never
     * compare equal, exactly like two real keygens. */
    static int s_generation;
    key->gen_id = ++s_generation;
    return 0;
}

/* ---- drbg / entropy ------------------------------------------------------- */

void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *ctx) { (void)ctx; }
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *ctx) { (void)ctx; }

int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *ctx,
                          int (*f_entropy)(void *, unsigned char *, size_t),
                          void *p_entropy,
                          const unsigned char *custom, size_t len)
{
    (void)ctx; (void)f_entropy; (void)p_entropy; (void)custom; (void)len;
    return 0;
}

int mbedtls_ctr_drbg_random(void *p_rng, unsigned char *output, size_t len)
{
    (void)p_rng;
    esp_fill_random(output, len);
    return 0;
}

void mbedtls_entropy_init(mbedtls_entropy_context *ctx) { (void)ctx; }
void mbedtls_entropy_free(mbedtls_entropy_context *ctx) { (void)ctx; }

int mbedtls_entropy_add_source(mbedtls_entropy_context *ctx,
                               int (*f_source)(void *, unsigned char *,
                                               size_t, size_t *),
                               void *p_source, size_t threshold, int strong)
{
    (void)ctx; (void)f_source; (void)p_source; (void)threshold; (void)strong;
    return 0;
}

int mbedtls_entropy_func(void *data, unsigned char *output, size_t len)
{
    (void)data;
    esp_fill_random(output, len);
    return 0;
}

/* ---- sha256 (a mixer, not SHA — see the stub header) ---------------------- */

int mbedtls_sha256(const unsigned char *input, size_t ilen,
                   unsigned char output[32], int is224)
{
    (void)is224;
    /* FNV-1a folded out to 32 bytes: stable for equal input, different for
     * any input the tests actually distinguish. */
    unsigned long long h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < ilen; i++) {
        h ^= input[i];
        h *= 0x100000001b3ull;
    }
    for (int i = 0; i < 32; i++) {
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull; h ^= h >> 29;
        output[i] = (unsigned char)h;
    }
    return 0;
}

/* ---- x509 parse ----------------------------------------------------------- */

void mbedtls_x509_crt_init(mbedtls_x509_crt *crt) { memset(crt, 0, sizeof(*crt)); }

void mbedtls_x509_crt_free(mbedtls_x509_crt *crt)
{
    if (!crt) return;
    free(crt->raw.p);
    mbedtls_pk_free(&crt->pk);
    memset(crt, 0, sizeof(*crt));
}

int mbedtls_x509_crt_parse(mbedtls_x509_crt *chain,
                           const unsigned char *buf, size_t buflen)
{
    const char *text = (const char *)buf;
    if (!strstr(text, "-----BEGIN CERTIFICATE-----"))
        return -0x2180;

    /* `raw` is what the fingerprint hashes: the bytes themselves, so a
     * different certificate text yields a different fingerprint. */
    chain->raw.p = malloc(buflen ? buflen : 1);
    if (!chain->raw.p) return -1;
    memcpy(chain->raw.p, buf, buflen);
    chain->raw.len = buflen;
    scan_field(text, "pair=", chain->pk.pair_tag, sizeof(chain->pk.pair_tag));
    return 0;
}

/* ---- x509 write ----------------------------------------------------------- */

void mbedtls_x509write_crt_init(mbedtls_x509write_cert *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}
void mbedtls_x509write_crt_free(mbedtls_x509write_cert *ctx) { (void)ctx; }

void mbedtls_x509write_crt_set_version(mbedtls_x509write_cert *ctx, int version)
{
    (void)ctx; (void)version;
}
void mbedtls_x509write_crt_set_md_alg(mbedtls_x509write_cert *ctx,
                                      mbedtls_md_type_t md_alg)
{
    (void)ctx; (void)md_alg;
}
void mbedtls_x509write_crt_set_subject_key(mbedtls_x509write_cert *ctx,
                                           mbedtls_pk_context *key)
{
    ctx->subject_key = key;
}
void mbedtls_x509write_crt_set_issuer_key(mbedtls_x509write_cert *ctx,
                                          mbedtls_pk_context *key)
{
    (void)ctx; (void)key;
}
int mbedtls_x509write_crt_set_subject_name(mbedtls_x509write_cert *ctx,
                                           const char *subject_name)
{
    (void)ctx; (void)subject_name;
    return 0;
}
int mbedtls_x509write_crt_set_issuer_name(mbedtls_x509write_cert *ctx,
                                          const char *issuer_name)
{
    (void)ctx; (void)issuer_name;
    return 0;
}
int mbedtls_x509write_crt_set_serial_raw(mbedtls_x509write_cert *ctx,
                                         unsigned char *serial, size_t len)
{
    if (len > sizeof(ctx->serial)) len = sizeof(ctx->serial);
    memcpy(ctx->serial, serial, len);
    ctx->serial_len = len;
    return 0;
}
int mbedtls_x509write_crt_set_validity(mbedtls_x509write_cert *ctx,
                                       const char *not_before,
                                       const char *not_after)
{
    (void)ctx; (void)not_before; (void)not_after;
    return 0;
}
int mbedtls_x509write_crt_set_basic_constraints(mbedtls_x509write_cert *ctx,
                                                int is_ca, int max_pathlen)
{
    (void)ctx; (void)is_ca; (void)max_pathlen;
    return 0;
}
int mbedtls_x509write_crt_set_subject_key_identifier(mbedtls_x509write_cert *ctx)
{
    (void)ctx;
    return 0;
}
int mbedtls_x509write_crt_set_authority_key_identifier(mbedtls_x509write_cert *ctx)
{
    (void)ctx;
    return 0;
}
int mbedtls_x509write_crt_set_key_usage(mbedtls_x509write_cert *ctx,
                                        unsigned int key_usage)
{
    (void)ctx; (void)key_usage;
    return 0;
}
int mbedtls_x509write_crt_set_ns_cert_type(mbedtls_x509write_cert *ctx,
                                           unsigned char ns_cert_type)
{
    (void)ctx; (void)ns_cert_type;
    return 0;
}

int mbedtls_x509write_crt_pem(mbedtls_x509write_cert *ctx,
                              unsigned char *buf, size_t size,
                              int (*f_rng)(void *, unsigned char *, size_t),
                              void *p_rng)
{
    (void)f_rng; (void)p_rng;
    int gen = (ctx->subject_key && ctx->subject_key->ec)
        ? ctx->subject_key->ec->gen_id : 0;
    char serial_hex[2 * sizeof(ctx->serial) + 1];
    for (size_t i = 0; i < ctx->serial_len; i++)
        snprintf(serial_hex + 2 * i, 3, "%02x", ctx->serial[i]);
    serial_hex[2 * ctx->serial_len] = '\0';
    int n = snprintf((char *)buf, size,
                     "-----BEGIN CERTIFICATE-----\n"
                     "pair=gen%d\n"
                     "serial=%s\n"
                     "-----END CERTIFICATE-----\n", gen, serial_hex);
    return (n > 0 && (size_t)n < size) ? 0 : -1;
}
