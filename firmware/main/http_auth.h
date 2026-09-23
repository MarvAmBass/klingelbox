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
 * same. (The non-blocking failure lockout in db_auth_gate_check is the second
 * half of the defense — it blunts throughput, this removes the oracle.)
 */
#ifndef DB_HTTP_AUTH_H
#define DB_HTTP_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The password length ceiling the API enforces (64 chars + NUL, matching
 * DB_STR_PASS in db_config.h without dragging that header in here). */
#define DB_AUTH_PASS_MAX 64

/* How long one failed guess closes the auth gate for, and therefore the
 * online guessing cap: one EVALUATED guess per window, ~3/s box-wide. */
#define DB_AUTH_LOCKOUT_MS 300

typedef enum {
    DB_AUTH_OK = 0,        /* header present, credentials correct        */
    DB_AUTH_MISSING,       /* no Authorization header / empty value      */
    DB_AUTH_WRONG,         /* malformed scheme, bad base64, or bad creds */
    DB_AUTH_LOCKED,        /* inside the failure lockout window — the
                              credential was NOT evaluated at all        */
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
 * db_auth_basic_check plus the brute-force lockout, in one call. This is the
 * gate http_api.c actually uses; the state is one caller-owned tick.
 *
 * Inside the window (`now_ms < *lockout_until_ms`) NOTHING is evaluated —
 * every request, the correct password included, is DB_AUTH_LOCKED. Outside
 * it the credential is checked, and a WRONG result (bad password, bad
 * base64, bad scheme — anything that presented a header) re-arms the window
 * to now + DB_AUTH_LOCKOUT_MS. That caps online guessing at one evaluated
 * guess per window without ever sleeping: the old implementation burned the
 * same 300 ms as a vTaskDelay on esp_http_server's single worker task, which
 * handed any unauthenticated peer a free stall-the-whole-UI primitive.
 *
 * Two deliberate asymmetries, both reasoned rather than inherited:
 *
 *  - MISSING does not arm the window. A request without an Authorization
 *    header carries no guess to rate-limit — and it is exactly what any
 *    hostile cross-origin page can fire in a loop (a no-preflight GET cannot
 *    set the header), so letting it arm the window would let such a page
 *    lock the real admin out of the API from a browser tab.
 *  - In-window refusals do not EXTEND the window. A hammering client gets
 *    exactly one evaluated guess per window; extension would evaluate none
 *    of theirs but also none of the admin's, forever.
 *
 * `now_ms` is any monotonic millisecond clock (esp_timer on the device, a
 * plain integer in the host tests); `*lockout_until_ms` starts at 0.
 */
db_auth_result_t db_auth_gate_check(const char *authorization,
                                    const char *password,
                                    int64_t now_ms,
                                    int64_t *lockout_until_ms);

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
