/*
 * pem_scan.h - Structural preflight of an uploaded TLS cert+key PEM pair.
 *
 * PURE LOGIC, NO FRAMEWORK, NO mbedTLS. This is the first of two validation
 * gates on POST /api/tls/identity. mbedTLS (in db_tls.c) is the authority on
 * whether the pair is cryptographically sound — but its parse errors are
 * numeric soup, and the mistakes people actually make when pasting PEM are
 * structural: fields swapped, a key pasted twice, an encrypted key, a chain
 * pasted into the key box. This module recognizes those shapes and returns a
 * sentence that names the mistake, so the API's 400 tells the user what to fix
 * instead of "parse error -0x2180". Being pure C, it runs verbatim in
 * host-test/test_http_auth.c, where every one of those sentences is pinned.
 *
 * IT NEVER APPROVES ANYTHING BY ITSELF. Passing this scan only means "worth
 * handing to mbedTLS"; the full parse + key-matches-certificate check still
 * decides. A malicious-but-well-shaped input gets past this module and is
 * rejected by the next one, so laxity here costs nothing but error quality.
 */
#ifndef DB_PEM_SCAN_H
#define DB_PEM_SCAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Scan `cert` (one leaf certificate, optionally followed by chain
 * certificates — leaf first) and `key` (exactly one unencrypted private key).
 *
 * Returns NULL when the pair is structurally plausible, otherwise a static
 * human sentence describing the first problem found — suitable verbatim as an
 * API {"error": ...} body. Lengths are the used byte counts (no NUL needed).
 */
const char *db_pem_scan_pair(const char *cert, size_t cert_len,
                             const char *key, size_t key_len);

/* Count "-----BEGIN CERTIFICATE-----" blocks (for logging a chain's depth). */
int db_pem_cert_count(const char *pem, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DB_PEM_SCAN_H */
