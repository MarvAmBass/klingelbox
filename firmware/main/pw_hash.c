/* pw_hash.c - see pw_hash.h. Pure C99, no framework includes: this file is
 * compiled unmodified into host-test/test_http_auth, where every primitive
 * is pinned against its published test vectors. */
#include "pw_hash.h"

#include <string.h>

/* ---------------------------------------------------------------- SHA-256
 *
 * Straight FIPS 180-4. No lookup tables beyond the round constants, no
 * data-dependent branches — the compression runs the same instructions for
 * every input, which is exactly what a password hash wants.
 */
typedef struct {
    uint32_t h[8];
    uint64_t total;          /* message bytes processed so far */
    uint8_t  buf[64];
    size_t   fill;
} sha256_ctx_t;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(sha256_ctx_t *c, const uint8_t p[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx_t *c)
{
    static const uint32_t H0[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(c->h, H0, sizeof(c->h));
    c->total = 0;
    c->fill = 0;
}

static void sha256_update(sha256_ctx_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    if (c->fill) {
        size_t take = 64 - c->fill;
        if (take > len) take = len;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        len -= take;
        if (c->fill == 64) {
            sha256_block(c, c->buf);
            c->fill = 0;
        }
    }
    while (len >= 64) {
        sha256_block(c, p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(c->buf, p, len);
        c->fill = len;
    }
}

static void sha256_final(sha256_ctx_t *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    static const uint8_t zeros[64] = { 0 };
    size_t rem = (size_t)(c->total % 64);
    sha256_update(c, zeros, (rem <= 56) ? (56 - rem) : (120 - rem));
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(c->h[i]);
    }
}

void db_pw_sha256(const void *data, size_t len, uint8_t out[DB_PW_HASH_LEN])
{
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ------------------------------------------------------------ HMAC-SHA256
 *
 * Kept as precomputable inner/outer states rather than the textbook
 * two-pass form, because PBKDF2 below calls HMAC with the SAME key
 * thousands of times: hashing the padded key once per state and cloning
 * costs 2 compressions per iteration instead of 4.
 */
typedef struct {
    sha256_ctx_t inner;      /* state after ipad-padded key */
    sha256_ctx_t outer;      /* state after opad-padded key */
} hmac_key_t;

static void hmac_key_init(hmac_key_t *k, const void *key, size_t key_len)
{
    uint8_t kb[64] = { 0 };
    if (key_len > 64)
        db_pw_sha256(key, key_len, kb);   /* RFC 2104: long keys are hashed */
    else
        memcpy(kb, key, key_len);

    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x36;
    sha256_init(&k->inner);
    sha256_update(&k->inner, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = kb[i] ^ 0x5c;
    sha256_init(&k->outer);
    sha256_update(&k->outer, pad, 64);

    memset(kb, 0, sizeof(kb));    /* the key is the password — wipe it */
    memset(pad, 0, sizeof(pad));
}

/* Two-part message so PBKDF2's U1 = PRF(P, salt || INT(1)) needs no
 * concatenation buffer (and no salt-length ceiling). */
static void hmac_run2(const hmac_key_t *k, const void *m1, size_t l1,
                      const void *m2, size_t l2, uint8_t out[32])
{
    sha256_ctx_t c = k->inner;
    sha256_update(&c, m1, l1);
    if (l2) sha256_update(&c, m2, l2);
    uint8_t ih[32];
    sha256_final(&c, ih);
    c = k->outer;
    sha256_update(&c, ih, 32);
    sha256_final(&c, out);
}

static void hmac_run(const hmac_key_t *k, const void *msg, size_t msg_len,
                     uint8_t out[32])
{
    hmac_run2(k, msg, msg_len, NULL, 0, out);
}

void db_pw_hmac_sha256(const void *key, size_t key_len,
                       const void *msg, size_t msg_len,
                       uint8_t out[DB_PW_HASH_LEN])
{
    hmac_key_t k;
    hmac_key_init(&k, key, key_len);
    hmac_run(&k, msg, msg_len, out);
    memset(&k, 0, sizeof(k));
}

/* ---------------------------------------------------------------- PBKDF2 */

void db_pw_pbkdf2(const void *password, size_t pw_len,
                  const uint8_t *salt, size_t salt_len,
                  uint32_t iters, uint8_t out[DB_PW_HASH_LEN])
{
    /* One output block (32 bytes = one SHA-256) is all any caller needs, so
     * the outer loop of RFC 8018 collapses to block index 1. */
    hmac_key_t k;
    hmac_key_init(&k, password, pw_len);

    static const uint8_t int1[4] = { 0, 0, 0, 1 };   /* INT(1), big-endian */
    uint8_t u[32];
    hmac_run2(&k, salt, salt_len, int1, 4, u);       /* U1 */
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iters; i++) {
        hmac_run(&k, u, 32, u);            /* U(i+1) = PRF(P, U(i)) */
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
    }

    memset(&k, 0, sizeof(k));
    memset(u, 0, sizeof(u));
}

void db_pw_rec_from_plaintext(db_pw_rec_t *rec, const char *password,
                              const uint8_t salt[DB_PW_SALT_LEN],
                              uint32_t iters)
{
    memset(rec, 0, sizeof(*rec));
    rec->iters = iters;
    memcpy(rec->salt, salt, DB_PW_SALT_LEN);
    db_pw_pbkdf2(password, strlen(password),
                 rec->salt, DB_PW_SALT_LEN, iters, rec->hash);
}

uint32_t db_pw_pick_iterations(uint32_t probe_iters, uint64_t probe_us)
{
    if (!probe_iters || !probe_us)
        return DB_PW_ITERS_MIN;   /* unmeasurable probe: see pw_hash.h */

    /* iters = target_us / (probe_us / probe_iters), rearranged so the
     * division happens last and integer truncation cannot zero the rate. */
    uint64_t target_us = (uint64_t)DB_PW_TARGET_VERIFY_MS * 1000u;
    uint64_t iters = (target_us * probe_iters) / probe_us;

    if (iters < DB_PW_ITERS_MIN) return DB_PW_ITERS_MIN;
    if (iters > DB_PW_ITERS_MAX) return DB_PW_ITERS_MAX;
    return (uint32_t)iters;
}
