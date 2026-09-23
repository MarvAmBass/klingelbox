/*
 * pw_hash.h - Password hashing for the web credential: SHA-256, HMAC-SHA256
 * and PBKDF2-HMAC-SHA256, plus the stored-record layout and the iteration
 * calibration rule.
 *
 * PURE LOGIC, NO FRAMEWORK — the same contract as http_auth.h: this module
 * includes nothing from ESP-IDF (and nothing from mbedTLS) so the host-test
 * suite runs the exact bytes the device runs, pinned against the published
 * NIST/RFC test vectors (host-test/test_http_auth.c). Rolling the primitives
 * by hand is normally the wrong call; here it is the price of testability,
 * and SHA-256 (FIPS 180-4), HMAC (RFC 2104) and PBKDF2 (RFC 8018) are fully
 * specified constructions with public vectors that the tests verify against.
 * Everything is plain C99 on fixed-width types with zero heap use and well
 * under 1 KB of stack, which is also what lets the verify run directly on
 * the httpd worker task (see api_auth_ok in http_api.c).
 *
 * WHY PBKDF2 AND NOT THE PLAINTEXT. Through v0.8.0 the web password was
 * stored as-is in the config blob: anyone who could read NVS (USB cable,
 * esptool, a discarded board) read the password — which people reuse. Stored
 * now: a random 16-byte salt, an iteration count and the 32-byte
 * PBKDF2-HMAC-SHA256 of the password. The flash dump still surrenders the
 * box (physical access always did), but no longer the credential itself.
 *
 * THE ITERATION COUNT IS CALIBRATED, NOT CONSTANT. PBKDF2 is slow on this
 * chip (the design notes measured ~140 us/iteration via mbedTLS on an
 * ESP32-S3; this software implementation is in the same order), so the
 * desktop-grade "hundreds of thousands of iterations" would take minutes.
 * Instead the count is chosen AT SET-TIME to cost roughly
 * DB_PW_TARGET_VERIFY_MS of wall clock on this box, measured with a short
 * probe run, and clamped to [DB_PW_ITERS_MIN, DB_PW_ITERS_MAX] so a timing
 * glitch can neither produce a trivially-cheap hash nor a minute-long login.
 * The chosen count is STORED IN THE RECORD: a hash written by an older or
 * slower build keeps verifying forever, whatever a later calibration picks.
 */
#ifndef DB_PW_HASH_H
#define DB_PW_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DB_PW_SALT_LEN 16
#define DB_PW_HASH_LEN 32

/* Calibration bounds. ~1 s per cold verify is the sweet spot the design
 * notes argue for: costly enough that the verify itself is the online
 * brute-force rate limit (~86k guesses/day box-wide, against a 1..64-char
 * password), cheap enough that a human's one real login does not feel
 * broken. The clamp keeps a mismeasured probe (scheduler hiccup, cache-cold
 * first run) within sane cost either way. */
#define DB_PW_TARGET_VERIFY_MS 1000
#define DB_PW_ITERS_MIN        4000
#define DB_PW_ITERS_MAX        20000

/* The stored credential record — embedded in db_config_t (v3). iters == 0
 * means "no password set"; salt and hash are meaningless then. */
typedef struct {
    uint32_t iters;                   /* 0 = no password stored             */
    uint8_t  salt[DB_PW_SALT_LEN];    /* from the hardware RNG at set-time  */
    uint8_t  hash[DB_PW_HASH_LEN];    /* PBKDF2-HMAC-SHA256(password)       */
} db_pw_rec_t;

/* One-shot SHA-256 (FIPS 180-4). */
void db_pw_sha256(const void *data, size_t len, uint8_t out[DB_PW_HASH_LEN]);

/* HMAC-SHA256 (RFC 2104). */
void db_pw_hmac_sha256(const void *key, size_t key_len,
                       const void *msg, size_t msg_len,
                       uint8_t out[DB_PW_HASH_LEN]);

/* PBKDF2-HMAC-SHA256 (RFC 8018), one 32-byte output block. `iters` must be
 * >= 1; the caller owns clamping (db_pw_pick_iterations). */
void db_pw_pbkdf2(const void *password, size_t pw_len,
                  const uint8_t *salt, size_t salt_len,
                  uint32_t iters, uint8_t out[DB_PW_HASH_LEN]);

/* Build a stored record from a plaintext password. This is THE function the
 * v2->v3 config migration feeds the old plaintext through (db_config.c) and
 * the one a password change goes through — factored out so the host tests
 * exercise the exact derivation the migration relies on. Salt and iteration
 * count are the caller's (RNG + calibration live device-side). */
void db_pw_rec_from_plaintext(db_pw_rec_t *rec, const char *password,
                              const uint8_t salt[DB_PW_SALT_LEN],
                              uint32_t iters);

/* The calibration rule, as pure arithmetic so the clamp is host-testable:
 * given that `probe_iters` iterations took `probe_us` microseconds, return
 * the count whose cost is ~DB_PW_TARGET_VERIFY_MS, clamped to
 * [DB_PW_ITERS_MIN, DB_PW_ITERS_MAX]. Degenerate probes (0 iterations, 0 us
 * — a broken or too-coarse clock) return DB_PW_ITERS_MIN: when the cost
 * cannot be measured, err on the cheap-but-sane side rather than minting a
 * login that might take minutes. */
uint32_t db_pw_pick_iterations(uint32_t probe_iters, uint64_t probe_us);

#ifdef __cplusplus
}
#endif

#endif /* DB_PW_HASH_H */
