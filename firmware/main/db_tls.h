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

/* Load the stored identity from NVS if one exists. Never generates. Returns
 * ESP_OK whether or not an identity was found — check db_tls_ready(). A
 * stored pair that no longer validates is discarded (anti-brick), so a
 * corrupt blob degrades to "no identity yet", never to a dead HTTPS server. */
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

/* Which identity is active (meaningful only while db_tls_ready()). */
db_tls_source_t db_tls_source(void);

/* Drop the stored identity (provided or generated) from NVS and RAM. The next
 * db_tls_ensure() mints a fresh self-signed one — which BREAKS existing pins,
 * so the API warns in its response and docs/security.md explains re-pairing. */
esp_err_t db_tls_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_TLS_H */
