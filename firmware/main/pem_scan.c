/* pem_scan.c - see pem_scan.h. Pure C, compiled unmodified into the host
 * tests; no framework includes. */
#include "pem_scan.h"

#include <string.h>

/* The PEM labels this module distinguishes. PKCS#8 ("PRIVATE KEY"), SEC1
 * ("EC PRIVATE KEY") and PKCS#1 ("RSA PRIVATE KEY") are all keys mbedTLS can
 * parse; "ENCRYPTED PRIVATE KEY" is recognized only to be refused with a
 * useful sentence — the box has nowhere to ask for a passphrase. */
static const char BEGIN_CERT[]    = "-----BEGIN CERTIFICATE-----";
static const char END_CERT[]      = "-----END CERTIFICATE-----";
static const char BEGIN_ENC_KEY[] = "-----BEGIN ENCRYPTED PRIVATE KEY-----";
static const char *const BEGIN_KEYS[] = {
    "-----BEGIN PRIVATE KEY-----",
    "-----BEGIN EC PRIVATE KEY-----",
    "-----BEGIN RSA PRIVATE KEY-----",
};
#define BEGIN_KEY_KINDS (sizeof(BEGIN_KEYS) / sizeof(BEGIN_KEYS[0]))

/* memmem for a bounded, non-NUL-terminated haystack (memmem itself is not in
 * C99, and the request body is length-counted, not NUL-terminated). */
static const char *scan_find(const char *hay, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    return NULL;
}

static int scan_count(const char *hay, size_t hlen, const char *needle)
{
    int n = 0;
    size_t nlen = strlen(needle);
    const char *p = hay;
    size_t left = hlen;
    const char *hit;
    while ((hit = scan_find(p, left, needle)) != NULL) {
        n++;
        left -= (size_t)(hit - p) + nlen;
        p = hit + nlen;
    }
    return n;
}

int db_pem_cert_count(const char *pem, size_t len)
{
    if (!pem) return 0;
    return scan_count(pem, len, BEGIN_CERT);
}

static int key_block_count(const char *pem, size_t len)
{
    int n = 0;
    for (size_t k = 0; k < BEGIN_KEY_KINDS; k++)
        n += scan_count(pem, len, BEGIN_KEYS[k]);
    /* "BEGIN PRIVATE KEY" is a substring of neither EC nor RSA variant (they
     * differ before the label), and vice versa, so plain summing is exact. */
    return n;
}

const char *db_pem_scan_pair(const char *cert, size_t cert_len,
                             const char *key, size_t key_len)
{
    if (!cert || cert_len == 0)
        return "cert_pem is empty — paste the server certificate in PEM form "
               "(\"-----BEGIN CERTIFICATE-----\")";
    if (!key || key_len == 0)
        return "key_pem is empty — paste the private key in PEM form "
               "(\"-----BEGIN PRIVATE KEY-----\")";

    /* ---- the certificate field ---- */
    int certs_in_cert = scan_count(cert, cert_len, BEGIN_CERT);
    if (certs_in_cert == 0) {
        if (key_block_count(cert, cert_len) > 0)
            return "cert_pem contains a private key, not a certificate — "
                   "the two fields look swapped";
        return "cert_pem holds no \"-----BEGIN CERTIFICATE-----\" block — "
               "it must be PEM, not DER or a file path";
    }
    if (scan_count(cert, cert_len, END_CERT) != certs_in_cert)
        return "cert_pem has mismatched BEGIN/END CERTIFICATE markers — "
               "a block was truncated in the paste";
    if (key_block_count(cert, cert_len) > 0)
        return "cert_pem also contains a private key — send the key only in "
               "key_pem, never alongside the certificate";
    /* Leaf-first is a convention this scan cannot verify (issuer/subject live
     * inside the DER), but the FIRST block being a certificate at all can be:
     * anything before the first BEGIN other than whitespace/comments is fine
     * per RFC 7468, so nothing further to refuse here. */

    /* ---- the key field ---- */
    if (scan_find(key, key_len, BEGIN_ENC_KEY))
        return "key_pem is an encrypted private key — the box cannot ask for "
               "a passphrase; export the key unencrypted and try again";
    int keys_in_key = key_block_count(key, key_len);
    if (keys_in_key == 0) {
        if (scan_count(key, key_len, BEGIN_CERT) > 0)
            return "key_pem contains a certificate, not a private key — "
                   "the two fields look swapped";
        return "key_pem holds no private-key block — it must be an "
               "unencrypted PEM key (PKCS#8 \"PRIVATE KEY\", \"EC PRIVATE "
               "KEY\" or \"RSA PRIVATE KEY\")";
    }
    if (keys_in_key > 1)
        return "key_pem contains more than one private key — send exactly "
               "the one that matches the certificate";
    if (scan_count(key, key_len, BEGIN_CERT) > 0)
        return "key_pem also contains a certificate — send certificates only "
               "in cert_pem (leaf first, then any chain)";

    return NULL;   /* structurally plausible — mbedTLS decides from here */
}
