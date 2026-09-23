/*
 * http_auth.h - HTTP Basic credential check for the REST API.
 *
 * PURE LOGIC, NO FRAMEWORK. This module sees the value of an Authorization
 * header, the stored password record and the per-boot cache, and answers "is
 * this the box's admin?". It includes nothing from ESP-IDF so the host-test
 * suite can exercise the parser, the slow verify and the cache state machine
 * exactly as the device runs them (host-test/test_http_auth.c). The 401
 * response and the decision of WHICH routes are guarded live in http_api.c,
 * next to the other request-gate policy.
 *
 * THE USERNAME IS HARDCODED "admin". One box, one operator, one credential:
 * a user database on a doorbell would be surface without a threat to match.
 * The web UI and docs/API.md state the username; only the password is chosen.
 *
 * THE PASSWORD IS STORED AS A SALTED PBKDF2 HASH (pw_hash.h), and the hash's
 * ~1 s cost is BOTH halves of the brute-force defense:
 *
 *   - Offline: a flash dump yields salt+hash, not the credential (people
 *     reuse passwords; the box was already forfeit to whoever holds it).
 *   - Online: every evaluated wrong guess costs the full PBKDF2 run on the
 *     box, ~1 s each, box-wide — that IS the rate limit (~1 guess/s against
 *     64 chars of keyspace). The 300 ms lockout window that previously
 *     capped guessing is GONE, deliberately: it refused the CORRECT password
 *     unevaluated for 300 ms after any wrong guess, so one hostile client
 *     interleaving misses could deny the real admin indefinitely — the exact
 *     failure the design notes call out. Cost-per-guess punishes only the
 *     guesser; it cannot be aimed at the admin's own correct login.
 *
 * THE PER-BOOT SUCCESS CACHE is what keeps the API usable despite that cost.
 * HTTP Basic re-sends the credential on EVERY request; a full PBKDF2 per
 * request would turn the UI into a slideshow. After one full verify the box
 * caches SHA256(boot_nonce || credential) — the nonce is drawn fresh each
 * boot, so the cached digest is useless off-device and worthless after a
 * reboot — and every later request is one SHA-256 plus a constant-time
 * compare. A WRONG credential never enters the cache, so wrong guesses
 * always pay full price; only the admin's correct credential ever gets the
 * fast path.
 *
 * ALL COMPARISONS ARE CONSTANT-TIME over fixed widths (db_auth_ct_equal): a
 * byte-by-byte early-exit memcmp would let a LAN attacker grow a matching
 * digest byte-wise from response timing. The PBKDF2 duration itself depends
 * only on the stored iteration count, never on the guess.
 */
#ifndef DB_HTTP_AUTH_H
#define DB_HTTP_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pw_hash.h"

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
 * The per-boot success cache plus its anti-race bookkeeping. One instance,
 * owned by http_api.c, zero heap. `generation` exists for one narrow race:
 * a full verify takes ~1 s, and if the password is changed WHILE a verify
 * against the old record is in flight, the verifier must not cache (or
 * accept) a credential that the change just retired. db_auth_check snapshots
 * the counter before the slow derive and discards its own result if the
 * counter moved — see the .c for the exact rule.
 */
typedef struct {
    uint8_t  nonce[DB_PW_HASH_LEN];   /* per-boot random, never persisted   */
    bool     valid;                   /* one cached success exists          */
    uint8_t  digest[DB_PW_HASH_LEN];  /* SHA256(nonce || good credential)   */
    uint32_t generation;              /* bumped by every invalidate         */
} db_auth_cache_t;

/* Seed the cache with this boot's nonce (device: esp_random bytes; tests: a
 * constant). Starts empty. */
void db_auth_cache_init(db_auth_cache_t *cache,
                        const uint8_t nonce[DB_PW_HASH_LEN]);

/* Drop the cached success and bump the generation. MUST be called on every
 * password set, change or removal — a credential that stopped being the
 * password must stop being fast-path valid on the very next request. */
void db_auth_cache_invalidate(db_auth_cache_t *cache);

/*
 * Check `authorization` (the raw header value, or NULL/"" when the request
 * carried none) against the stored record.
 *
 * Accepts exactly `Basic <base64("admin:<password>")>`, scheme name
 * case-insensitive per RFC 7617, one or more spaces after the scheme.
 *
 * COST BY OUTCOME (the security model in one table):
 *   cached correct credential   one SHA-256               DB_AUTH_OK
 *   uncached correct credential full PBKDF2, then cached  DB_AUTH_OK
 *   wrong password / username   full PBKDF2, NOT cached   DB_AUTH_WRONG
 *   malformed header            fast fail                 DB_AUTH_WRONG
 *   no header                   fast fail                 DB_AUTH_MISSING
 *
 * Malformed (bad scheme, undecodable base64, credential outside the
 * documented "admin:" + 1..64-char shape) fails FAST on purpose: it carries
 * no password guess, so slowing it down would rate-limit nothing — while
 * burning ~1 s of the single httpd task per junk request would hand any
 * peer a cheap stall-the-whole-UI primitive. The fast/slow timing split
 * only tells an attacker their request was malformed, which the uniform 401
 * body already implies and API.md documents outright. Every WELL-FORMED
 * guess costs the full derive, wrong username included ("admin" is public;
 * folding it into the slow compare keeps wrong-user and wrong-password
 * indistinguishable by timing).
 *
 * `rec` must hold a stored password (iters != 0). "No password set" is the
 * CALLER's short-circuit to open access; passing an empty record here is a
 * contract violation and returns DB_AUTH_WRONG so a bookkeeping bug fails
 * closed rather than open. Decoded credential bytes are zeroed before
 * return.
 */
db_auth_result_t db_auth_check(const char *authorization,
                               const db_pw_rec_t *rec,
                               db_auth_cache_t *cache);

/*
 * Constant-time equality of two byte strings, exposed for the test suite and
 * for any future secret-bearing comparison. Reads both operands over the full
 * `width` regardless of where they differ; `width` must be >= both lengths.
 */
bool db_auth_ct_equal(const char *a, size_t alen,
                      const char *b, size_t blen, size_t width);

#ifdef DB_HOSTTEST
/* Test hook, fired between the generation snapshot and the slow derive's
 * cache-store decision — the only way a single-threaded test can land a
 * password change INSIDE the verify window and pin the race rule down. */
extern void (*db_auth_test_after_derive)(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DB_HTTP_AUTH_H */
