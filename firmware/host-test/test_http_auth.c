/*
 * test_http_auth.c - Host-compiled tests for the pure halves of the web
 * security wave: the password-hashing primitives (main/pw_hash.c), the HTTP
 * Basic credential check with its per-boot verify cache (main/http_auth.c)
 * and the structural PEM preflight (main/pem_scan.c).
 *
 * All three files compile here unmodified — no ESP-IDF, no mbedTLS — which
 * is the point: the code that stands between the network and the stored
 * password hash is exactly the code most worth running under a fast,
 * merciless loop. The things pinned down:
 *
 *   - SHA-256, HMAC-SHA256 and PBKDF2-HMAC-SHA256 against their PUBLISHED
 *     vectors (FIPS 180-4 / RFC 4231 / the RFC 7914 PBKDF2 vectors) — a
 *     hand-rolled primitive is only tolerable while these pass;
 *   - db_pw_rec_from_plaintext round-trips: the exact derivation the v2->v3
 *     config migration feeds the old PLAINTEXT password through must produce
 *     a record the login path then verifies;
 *   - the calibration clamp (db_pw_pick_iterations): target math, both clamp
 *     edges, and the degenerate-probe fallback;
 *   - every accepted Authorization spelling decodes to exactly ONE credential
 *     (the verify cache keys on the decoded bytes, so a lax parser would
 *     mint several cache identities for one password);
 *   - malformed base64, wrong scheme, wrong user, wrong password and a
 *     missing header are all refused, MISSING vs WRONG classified correctly,
 *     and the FAST/SLOW split is enforced by counting derives through the
 *     DB_HOSTTEST hook: malformed and header-less requests must never reach
 *     PBKDF2, every well-formed guess must always pay for it;
 *   - the per-boot cache state machine: a success is cached and later
 *     requests skip the derive; a WRONG credential never enters the cache;
 *     invalidation (= password change) forces a fresh full verify; and the
 *     change-during-slow-verify race — generation bumped between derive and
 *     cache-store — neither caches nor accepts the stale result;
 *   - the constant-time comparator's CONTRACT (equality only; the timing
 *     property itself is by construction and reviewed, not measurable here);
 *   - each pem_scan error sentence fires on the paste mistake it names, and a
 *     structurally sound pair (single cert, chain, all three key labels)
 *     passes through to mbedTLS.
 *
 * Build and run:  make test   (this binary is compiled with -DDB_HOSTTEST
 * for the mid-verify test hook; the hook is compiled out on the device)
 */
#include <stdio.h>
#include <string.h>

#include "http_auth.h"
#include "pem_scan.h"
#include "pw_hash.h"

/* ---- micro test harness (same shape as test_rf_decode.c) ----------------- */

static int g_pass;
static int g_fail;
static const char *g_case = "";

#define CASE(name) do { g_case = (name); } while (0)

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            g_pass++;                                                           \
        } else {                                                                \
            g_fail++;                                                           \
            printf("  FAIL [%s] %s:%d: ", g_case, __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

/* ---- helpers ------------------------------------------------------------- */

/* Reference base64 ENCODER (the module only decodes, so the test builds its
 * inputs independently instead of round-tripping through the code under test). */
static void b64enc(const char *in, char *out, int pad)
{
    static const char AB[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)(unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)(unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)(unsigned char)in[i + 2];
        out[o++] = AB[(v >> 18) & 63];
        out[o++] = AB[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? AB[(v >> 6) & 63] : (pad ? '=' : '\0');
        if (out[o - 1] == '\0') { o--; break; }
        out[o++] = (i + 2 < n) ? AB[v & 63] : (pad ? '=' : '\0');
        if (out[o - 1] == '\0') { o--; break; }
    }
    out[o] = '\0';
}

/* "Basic " + base64("admin:<pw>") — the one header the box accepts. */
static void make_header(const char *user, const char *pw, char *out, size_t cap)
{
    char cred[256], b64[400];
    snprintf(cred, sizeof(cred), "%s:%s", user, pw);
    b64enc(cred, b64, 1);
    snprintf(out, cap, "Basic %s", b64);
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", in[i]);
    out[2 * n] = '\0';
}

/* Test iteration counts are tiny on purpose (the clamp lives in
 * db_pw_pick_iterations, not in the verify path) — a full calibrated derive
 * per CHECK would make this suite crawl for no extra coverage. */
#define TEST_ITERS 16

static const uint8_t TEST_SALT[DB_PW_SALT_LEN] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
};
static const uint8_t TEST_NONCE[DB_PW_HASH_LEN] = {
    0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a,
    0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a,
    0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a,
    0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0x5a,
};

static db_pw_rec_t mkrec(const char *pw)
{
    db_pw_rec_t rec;
    db_pw_rec_from_plaintext(&rec, pw, TEST_SALT, TEST_ITERS);
    return rec;
}

static db_auth_cache_t mkcache(void)
{
    db_auth_cache_t c;
    db_auth_cache_init(&c, TEST_NONCE);
    return c;
}

/* Count PBKDF2 derives through the DB_HOSTTEST hook: the fast/slow contract
 * of http_auth.h is exactly "who reached the derive". */
static int g_derives;
static void count_derive(void) { g_derives++; }

/* ---- the hash primitives, against published vectors ----------------------- */

static void test_sha256_vectors(void)
{
    CASE("sha256 vectors");
    uint8_t d[32];
    char h[65];

    db_pw_sha256("abc", 3, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223"
                     "b00361a396177a9cb410ff61f20015ad"), "sha256(abc): %s", h);

    db_pw_sha256("", 0, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb924"
                     "27ae41e4649b934ca495991b7852b855"), "sha256(\"\"): %s", h);

    /* Two blocks + length padding straddle (FIPS 180-4 example #2). */
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    db_pw_sha256(m2, strlen(m2), d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039"
                     "a33ce45964ff2167f6ecedd419db06c1"), "sha256(2-block): %s", h);
}

static void test_hmac_vectors(void)
{
    CASE("hmac vectors");
    uint8_t d[32];
    char h[65];

    /* RFC 4231 test case 1. */
    uint8_t key1[20];
    memset(key1, 0x0b, sizeof(key1));
    db_pw_hmac_sha256(key1, sizeof(key1), "Hi There", 8, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b"
                     "881dc200c9833da726e9376c2e32cff7"), "rfc4231 tc1: %s", h);

    /* RFC 4231 test case 2 (short key path). */
    db_pw_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "5bdcc146bf60754e6a042426089575c7"
                     "5a003f089d2739839dec58b964ec3843"), "rfc4231 tc2: %s", h);

    /* RFC 4231 test case 6: a 131-byte key exercises the hash-the-key path. */
    uint8_t key6[131];
    memset(key6, 0xaa, sizeof(key6));
    db_pw_hmac_sha256(key6, sizeof(key6),
                      "Test Using Larger Than Block-Size Key - Hash Key First",
                      54, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "60e431591ee0b67f0d8a26aacbf5b77f"
                     "8e0bc6213728c5140546040f0ee37f54"), "rfc4231 tc6: %s", h);
}

static void test_pbkdf2_vectors(void)
{
    CASE("pbkdf2 vectors");
    uint8_t d[32];
    char h[65];

    /* The PBKDF2-HMAC-SHA256 vectors published with RFC 7914 (scrypt). */
    db_pw_pbkdf2("password", 8, (const uint8_t *)"salt", 4, 1, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "120fb6cffcf8b32c43e7225256c4f837"
                     "a86548c92ccc35480805987cb70be17b"), "c=1: %s", h);

    db_pw_pbkdf2("password", 8, (const uint8_t *)"salt", 4, 2, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "ae4d0c95af6b46d32d0adff928f06dd0"
                     "2a303f8ef3c251dfd6e2d85a95474c43"), "c=2: %s", h);

    db_pw_pbkdf2("password", 8, (const uint8_t *)"salt", 4, 4096, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "c5e478d59288c841aa530db6845c4c8d"
                     "962893a001ce4e11a4963873aa98134a"), "c=4096: %s", h);

    /* Long password AND a salt longer than DB_PW_SALT_LEN — the streaming
     * salt path (RFC 6070's long vector, transposed to SHA-256 as in the
     * scrypt draft's appendix). */
    db_pw_pbkdf2("passwordPASSWORDpassword", 24,
                 (const uint8_t *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36,
                 4096, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "348c89dbcbd32b2f32d814b8116e84cf"
                     "2b17347ebc1800181c4e2a1fb8dd53e1"), "long/36-salt: %s", h);
}

/* ---- calibration clamp ----------------------------------------------------- */

static void test_pick_iterations(void)
{
    CASE("pick iterations");

    /* Degenerate probes: an unmeasurable cost must not mint a minute-long
     * (or a free) login — fall to the floor. */
    CHECK(db_pw_pick_iterations(0, 12345) == DB_PW_ITERS_MIN, "probe_iters=0");
    CHECK(db_pw_pick_iterations(200, 0) == DB_PW_ITERS_MIN, "probe_us=0");

    /* The notes' measured S3 rate, ~140 us/iteration: 200 iters in 28 ms
     * -> 1e6/140 = 7142 for the 1 s target. In range, no clamp. */
    CHECK(db_pw_pick_iterations(200, 28000) == 7142,
          "140us/iter: %u", (unsigned)db_pw_pick_iterations(200, 28000));

    /* A fast box clamps at the ceiling (1 us/iter would ask for 1M). */
    CHECK(db_pw_pick_iterations(200, 200) == DB_PW_ITERS_MAX, "fast clamp");
    /* A glacial box clamps at the floor (10 ms/iter would ask for 100). */
    CHECK(db_pw_pick_iterations(200, 2000000) == DB_PW_ITERS_MIN, "slow clamp");

    /* Clamp edges are inclusive: rates that land exactly on a bound pass
     * through unclamped. target_us * probe / probe_us. */
    CHECK(db_pw_pick_iterations(DB_PW_ITERS_MIN, 1000000) == DB_PW_ITERS_MIN,
          "floor edge");
    CHECK(db_pw_pick_iterations(DB_PW_ITERS_MAX, 1000000) == DB_PW_ITERS_MAX,
          "ceiling edge");
    CHECK(db_pw_pick_iterations(DB_PW_ITERS_MAX + 1, 1000000) == DB_PW_ITERS_MAX,
          "just past ceiling");

    /* Monotonic sanity: a slower probe never yields MORE iterations. */
    CHECK(db_pw_pick_iterations(200, 40000) <= db_pw_pick_iterations(200, 20000),
          "not monotonic");
}

/* ---- migration hashing ----------------------------------------------------- */

/* db_pw_rec_from_plaintext is THE function db_config.c's v2->v3 migration
 * feeds the stored plaintext through. The upgrade contract — "a v2 box comes
 * up with its password still working" — reduces to: the record built from a
 * plaintext verifies that same plaintext through the real login path. */
static void test_migration_roundtrip(void)
{
    CASE("migration hashing");
    static const char *pws[] = {
        "hunter2", "with space", "ümläut-ß", "colon:inside", "a",
    };
    for (size_t i = 0; i < sizeof(pws) / sizeof(pws[0]); i++) {
        db_pw_rec_t rec = mkrec(pws[i]);
        CHECK(rec.iters == TEST_ITERS, "iters not stored (%zu)", i);
        CHECK(memcmp(rec.salt, TEST_SALT, DB_PW_SALT_LEN) == 0,
              "salt not stored (%zu)", i);

        db_auth_cache_t cache = mkcache();
        char hdr[512];
        make_header("admin", pws[i], hdr, sizeof(hdr));
        CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_OK,
              "migrated password %zu rejected", i);
        make_header("admin", "not-it", hdr, sizeof(hdr));
        CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG,
              "wrong password %zu accepted", i);
    }

    /* The stored iteration count is authoritative: two records for the same
     * password at different counts verify independently — which is what
     * keeps old hashes valid after a recalibration. */
    db_pw_rec_t r16 = mkrec("same-pw");
    db_pw_rec_t r64;
    db_pw_rec_from_plaintext(&r64, "same-pw", TEST_SALT, 64);
    CHECK(memcmp(r16.hash, r64.hash, DB_PW_HASH_LEN) != 0,
          "iteration count did not change the hash");
    db_auth_cache_t cache = mkcache();
    char hdr[512];
    make_header("admin", "same-pw", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &r64, &cache) == DB_AUTH_OK,
          "other-iteration record rejected");

    /* Same password, different salt -> different hash (the point of salt). */
    uint8_t salt2[DB_PW_SALT_LEN];
    memset(salt2, 0xEE, sizeof(salt2));
    db_pw_rec_t rs;
    db_pw_rec_from_plaintext(&rs, "same-pw", salt2, TEST_ITERS);
    CHECK(memcmp(r16.hash, rs.hash, DB_PW_HASH_LEN) != 0,
          "salt did not change the hash");
}

/* ---- auth: the happy path ------------------------------------------------ */

static void test_auth_accepts(void)
{
    CASE("auth accepts");
    db_pw_rec_t rec = mkrec("secret");
    db_auth_cache_t cache = mkcache();
    char hdr[512];

    make_header("admin", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_OK,
          "canonical header rejected: %s", hdr);

    /* Scheme case-insensitivity (RFC 7617) and flexible whitespace. */
    const char *b64 = hdr + 6;   /* skip "Basic " */
    char v[512];
    snprintf(v, sizeof(v), "basic %s", b64);
    CHECK(db_auth_check(v, &rec, &cache) == DB_AUTH_OK, "lowercase scheme rejected");
    snprintf(v, sizeof(v), "BASIC %s", b64);
    CHECK(db_auth_check(v, &rec, &cache) == DB_AUTH_OK, "uppercase scheme rejected");
    snprintf(v, sizeof(v), "Basic   %s", b64);
    CHECK(db_auth_check(v, &rec, &cache) == DB_AUTH_OK, "multi-space rejected");
    snprintf(v, sizeof(v), "  Basic %s", b64);
    CHECK(db_auth_check(v, &rec, &cache) == DB_AUTH_OK, "leading-space rejected");

    /* Unpadded base64 decodes to the SAME credential — accepted, and no
     * different credential can hide behind the padding difference (nor a
     * second cache identity: the cache keys on the DECODED bytes). */
    db_pw_rec_t rec1 = mkrec("pw1");
    db_auth_cache_t cache1 = mkcache();
    char cred[128], nopad[200];
    snprintf(cred, sizeof(cred), "admin:%s", "pw1");   /* 9 bytes -> padding */
    b64enc(cred, nopad, 0);
    snprintf(v, sizeof(v), "Basic %s", nopad);
    CHECK(db_auth_check(v, &rec1, &cache1) == DB_AUTH_OK, "unpadded b64 rejected");

    /* Passwords with the characters people actually use. */
    static const char *pws[] = {
        "a", "with space", "ümläut-ß", "colon:inside",
        ("64char-64char-64char-64char-64char-64char-64char-64char-64ch"),
    };
    for (size_t i = 0; i < sizeof(pws) / sizeof(pws[0]); i++) {
        db_pw_rec_t r = mkrec(pws[i]);
        db_auth_cache_t c = mkcache();
        make_header("admin", pws[i], hdr, sizeof(hdr));
        CHECK(db_auth_check(hdr, &r, &c) == DB_AUTH_OK,
              "password %zu rejected", i);
    }
    /* The documented ceiling: exactly 64 characters works. */
    char pw64[65];
    memset(pw64, 'x', 64); pw64[64] = '\0';
    db_pw_rec_t r64 = mkrec(pw64);
    db_auth_cache_t c64 = mkcache();
    make_header("admin", pw64, hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &r64, &c64) == DB_AUTH_OK, "64-char password rejected");
}

/* ---- auth: everything refused -------------------------------------------- */

static void test_auth_refuses(void)
{
    CASE("auth refuses");
    db_pw_rec_t rec = mkrec("secret");
    db_auth_cache_t cache = mkcache();
    char hdr[512];

    /* Missing vs wrong is a real distinction (the UI treats MISSING quietly). */
    CHECK(db_auth_check(NULL, &rec, &cache) == DB_AUTH_MISSING, "NULL != MISSING");
    CHECK(db_auth_check("", &rec, &cache) == DB_AUTH_MISSING, "\"\" != MISSING");

    make_header("admin", "wrong", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "wrong pw accepted");
    make_header("admin", "secre", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "prefix pw accepted");
    make_header("admin", "secrets", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "superstring pw accepted");
    make_header("admin", "", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "empty pw accepted");

    /* The username is not negotiable. */
    make_header("root", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "user root accepted");
    make_header("Admin", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "user Admin accepted");
    make_header("", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "empty user accepted");

    /* No colon at all in the decoded credential. */
    char b64[64];
    b64enc("adminsecret", b64, 1);
    snprintf(hdr, sizeof(hdr), "Basic %s", b64);
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "colonless accepted");

    /* Other schemes, malformed schemes, scheme-only. */
    CHECK(db_auth_check("Bearer abc123", &rec, &cache) == DB_AUTH_WRONG,
          "Bearer accepted");
    CHECK(db_auth_check("Basic", &rec, &cache) == DB_AUTH_WRONG,
          "bare scheme accepted");
    CHECK(db_auth_check("Basic ", &rec, &cache) == DB_AUTH_WRONG,
          "scheme + space accepted");
    CHECK(db_auth_check("Basicx YWRtaW46c2VjcmV0", &rec, &cache) == DB_AUTH_WRONG,
          "glued scheme accepted");

    /* Malformed base64: bad characters, embedded whitespace, broken padding,
     * data after the padding. A strict decoder refuses each. */
    CHECK(db_auth_check("Basic !!!!", &rec, &cache) == DB_AUTH_WRONG, "junk b64");
    CHECK(db_auth_check("Basic YWRt aW46c2VjcmV0", &rec, &cache) == DB_AUTH_WRONG,
          "inner space survived");
    CHECK(db_auth_check("Basic YQ=X", &rec, &cache) == DB_AUTH_WRONG,
          "data after padding survived");
    CHECK(db_auth_check("Basic YQ=", &rec, &cache) == DB_AUTH_WRONG,
          "short-padded 'a' accepted as a credential");
    CHECK(db_auth_check("Basic Y", &rec, &cache) == DB_AUTH_WRONG,
          "dangling 6 bits accepted");

    /* Fail CLOSED on caller bugs: an empty record means "auth disabled" and
     * belongs to the caller's short-circuit — down here it never matches. */
    db_pw_rec_t empty;
    memset(&empty, 0, sizeof(empty));
    make_header("admin", "", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &empty, &cache) == DB_AUTH_WRONG, "empty rec open");
    make_header("admin", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &empty, &cache) == DB_AUTH_WRONG,
          "empty rec accepted a password");
    CHECK(db_auth_check(hdr, NULL, &cache) == DB_AUTH_WRONG, "NULL rec open");
    CHECK(db_auth_check(hdr, &rec, NULL) == DB_AUTH_WRONG, "NULL cache open");

    /* Over-long guess (65 chars can never be correct — the 64 ceiling is
     * public API.md contract) refuses without pretending to evaluate. */
    char pw65[66];
    memset(pw65, 'x', 65); pw65[65] = '\0';
    make_header("admin", pw65, hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "65-char pw accepted");
}

/* ---- fast-vs-slow: who pays the PBKDF2 ------------------------------------ */

static void test_auth_cost_classes(void)
{
    CASE("auth cost classes");
    db_pw_rec_t rec = mkrec("secret");
    db_auth_cache_t cache = mkcache();
    char hdr[512];
    db_auth_test_after_derive = count_derive;

    /* MISSING and malformed never reach the derive: they carry no guess, and
     * ~1 s of the single worker per junk request would be a free stall
     * primitive (http_auth.h). */
    g_derives = 0;
    db_auth_check(NULL, &rec, &cache);
    db_auth_check("", &rec, &cache);
    db_auth_check("Bearer abc123", &rec, &cache);
    db_auth_check("Basic", &rec, &cache);
    db_auth_check("Basic !!!!", &rec, &cache);
    db_auth_check("Basic YQ==", &rec, &cache);          /* 1 byte: shape gate */
    char pw65[66];
    memset(pw65, 'x', 65); pw65[65] = '\0';
    make_header("admin", pw65, hdr, sizeof(hdr));
    db_auth_check(hdr, &rec, &cache);                   /* 71 bytes: shape gate */
    CHECK(g_derives == 0, "malformed/missing paid the derive %d times", g_derives);

    /* Every WELL-FORMED guess pays: wrong password AND wrong username (the
     * username folds into the slow compare so the two are timing-identical). */
    g_derives = 0;
    make_header("admin", "wrong", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "wrong pw accepted");
    make_header("root", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_check(hdr, &rec, &cache) == DB_AUTH_WRONG, "wrong user accepted");
    CHECK(g_derives == 2, "well-formed guesses paid %d derives, want 2", g_derives);

    db_auth_test_after_derive = NULL;
}

/* ---- the per-boot verify cache -------------------------------------------- */

static void test_auth_cache(void)
{
    CASE("auth cache");
    db_pw_rec_t rec = mkrec("secret");
    db_auth_cache_t cache = mkcache();
    char good[512], bad[512];
    make_header("admin", "secret", good, sizeof(good));
    make_header("admin", "nope", bad, sizeof(bad));
    db_auth_test_after_derive = count_derive;

    CHECK(!cache.valid, "cache born valid");

    /* Cold verify: one derive, then the success is cached. */
    g_derives = 0;
    CHECK(db_auth_check(good, &rec, &cache) == DB_AUTH_OK, "cold verify failed");
    CHECK(g_derives == 1, "cold verify: %d derives", g_derives);
    CHECK(cache.valid, "success not cached");

    /* Warm verifies: same result, ZERO derives — one SHA-256 per request is
     * the entire point of the cache. */
    g_derives = 0;
    for (int i = 0; i < 5; i++)
        CHECK(db_auth_check(good, &rec, &cache) == DB_AUTH_OK,
              "warm verify %d failed", i);
    CHECK(g_derives == 0, "warm verifies paid %d derives", g_derives);

    /* Alternative SPELLINGS of the same credential hit the same cache entry:
     * the cache keys on decoded bytes, not on the header text. */
    char alt[512];
    snprintf(alt, sizeof(alt), "BASIC   %s", good + 6);
    g_derives = 0;
    CHECK(db_auth_check(alt, &rec, &cache) == DB_AUTH_OK, "respelt hdr rejected");
    CHECK(g_derives == 0, "respelling missed the cache");

    /* A wrong guess against a warm cache: full price, refused, and the GOOD
     * entry survives (a guesser must not be able to evict the admin). */
    g_derives = 0;
    CHECK(db_auth_check(bad, &rec, &cache) == DB_AUTH_WRONG, "bad pw accepted");
    CHECK(g_derives == 1, "wrong guess dodged the derive");
    CHECK(cache.valid, "wrong guess evicted the cache");
    g_derives = 0;
    CHECK(db_auth_check(good, &rec, &cache) == DB_AUTH_OK, "good pw after bad");
    CHECK(g_derives == 0, "good credential lost its cache entry");

    /* A wrong credential NEVER seeds an empty cache — so it always pays. */
    db_auth_cache_t fresh = mkcache();
    g_derives = 0;
    CHECK(db_auth_check(bad, &rec, &fresh) == DB_AUTH_WRONG, "bad pw accepted");
    CHECK(!fresh.valid, "WRONG entered the cache");
    CHECK(db_auth_check(bad, &rec, &fresh) == DB_AUTH_WRONG, "bad pw accepted");
    CHECK(g_derives == 2, "repeated wrong guess got a fast path: %d", g_derives);

    /* Invalidation = password change: the old success must re-verify in full
     * (and would fail against a genuinely new record). */
    uint32_t gen_before = cache.generation;
    db_auth_cache_invalidate(&cache);
    CHECK(!cache.valid, "invalidate left the cache valid");
    CHECK(cache.generation == gen_before + 1, "invalidate did not bump gen");
    g_derives = 0;
    CHECK(db_auth_check(good, &rec, &cache) == DB_AUTH_OK, "re-verify failed");
    CHECK(g_derives == 1, "re-verify after invalidate: %d derives", g_derives);

    /* Password actually changed: old credential now fails (full price), new
     * one verifies and caches. */
    db_pw_rec_t rec2 = mkrec("changed");
    db_auth_cache_invalidate(&cache);
    CHECK(db_auth_check(good, &rec2, &cache) == DB_AUTH_WRONG,
          "old password still valid after change");
    CHECK(!cache.valid, "old password re-entered the cache");
    char good2[512];
    make_header("admin", "changed", good2, sizeof(good2));
    CHECK(db_auth_check(good2, &rec2, &cache) == DB_AUTH_OK, "new password refused");
    CHECK(cache.valid, "new password not cached");

    db_auth_test_after_derive = NULL;
}

/* ---- the change-during-slow-verify race ----------------------------------- */

static db_auth_cache_t *g_race_cache;
static void invalidate_midverify(void)
{
    g_derives++;
    db_auth_cache_invalidate(g_race_cache);   /* the password change lands
                                                 while the derive grinds */
}

static void test_auth_generation_race(void)
{
    CASE("generation race");
    db_pw_rec_t rec = mkrec("secret");
    db_auth_cache_t cache = mkcache();
    char good[512];
    make_header("admin", "secret", good, sizeof(good));

    /* An invalidation landing between the generation snapshot and the
     * cache-store decision: the verify's own result describes a RETIRED
     * record, so it must neither be cached nor accepted — the client simply
     * retries against whatever the operator just set. */
    g_race_cache = &cache;
    db_auth_test_after_derive = invalidate_midverify;
    g_derives = 0;
    CHECK(db_auth_check(good, &rec, &cache) == DB_AUTH_WRONG,
          "stale verify accepted across a mid-flight password change");
    CHECK(g_derives == 1, "race test never reached the derive");
    CHECK(!cache.valid, "stale verify cached across a password change");

    /* And the system recovers: with no further interference the same
     * credential verifies and caches normally. */
    db_auth_test_after_derive = NULL;
    CHECK(db_auth_check(good, &rec, &cache) == DB_AUTH_OK, "recovery failed");
    CHECK(cache.valid, "recovery did not cache");
}

/* ---- the constant-time comparator's contract ------------------------------ */

static void test_ct_equal(void)
{
    CASE("ct_equal");
    CHECK(db_auth_ct_equal("abc", 3, "abc", 3, 16), "equal != equal");
    CHECK(db_auth_ct_equal("", 0, "", 0, 16), "empty != empty");
    CHECK(!db_auth_ct_equal("abc", 3, "abd", 3, 16), "last-byte diff equal");
    CHECK(!db_auth_ct_equal("abc", 3, "xbc", 3, 16), "first-byte diff equal");
    CHECK(!db_auth_ct_equal("abc", 3, "ab", 2, 16), "length diff equal");
    CHECK(!db_auth_ct_equal("abc", 3, "abc\0", 4, 16), "trailing NUL equal");
    /* Same length, same prefix, difference beyond it. */
    CHECK(!db_auth_ct_equal("aaaa", 4, "aaab", 4, 4), "width-edge diff equal");
}

/* ---- pem_scan ------------------------------------------------------------- */

/* Tiny synthetic PEMs: pem_scan is structural, so the base64 payload can be
 * anything — mbedTLS is the gate that reads it. */
#define CERT1 "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n"
#define KEY_P8  "-----BEGIN PRIVATE KEY-----\nMIGH\n-----END PRIVATE KEY-----\n"
#define KEY_EC  "-----BEGIN EC PRIVATE KEY-----\nMHcC\n-----END EC PRIVATE KEY-----\n"
#define KEY_RSA "-----BEGIN RSA PRIVATE KEY-----\nMIIE\n-----END RSA PRIVATE KEY-----\n"
#define KEY_ENC "-----BEGIN ENCRYPTED PRIVATE KEY-----\nMIIF\n-----END ENCRYPTED PRIVATE KEY-----\n"

static const char *scan(const char *cert, const char *key)
{
    return db_pem_scan_pair(cert, cert ? strlen(cert) : 0,
                            key, key ? strlen(key) : 0);
}

/* The sentence must NAME the mistake — that is this module's whole job. */
#define EXPECT_ERR(needle, cert, key)                                           \
    do {                                                                        \
        const char *w = scan((cert), (key));                                    \
        CHECK(w != NULL, "expected a refusal mentioning \"%s\"", (needle));     \
        CHECK(w && strstr(w, (needle)) != NULL,                                 \
              "\"%s\" not in \"%s\"", (needle), w ? w : "(null)");              \
    } while (0)

static void test_pem_scan_accepts(void)
{
    CASE("pem accepts");
    CHECK(scan(CERT1, KEY_P8) == NULL, "single cert + pkcs8 refused: %s",
          scan(CERT1, KEY_P8));
    CHECK(scan(CERT1, KEY_EC) == NULL, "EC key refused");
    CHECK(scan(CERT1, KEY_RSA) == NULL, "RSA key refused");
    CHECK(scan(CERT1 CERT1, KEY_P8) == NULL, "leaf+chain refused");
    CHECK(scan(CERT1 CERT1 CERT1, KEY_P8) == NULL, "three-deep chain refused");
    /* RFC 7468 allows text around the blocks (openssl -text output). */
    CHECK(scan("Subject: CN=box\n" CERT1, "comment\n" KEY_P8) == NULL,
          "surrounding text refused");

    CHECK(db_pem_cert_count(CERT1, strlen(CERT1)) == 1, "count(1) wrong");
    CHECK(db_pem_cert_count(CERT1 CERT1, strlen(CERT1 CERT1)) == 2, "count(2) wrong");
    CHECK(db_pem_cert_count(KEY_P8, strlen(KEY_P8)) == 0, "count(key) wrong");
    CHECK(db_pem_cert_count(NULL, 0) == 0, "count(NULL) wrong");
}

static void test_pem_scan_refuses(void)
{
    CASE("pem refuses");
    EXPECT_ERR("cert_pem is empty", "", KEY_P8);
    EXPECT_ERR("cert_pem is empty", NULL, KEY_P8);
    EXPECT_ERR("key_pem is empty", CERT1, "");
    EXPECT_ERR("key_pem is empty", CERT1, NULL);

    /* The classic paste mistakes, each named. */
    EXPECT_ERR("swapped", KEY_P8, CERT1);           /* both fields swapped   */
    EXPECT_ERR("swapped", KEY_EC, KEY_EC);          /* key pasted into cert  */
    EXPECT_ERR("not DER", "MIIBpTCCAUu", KEY_P8);   /* raw base64, no armor  */
    EXPECT_ERR("truncated", "-----BEGIN CERTIFICATE-----\nMIIB\n", KEY_P8);
    EXPECT_ERR("also contains a private key", CERT1 KEY_P8, KEY_P8);
    EXPECT_ERR("encrypted", CERT1, KEY_ENC);
    EXPECT_ERR("no private-key block", CERT1, "MIGHAgEA");
    EXPECT_ERR("swapped", CERT1, CERT1);            /* cert in the key field */
    EXPECT_ERR("more than one private key", CERT1, KEY_P8 KEY_P8);
    EXPECT_ERR("more than one private key", CERT1, KEY_P8 KEY_EC);
    EXPECT_ERR("also contains a certificate", CERT1, KEY_P8 CERT1);
}

/* ---- main ----------------------------------------------------------------- */

int main(void)
{
    test_sha256_vectors();
    test_hmac_vectors();
    test_pbkdf2_vectors();
    test_pick_iterations();
    test_migration_roundtrip();
    test_auth_accepts();
    test_auth_refuses();
    test_auth_cost_classes();
    test_auth_cache();
    test_auth_generation_race();
    test_ct_equal();
    test_pem_scan_accepts();
    test_pem_scan_refuses();

    printf("test_http_auth: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
