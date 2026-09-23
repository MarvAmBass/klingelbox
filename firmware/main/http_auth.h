/*
 * http_auth.h - HTTP Basic credential check for the REST API.
 *
 * PURE LOGIC, NO FRAMEWORK. This module sees two strings — the value of an
 * Authorization header and the configured password — and answers "is this the
 * box's admin?". It includes nothing from ESP-IDF so the host-test suite can
 * exercise the parser and the comparison exactly as the device runs them
 * (host-test/test_http_auth.c). The 401 response, the failure delay and the
 * decision of WHICH routes are guarded all live in http_api.c, next to the
 * other request-gate policy.
 *
 * THE USERNAME IS HARDCODED "admin". One box, one operator, one credential:
 * a user database on a doorbell would be surface without a threat to match.
 * The web UI and docs/API.md state the username; only the password is chosen.
 *
 * THE COMPARISON IS CONSTANT-TIME over the credential bytes. A byte-by-byte
 * early-exit memcmp would let an attacker on the LAN grow a correct password
 * one byte at a time from response timing. The compare below always walks the
 * full width of the credential buffer and folds every difference into one
 * accumulator, so a first-byte mismatch and a last-byte mismatch cost the
 * same. (The ~300 ms uniform failure delay in http_api.c is the second half
 * of the defense — it blunts throughput, this removes the oracle.)
 */
#ifndef DB_HTTP_AUTH_H
#define DB_HTTP_AUTH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The password length ceiling the API enforces (64 chars + NUL, matching
 * DB_STR_PASS in db_config.h without dragging that header in here). */
#define DB_AUTH_PASS_MAX 64

typedef enum {
    DB_AUTH_OK = 0,        /* header present, credentials correct        */
    DB_AUTH_MISSING,       /* no Authorization header / empty value      */
    DB_AUTH_WRONG,         /* malformed scheme, bad base64, or bad creds */
} db_auth_result_t;

/*
 * Check `authorization` (the raw header value, or NULL/"" when the request
 * carried none) against the configured `password`.
 *
 * Accepts exactly `Basic <base64("admin:<password>")>`, scheme name
 * case-insensitive per RFC 7617, one or more spaces after the scheme. Any
 * other scheme, undecodable base64 or wrong credentials is DB_AUTH_WRONG —
 * the caller answers both MISSING and WRONG with the same 401 body, but only
 * MISSING is the "browser has not asked the user yet" state a UI may treat
 * quietly.
 *
 * `password` must be a NUL-terminated string of 1..DB_AUTH_PASS_MAX bytes.
 * An empty password means auth is disabled and is the CALLER's short-circuit;
 * passing "" here is a contract violation and returns DB_AUTH_WRONG so a
 * bookkeeping bug fails closed rather than open.
 */
db_auth_result_t db_auth_basic_check(const char *authorization,
                                     const char *password);

/*
 * Constant-time equality of two byte strings, exposed for the test suite and
 * for any future secret-bearing comparison. Reads both operands over the full
 * `width` regardless of where they differ; `width` must be >= both lengths.
 */
bool db_auth_ct_equal(const char *a, size_t alen,
                      const char *b, size_t blen, size_t width);

#ifdef __cplusplus
}
#endif

#endif /* DB_HTTP_AUTH_H */
