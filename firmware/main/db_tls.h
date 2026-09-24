/*
 * db_tls.h - The box's own TLS identity (certificate + private key).
 *
 * The keypair is generated ON the device and never leaves it. The public
 * certificate is served unauthenticated at GET /cert.pem so a client can pin
 * it on first pairing (trust-on-first-use); GET /api/config reports its
 * SHA-256 fingerprint so the pin can be checked out-of-band.
 *
 * Choices, and why (docs/security.md carries the full reasoning):
 *
 *  - ECDSA P-256, not RSA. The ESP32-S3 has AES/SHA/MPI(RSA) accelerators but
 *    NO ECC accelerator, so ECDSA runs in software — and is still roughly an
 *    order of magnitude faster per handshake than hardware-accelerated
 *    RSA-4096. Since clients pin the certificate, big RSA keys would buy
 *    nothing anyway: pinning removes forge-a-cert-via-a-CA from the threat
 *    model entirely.
 *
 *  - FIXED validity: notBefore 2026-01-01, notAfter 30 years later. This box
 *    deliberately has no SNTP and no RTC (every timestamp in the API is
 *    uptime-seconds for that reason), so time(NULL) is meaningless here and
 *    stamping "now" would mint a certificate valid from 1970. A constant
 *    notBefore safely in the past and a notAfter that outlives the hardware
 *    is the honest shape for a device with no clock. 30 years rather than
 *    100 because some TLS stacks still reject absurdly distant dates.
 *
 *  - Generated LAZILY, on the first TLS enable — never at boot. Two reasons:
 *    a box that never enables TLS should never pay the ~24 KB NVS partition
 *    of the oldest fielded units for a keypair it does not use, and key
 *    generation needs the hardware RNG, which is only a true RNG once the RF
 *    subsystem is up (Wi-Fi starts before the HTTP server, so any request
 *    that can trigger generation runs with real entropy).
 *
 * An operator-supplied cert+key (POST /api/tls/identity) replaces the
 * generated one and is validated COMPLETELY before anything is persisted, so
 * a bad upload can never take the HTTPS server down. DELETE reverts to a
 * fresh on-device identity.
 *
 * REPLACEMENT + NOTICE. A STORED pair — provided or generated — can still
 * fail validation at load; the concrete provided case from our own history
 * is a pair accepted under an older firmware's rules that a newer strength
 * floor refuses (v0.8.0 took RSA-1024; v0.9.0's floor does not), the
 * generated case is flash corruption. Either way the pair is REPLACED:
 * erased, and the next db_tls_ensure() mints and persists the usual
 * self-signed identity (TLS stays on — no downgrade, and the generated
 * fingerprint is stable across boots, so it can be pinned). Erasing an
 * operator's upload is honest, not rude: this system has no retrieval path
 * for it — /cert.pem serves the ACTIVE certificate and the API never returns
 * private keys — so keeping the bytes could never help anyone, and the
 * uploader still has the originals (the full why sits at the decision point
 * in db_tls.c). What survives instead is a small persisted NOTICE —
 * db_tls_custom_rejected()'s flag, a one-sentence reason, and WHICH KIND of
 * pair was replaced (db_tls_rejected_source) — so the API and UI can say
 * what happened boots later. Uniform across both kinds on purpose: any
 * replacement gets explained — a changed fingerprint must never be a
 * mystery. A successful new upload or an explicit acknowledge
 * (db_tls_ack_rejection) ends the notice; a reboot does not.
 */
#ifndef DB_TLS_H
#define DB_TLS_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the active identity came from — the `source` string in the API. */
typedef enum {
    DB_TLS_GENERATED = 0,   /* self-signed, minted on this box (the default) */
    DB_TLS_PROVIDED  = 1,   /* uploaded via POST /api/tls/identity           */
} db_tls_source_t;

/* The two mbedTLS-verdict sentences, defined once so the upload 400 in
 * http_api.c and the boot-time rejected_reason (the notice) use the SAME
 * words — the person who reads one at upload time recognizes the other at
 * boot time. */
#define DB_TLS_MSG_PAIR_INVALID \
    "the pair did not validate: a certificate or the key failed to parse, " \
    "or the key does not match the (first) certificate — the chain must " \
    "be leaf-first"
#define DB_TLS_MSG_KEY_WEAK \
    "the private key is too weak for a TLS server: the floor is 2048 bits " \
    "for RSA and 255 bits for EC (P-256 and up)"

/* Longest reason is a pem_scan sentence (~140 bytes of UTF-8), with room. */
#define DB_TLS_REJECT_REASON_MAX 192

/* Load the stored identity from NVS if one exists. Never generates. Returns
 * ESP_OK whether or not an identity was found — check db_tls_ready(). A
 * stored pair that no longer validates degrades to "no identity yet", never
 * to a dead HTTPS server (anti-brick): the pair — provided or generated —
 * is erased with a persisted notice left in its place — see the file header
 * and db_tls_custom_rejected(). Also reloads any standing notice from
 * NVS. */
esp_err_t db_tls_load(void);

/* True once a certificate + key are in RAM (loaded, generated or installed). */
bool db_tls_ready(void);

/* Load-or-generate: the lazy path POST /api/config takes on the first TLS
 * enable. Blocking for roughly 100-300 ms when it has to generate (software
 * P-256 keygen + self-signing). `hostname` becomes the certificate CN — for
 * human readability only; pinning clients never check the name.
 * ESP_ERR_NVS_NOT_ENOUGH_SPACE when the identity cannot be persisted. */
esp_err_t db_tls_ensure(const char *hostname);

/* Borrowed pointers, valid until the next set/clear/ensure. NUL-terminated;
 * *len INCLUDES the NUL, which is what esp_https_server wants for PEM.
 * MAIN-SERVER-TASK ONLY (server start/restart): the buffers behind these
 * pointers are mutated by set/clear on that same task, so any OTHER task
 * must go through db_tls_dup_cert_pem instead. */
esp_err_t db_tls_get_cert_pem(const char **pem, size_t *len);
esp_err_t db_tls_get_key_pem(const char **pem, size_t *len);

/* Heap COPY of the certificate PEM, taken under the module lock — the one
 * certificate read that is safe from any task (the :80 redirect helper
 * serves GET /cert.pem on its own httpd task while the main task may be
 * installing or clearing an identity). *len EXCLUDES the NUL; the caller
 * free()s *pem. ESP_ERR_INVALID_STATE when no identity exists,
 * ESP_ERR_NO_MEM when the copy cannot be allocated. */
esp_err_t db_tls_dup_cert_pem(char **pem, size_t *len);

/* Lowercase hex SHA-256 of the DER certificate — the value a client pins.
 * Needs out_sz >= 65. */
esp_err_t db_tls_get_fingerprint(char *out, size_t out_sz);

/*
 * Install an operator-supplied certificate + private key (both PEM). The pair
 * is FULLY validated before anything is persisted — certificate(s) parse, key
 * parses, the key matches the LEAF certificate's public key, and the key
 * meets the strength floor (RSA >= 2048 bits, EC >= 255 bits) — so a
 * rejected upload leaves the active identity untouched. A multi-certificate
 * PEM (leaf first, then chain) is accepted. Returns ESP_ERR_INVALID_ARG when
 * parsing/matching fails, ESP_ERR_NOT_SUPPORTED when the key is below the
 * floor (or of a type no TLS server key should be), and
 * ESP_ERR_NVS_NOT_ENOUGH_SPACE when NVS is full.
 */
esp_err_t db_tls_set_identity(const char *cert_pem, size_t cert_len,
                              const char *key_pem, size_t key_len);

/* Which identity is active (meaningful only while db_tls_ready()). After a
 * rejected upload was replaced this reports DB_TLS_GENERATED — that IS what
 * is served and stored; the replacement's story is db_tls_custom_rejected()'s
 * to tell. */
db_tls_source_t db_tls_source(void);

/* True while the persisted rejected notice stands: a STORED pair — provided
 * or generated — failed load-time validation and was replaced by a persisted
 * generated identity (see the file header). When true and reason_out is
 * non-NULL, copies the one-sentence human reason (at most
 * DB_TLS_REJECT_REASON_MAX bytes including the NUL) — either a pem_scan.c
 * structural sentence or one of the DB_TLS_MSG_* verdicts above. Takes the
 * module lock; safe from any task. Cleared by a successful
 * db_tls_set_identity() (the fixed re-upload) or by db_tls_ack_rejection()
 * (the explicit dismissal) — never by a reboot, which reloads it from NVS. */
bool db_tls_custom_rejected(char *reason_out, size_t reason_sz);

/* WHICH KIND of stored pair the standing notice replaced — the
 * `rejected_source` string in the API, and what lets the UI say "the
 * certificate you uploaded" versus "the box's stored certificate".
 * Meaningful only while db_tls_custom_rejected() is true. A notice persisted
 * before the source marker existed reads as DB_TLS_PROVIDED: only
 * provided-pair notices could exist back then. Takes the module lock; safe
 * from any task. */
db_tls_source_t db_tls_rejected_source(void);

/* True only on the boot whose db_tls_load() DETECTED the rejection (and so
 * performed the replacement), false when a standing notice was merely
 * reloaded — the difference between "announce the replacement once" and
 * "nag every boot" for the event feed. */
bool db_tls_rejection_is_fresh(void);

/* Dismiss the rejected notice without touching the active identity — the
 * DELETE /api/tls/rejection acknowledgment. Erases the persisted record so
 * the dismissal outlives a reboot; idempotent. */
esp_err_t db_tls_ack_rejection(void);

/* Drop the stored identity (provided or generated) from NVS and RAM. The next
 * db_tls_ensure() mints a fresh self-signed one — which BREAKS existing pins,
 * so the API warns in its response and docs/security.md explains re-pairing.
 * A standing rejected notice is deliberately NOT cleared — it has its own
 * dismissals (upload, ack), and a key rotation should not swallow the
 * explanation of why the previous upload vanished. */
esp_err_t db_tls_clear(void);

#ifdef DB_HOSTTEST
/* Host tests only (host-test/Makefile defines DB_HOSTTEST): forget the
 * resident state so db_tls_load() can run again, simulating a reboot against
 * whatever the fake flash holds. Does not exist in a device build. */
void db_tls_hosttest_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DB_TLS_H */
