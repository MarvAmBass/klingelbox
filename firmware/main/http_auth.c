/* http_auth.c - see http_auth.h. Pure C, no framework includes: this file is
 * compiled unmodified into host-test/test_http_auth. */
#include "http_auth.h"

#include <string.h>

/* "admin:" + password. */
#define AUTH_USER_PREFIX     "admin:"
#define AUTH_USER_PREFIX_LEN 6

/* Widest credential we ever compare: "admin:" + a DB_AUTH_PASS_MAX password.
 * Also the fixed width of the constant-time walk, so the compare cost does not
 * depend on the configured password's length either. */
#define AUTH_CRED_MAX (AUTH_USER_PREFIX_LEN + DB_AUTH_PASS_MAX)

/* Decoded base64 payload buffer: the credential plus slack, so an attacker
 * sending a slightly-too-long guess still gets the constant-time treatment
 * instead of a cheap length-based early out at the decode stage. */
#define AUTH_DECODE_MAX (AUTH_CRED_MAX + 32)

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
 * several accepted spellings, which is free extra tries per rate-limit slot. */
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

db_auth_result_t db_auth_basic_check(const char *authorization,
                                     const char *password)
{
    /* Fail CLOSED on a caller bug: an empty password means "auth disabled",
     * and that decision belongs to the caller's short-circuit, never to a
     * string comparison that would then accept "admin:". */
    if (!password || !password[0]) return DB_AUTH_WRONG;
    size_t plen = strlen(password);
    if (plen > DB_AUTH_PASS_MAX) return DB_AUTH_WRONG;

    if (!authorization || !authorization[0]) return DB_AUTH_MISSING;

    /* Scheme: "Basic", case-insensitive (RFC 7617), then at least one space. */
    static const char scheme[] = "basic";
    size_t si = 0;
    const char *p = authorization;
    while (*p == ' ' || *p == '\t') p++;
    for (; si < sizeof(scheme) - 1; si++, p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != scheme[si]) return DB_AUTH_WRONG;
    }
    if (*p != ' ' && *p != '\t') return DB_AUTH_WRONG;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return DB_AUTH_WRONG;

    unsigned char decoded[AUTH_DECODE_MAX];
    int dlen = b64_decode(p, strlen(p), decoded, sizeof(decoded));
    if (dlen < 0) return DB_AUTH_WRONG;

    char expected[AUTH_CRED_MAX + 1];
    memcpy(expected, AUTH_USER_PREFIX, AUTH_USER_PREFIX_LEN);
    memcpy(expected + AUTH_USER_PREFIX_LEN, password, plen);
    size_t elen = AUTH_USER_PREFIX_LEN + plen;

    bool ok = db_auth_ct_equal((const char *)decoded, (size_t)dlen,
                               expected, elen, sizeof(decoded));

    /* The expected credential contains the live password — do not leave it
     * on the stack for the next frame to read. (memset is fine here: the
     * array is passed to a function above, so it cannot be elided.) */
    memset(expected, 0, sizeof(expected));
    memset(decoded, 0, sizeof(decoded));

    return ok ? DB_AUTH_OK : DB_AUTH_WRONG;
}
