/*
 * test_http_auth.c - Host-compiled tests for the two pure halves of the web
 * security wave: the HTTP Basic credential check (main/http_auth.c) and the
 * structural PEM preflight (main/pem_scan.c).
 *
 * Both files compile here unmodified — no ESP-IDF, no mbedTLS — which is the
 * point: the parser that stands between the network and the password compare
 * is exactly the code most worth running under a fast, merciless loop. The
 * things pinned down:
 *
 *   - every accepted Authorization spelling decodes to exactly ONE credential
 *     (a lax parser would hand an attacker several tries per rate-limit slot);
 *   - malformed base64, wrong scheme, wrong user, wrong password and a missing
 *     header are all refused, and MISSING vs WRONG is classified correctly;
 *   - the constant-time comparator's CONTRACT (equality only; the timing
 *     property itself is by construction and reviewed, not measurable here);
 *   - each pem_scan error sentence fires on the paste mistake it names, and a
 *     structurally sound pair (single cert, chain, all three key labels)
 *     passes through to mbedTLS.
 *
 * Build and run:  make test
 */
#include <stdio.h>
#include <string.h>

#include "http_auth.h"
#include "pem_scan.h"

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

/* ---- auth: the happy path ------------------------------------------------ */

static void test_auth_accepts(void)
{
    CASE("auth accepts");
    char hdr[512];

    make_header("admin", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_OK,
          "canonical header rejected: %s", hdr);

    /* Scheme case-insensitivity (RFC 7617) and flexible whitespace. */
    const char *b64 = hdr + 6;   /* skip "Basic " */
    char v[512];
    snprintf(v, sizeof(v), "basic %s", b64);
    CHECK(db_auth_basic_check(v, "secret") == DB_AUTH_OK, "lowercase scheme rejected");
    snprintf(v, sizeof(v), "BASIC %s", b64);
    CHECK(db_auth_basic_check(v, "secret") == DB_AUTH_OK, "uppercase scheme rejected");
    snprintf(v, sizeof(v), "Basic   %s", b64);
    CHECK(db_auth_basic_check(v, "secret") == DB_AUTH_OK, "multi-space rejected");
    snprintf(v, sizeof(v), "  Basic %s", b64);
    CHECK(db_auth_basic_check(v, "secret") == DB_AUTH_OK, "leading-space rejected");

    /* Unpadded base64 decodes to the SAME credential — accepted, and no
     * different credential can hide behind the padding difference. */
    char cred[128], nopad[200];
    snprintf(cred, sizeof(cred), "admin:%s", "pw1");   /* 9 bytes -> padding */
    b64enc(cred, nopad, 0);
    snprintf(v, sizeof(v), "Basic %s", nopad);
    CHECK(db_auth_basic_check(v, "pw1") == DB_AUTH_OK, "unpadded b64 rejected");

    /* Passwords with the characters people actually use. */
    static const char *pws[] = {
        "a", "with space", "ümläut-ß", "colon:inside",
        ("64char-64char-64char-64char-64char-64char-64char-64char-64ch"),
    };
    for (size_t i = 0; i < sizeof(pws) / sizeof(pws[0]); i++) {
        make_header("admin", pws[i], hdr, sizeof(hdr));
        CHECK(db_auth_basic_check(hdr, pws[i]) == DB_AUTH_OK,
              "password %zu rejected", i);
    }
    /* The documented ceiling: exactly 64 characters works. */
    char pw64[65];
    memset(pw64, 'x', 64); pw64[64] = '\0';
    make_header("admin", pw64, hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, pw64) == DB_AUTH_OK, "64-char password rejected");
}

/* ---- auth: everything refused -------------------------------------------- */

static void test_auth_refuses(void)
{
    CASE("auth refuses");
    char hdr[512];

    /* Missing vs wrong is a real distinction (the UI treats MISSING quietly). */
    CHECK(db_auth_basic_check(NULL, "secret") == DB_AUTH_MISSING, "NULL != MISSING");
    CHECK(db_auth_basic_check("", "secret") == DB_AUTH_MISSING, "\"\" != MISSING");

    make_header("admin", "wrong", hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_WRONG, "wrong pw accepted");
    make_header("admin", "secre", hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_WRONG, "prefix pw accepted");
    make_header("admin", "secrets", hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_WRONG, "superstring pw accepted");
    make_header("admin", "", hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_WRONG, "empty pw accepted");

    /* The username is not negotiable. */
    make_header("root", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_WRONG, "user root accepted");
    make_header("Admin", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_WRONG, "user Admin accepted");
    make_header("", "secret", hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_WRONG, "empty user accepted");

    /* No colon at all in the decoded credential. */
    char b64[64];
    b64enc("adminsecret", b64, 1);
    snprintf(hdr, sizeof(hdr), "Basic %s", b64);
    CHECK(db_auth_basic_check(hdr, "secret") == DB_AUTH_WRONG, "colonless accepted");

    /* Other schemes, malformed schemes, scheme-only. */
    CHECK(db_auth_basic_check("Bearer abc123", "secret") == DB_AUTH_WRONG,
          "Bearer accepted");
    CHECK(db_auth_basic_check("Basic", "secret") == DB_AUTH_WRONG,
          "bare scheme accepted");
    CHECK(db_auth_basic_check("Basic ", "secret") == DB_AUTH_WRONG,
          "scheme + space accepted");
    CHECK(db_auth_basic_check("Basicx YWRtaW46c2VjcmV0", "secret") == DB_AUTH_WRONG,
          "glued scheme accepted");

    /* Malformed base64: bad characters, embedded whitespace, broken padding,
     * data after the padding. A strict decoder refuses each. */
    CHECK(db_auth_basic_check("Basic !!!!", "secret") == DB_AUTH_WRONG, "junk b64");
    CHECK(db_auth_basic_check("Basic YWRt aW46c2VjcmV0", "secret") == DB_AUTH_WRONG,
          "inner space survived");
    CHECK(db_auth_basic_check("Basic YQ=X", "secret") == DB_AUTH_WRONG,
          "data after padding survived");
    CHECK(db_auth_basic_check("Basic YQ=", "secret") == DB_AUTH_WRONG,
          "short-padded 'a' accepted as a credential");
    CHECK(db_auth_basic_check("Basic Y", "secret") == DB_AUTH_WRONG,
          "dangling 6 bits accepted");

    /* Fail CLOSED on caller bugs: an empty configured password never matches,
     * not even "admin:". */
    b64enc("admin:", b64, 1);
    snprintf(hdr, sizeof(hdr), "Basic %s", b64);
    CHECK(db_auth_basic_check(hdr, "") == DB_AUTH_WRONG, "empty config pw open");
    CHECK(db_auth_basic_check(hdr, NULL) == DB_AUTH_WRONG, "NULL config pw open");

    /* Over-long configured password (contract violation) fails closed too. */
    char pw65[66];
    memset(pw65, 'x', 65); pw65[65] = '\0';
    make_header("admin", pw65, hdr, sizeof(hdr));
    CHECK(db_auth_basic_check(hdr, pw65) == DB_AUTH_WRONG, "65-char pw accepted");
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
    test_auth_accepts();
    test_auth_refuses();
    test_ct_equal();
    test_pem_scan_accepts();
    test_pem_scan_refuses();

    printf("test_http_auth: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
