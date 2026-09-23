/* http_auth.c - see http_auth.h. Pure C, no framework includes: this file is
 * compiled unmodified into host-test/test_http_auth. */
#include "http_auth.h"

#include <string.h>

/* "admin:" + password. */
#define AUTH_USER_PREFIX     "admin:"
#define AUTH_USER_PREFIX_LEN 6

/* Widest credential we ever accept: "admin:" + a DB_AUTH_PASS_MAX password. */
#define AUTH_CRED_MAX (AUTH_USER_PREFIX_LEN + DB_AUTH_PASS_MAX)

/* Decoded base64 payload buffer: the credential plus slack, so a guess a few
 * bytes over the limit still decodes far enough to be REFUSED by the length
 * rule below rather than erroring out of the decoder on capacity. */
#define AUTH_DECODE_MAX (AUTH_CRED_MAX + 32)

#ifdef DB_HOSTTEST
void (*db_auth_test_after_derive)(void);
#endif

bool db_auth_ct_equal(const char *a, size_t alen,
                      const char *b, size_t blen, size_t width)
{
    /* Fold the length difference in first, then XOR byte-by-byte over the
     * full width. Out-of-range indexes read as 0 on BOTH sides, so the loop
     * body is identical for every i and every input — no data-dependent
     * branches, no data-dependent trip count. */
    unsigned char acc = (unsigned char)((alen ^ blen) ? 1 : 0);
    for (size_t i = 0; i < width; i++) {
        unsigned char ca = (i < alen) ? (unsigned char)a[i] : 0;
        unsigned char cb = (i < blen) ? (unsigned char)b[i] : 0;
        acc |= (unsigned char)(ca ^ cb);
    }
    return acc == 0;
}

/* Strict base64: the RFC 4648 alphabet, optional trailing '='-padding, no
 * whitespace. Returns the decoded length, or -1 on any malformed input.
 * Strictness matters here: a lax decoder would give one guessed credential
 * several accepted spellings, and the per-boot cache below depends on every
 * accepted spelling decoding to exactly ONE byte sequence. */
static int b64_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap)
{
    size_t outlen = 0;
    unsigned bits = 0;
    int nbits = 0;
    size_t i = 0;

    for (; i < inlen; i++) {
        char c = in[i];
        int v;
        if (c >= 'A' && c <= 'Z')      v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+')             v = 62;
        else if (c == '/')             v = 63;
        else if (c == '=')             break;   /* padding: only '=' may follow */
        else return -1;

        bits = (bits << 6) | (unsigned)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            if (outlen >= outcap) return -1;
            out[outlen++] = (unsigned char)((bits >> nbits) & 0xFF);
        }
    }
    /* Whatever remains must be padding, and the padding must square with the
     * data length (a lone dangling 6 bits means a truncated group). */
    for (; i < inlen; i++)
        if (in[i] != '=') return -1;
    if (nbits >= 6) return -1;
    return (int)outlen;
}

/* Parse "Basic <b64>" into decoded credential bytes. Returns the decoded
 * length, -1 = no header (MISSING), -2 = malformed (fast WRONG — the
 * reasoning for the fast path is in http_auth.h). */
static int parse_basic(const char *authorization, unsigned char *out,
                       size_t outcap)
{
    if (!authorization || !authorization[0]) return -1;

    /* Scheme: "Basic", case-insensitive (RFC 7617), then at least one space. */
    static const char scheme[] = "basic";
    size_t si = 0;
    const char *p = authorization;
    while (*p == ' ' || *p == '\t') p++;
    for (; si < sizeof(scheme) - 1; si++, p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != scheme[si]) return -2;
    }
    if (*p != ' ' && *p != '\t') return -2;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return -2;

    int dlen = b64_decode(p, strlen(p), out, outcap);
    return (dlen < 0) ? -2 : dlen;
}

void db_auth_cache_init(db_auth_cache_t *cache,
                        const uint8_t nonce[DB_PW_HASH_LEN])
{
    memset(cache, 0, sizeof(*cache));
    memcpy(cache->nonce, nonce, DB_PW_HASH_LEN);
}

void db_auth_cache_invalidate(db_auth_cache_t *cache)
{
    cache->valid = false;
    memset(cache->digest, 0, sizeof(cache->digest));
    cache->generation++;
}

db_auth_result_t db_auth_check(const char *authorization,
                               const db_pw_rec_t *rec,
                               db_auth_cache_t *cache)
{
    /* Fail CLOSED on a caller bug: an empty record means "auth disabled",
     * and that decision belongs to the caller's short-circuit, never to a
     * hash comparison down here. */
    if (!rec || rec->iters == 0 || !cache) return DB_AUTH_WRONG;

    unsigned char decoded[AUTH_DECODE_MAX];
    int dlen = parse_basic(authorization, decoded, sizeof(decoded));
    if (dlen == -1) return DB_AUTH_MISSING;
    if (dlen < 0) return DB_AUTH_WRONG;

    /* Shape gate: "admin:" plus 1..64 password bytes. Outside those PUBLIC
     * bounds (API.md documents both) no credential can ever be correct, so
     * this is the malformed class and fails fast — see http_auth.h. */
    if (dlen < AUTH_USER_PREFIX_LEN + 1 || dlen > AUTH_CRED_MAX) {
        memset(decoded, 0, sizeof(decoded));
        return DB_AUTH_WRONG;
    }

    /* Snapshot the record: it lives in the mutable config, and the slow path
     * below runs long enough for a concurrent password change to rewrite it
     * mid-read. A torn record must not be verified against. */
    db_pw_rec_t r = *rec;
    if (r.iters == 0) {
        memset(decoded, 0, sizeof(decoded));
        return DB_AUTH_WRONG;
    }

    /* Fast path: SHA256(boot_nonce || credential) against the cached success.
     * Constant-time and over the full credential (user AND password), so the
     * cache introduces no oracle the slow path did not already lack. */
    uint8_t nc[DB_PW_HASH_LEN + AUTH_CRED_MAX];
    memcpy(nc, cache->nonce, DB_PW_HASH_LEN);
    memcpy(nc + DB_PW_HASH_LEN, decoded, (size_t)dlen);
    uint8_t digest[DB_PW_HASH_LEN];
    db_pw_sha256(nc, DB_PW_HASH_LEN + (size_t)dlen, digest);
    memset(nc, 0, sizeof(nc));

    if (cache->valid &&
        db_auth_ct_equal((const char *)digest, DB_PW_HASH_LEN,
                         (const char *)cache->digest, DB_PW_HASH_LEN,
                         DB_PW_HASH_LEN)) {
        memset(decoded, 0, sizeof(decoded));
        memset(digest, 0, sizeof(digest));
        return DB_AUTH_OK;
    }

    /* Slow path: the full PBKDF2 derive (~1 s on the box, by calibration —
     * pw_hash.h). Snapshot the generation FIRST: if a password change lands
     * while we grind, our result describes a retired record and must neither
     * be cached nor accepted. */
    uint32_t gen = cache->generation;

    uint8_t guess[DB_PW_HASH_LEN];
    db_pw_pbkdf2(decoded + AUTH_USER_PREFIX_LEN,
                 (size_t)dlen - AUTH_USER_PREFIX_LEN,
                 r.salt, DB_PW_SALT_LEN, r.iters, guess);

#ifdef DB_HOSTTEST
    if (db_auth_test_after_derive) db_auth_test_after_derive();
#endif

    /* Fold the username in AFTER the derive, constant-time, so wrong-user
     * and wrong-password cost identically and neither is timing-visible. */
    bool user_ok = db_auth_ct_equal((const char *)decoded, AUTH_USER_PREFIX_LEN,
                                    AUTH_USER_PREFIX, AUTH_USER_PREFIX_LEN,
                                    AUTH_USER_PREFIX_LEN);
    bool hash_ok = db_auth_ct_equal((const char *)guess, DB_PW_HASH_LEN,
                                    (const char *)r.hash, DB_PW_HASH_LEN,
                                    DB_PW_HASH_LEN);
    bool ok = user_ok && hash_ok;

    if (ok && cache->generation == gen) {
        /* Only a verified-correct credential enters the cache, and only when
         * no invalidation raced the derive. */
        memcpy(cache->digest, digest, DB_PW_HASH_LEN);
        cache->valid = true;
    } else if (ok) {
        /* Correct against the OLD record, but the password changed while we
         * verified: the credential is retired, refuse it. The client retries
         * with whatever the operator just set. */
        ok = false;
    }

    memset(decoded, 0, sizeof(decoded));
    memset(digest, 0, sizeof(digest));
    memset(guess, 0, sizeof(guess));
    memset(&r, 0, sizeof(r));

    return ok ? DB_AUTH_OK : DB_AUTH_WRONG;
}
