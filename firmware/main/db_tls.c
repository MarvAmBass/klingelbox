/*
 * db_tls.c - see db_tls.h.
 *
 * STORAGE. Its own NVS namespace ("db_tls"), four keys: cert, key, src, plus
 * rej_why (the small rejected-notice record, see below). NOT
 * part of the db_config blob on purpose — the PEM pair is ~1.1 KB (generated)
 * up to ~8 KB (provided chain), and folding that into the config blob would
 * make every config save rewrite it. Deliberately goes through the ordinary
 * NVS error path rather than assuming space: the oldest fielded boxes still
 * run the 24 KB NVS partition (the table is never rewritten by OTA), where a
 * generated pair fits comfortably but a full provided chain plus a full
 * signal store might not — ESP_ERR_NVS_NOT_ENOUGH_SPACE propagates out and
 * http_api turns it into the same 507 every other storage-full path gets.
 */
#include "db_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"

#include "pem_scan.h"

static const char *TAG = "db_tls";

/* Sized for OPERATOR-supplied identities, not just our own P-256 leaf (~1.1 KB
 * for the pair): an RSA private key PEM is ~3.2 KB and a provided certificate
 * may be a leaf plus chain. Static BSS, not any task's stack. */
#define DB_TLS_CERT_MAX 6144
#define DB_TLS_KEY_MAX  4096

#define DB_TLS_NS   "db_tls"
#define NVS_K_CERT  "cert"
#define NVS_K_KEY   "key"
#define NVS_K_SRC   "src"     /* u8 db_tls_source_t */
#define NVS_K_REJ   "rej_why" /* blob: NUL-terminated rejected-notice reason
                                 sentence; the key's PRESENCE is the
                                 custom_rejected flag — a notice always has a
                                 reason, so a separate flag would only add a
                                 way for the two to disagree */

/*
 * FIXED validity window — see db_tls.h for why a box with no clock must not
 * stamp "now". notBefore is the project's TLS epoch (any date safely before
 * the first unit that can run this firmware counts); bump it only alongside
 * a change that regenerates identities anyway.
 */
#define DB_TLS_NOT_BEFORE "20260101000000"
#define DB_TLS_NOT_AFTER  "20560101000000"   /* + 30 years */

static char   s_cert_pem[DB_TLS_CERT_MAX];
static char   s_key_pem[DB_TLS_KEY_MAX];
static size_t s_cert_len;
static size_t s_key_len;
static bool   s_ready;
static db_tls_source_t s_source = DB_TLS_GENERATED;

/* REJECTED-NOTICE state (db_tls.h): a stored PROVIDED pair failed load-time
 * validation and was replaced by a persisted generated identity; the notice
 * (flag + reason sentence) is the durable record of that replacement. Mirror
 * of the NVS_K_REJ blob: written at replacement, reloaded at every boot,
 * erased only by a successful new upload or an explicit acknowledge. */
static bool s_custom_rejected;
static char s_rejected_reason[DB_TLS_REJECT_REASON_MAX];
/* RAM-only: the rejection was DETECTED during this boot's load (as opposed
 * to the notice merely being reloaded from NVS). http_api uses it to put the
 * replacement into the event feed exactly once — at replacement time, not on
 * every boot the standing notice survives. */
static bool s_rejected_this_boot;

/*
 * WHY A LOCK, when nearly everything here runs on the main server's one
 * httpd task: GET /cert.pem is ALSO served by the :80 redirect helper, a
 * second httpd instance with its own task, while POST/DELETE
 * /api/tls/identity mutate these buffers on the first. Without it a TOFU
 * client whose transfer straddles a rotation receives a chimera of old and
 * new PEM bytes — a torn pin at exactly the moment pinning matters. The
 * cross-task reader (db_tls_dup_cert_pem) and every state mutation take it;
 * created in db_tls_load, which db_http_start calls before any server task
 * exists, so lazy creation cannot race. Held only across memcpy/memset and
 * the NVS write of an install — never across keygen, never across a send.
 */
static SemaphoreHandle_t s_lock;

static void state_lock(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void state_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

/* mbedTLS entropy callback backed by the ESP hardware RNG. The trailing olen
 * out-parameter is part of mbedtls_entropy_f_source_ptr; esp_fill_random
 * always fills the whole buffer, so olen is simply len. */
static int esp_entropy(void *ctx, unsigned char *buf, size_t len, size_t *olen)
{
    (void)ctx;
    esp_fill_random(buf, len);
    *olen = len;
    return 0;
}

/* mbedTLS parses PEM as a C string and wants the terminating NUL counted in
 * the length it is given. */
static size_t pem_len_with_nul(const char *pem, size_t len)
{
    if (len > 0 && pem[len - 1] == '\0') return len;
    return len + 1;
}

/* ------------------------------------------------------------ validation */

/*
 * Validate a cert+key pair with mbedTLS: the certificate(s) must parse, the
 * private key must parse, and the key must match the LEAF certificate's
 * public key (crt.pk is the first certificate parsed, which is why the API
 * demands leaf-first chains). This is the gate that makes accepting operator
 * input safe — an identity that would make the HTTPS server refuse to start
 * is rejected here, before it is ever persisted or activated.
 */
static esp_err_t validate_pair(const char *cert, size_t clen,
                               const char *key, size_t klen)
{
    const size_t csz = pem_len_with_nul(cert, clen);
    const size_t ksz = pem_len_with_nul(key, klen);

    mbedtls_x509_crt         crt;
    mbedtls_pk_context       pk;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&pk);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);

    esp_err_t ret = ESP_ERR_INVALID_ARG;

    mbedtls_entropy_add_source(&entropy, esp_entropy, NULL, 32,
                               MBEDTLS_ENTROPY_SOURCE_STRONG);
    const char *pers = "db_tls_validate";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        goto out;
    }
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert, csz) != 0) {
        ESP_LOGW(TAG, "certificate did not parse");
        goto out;
    }
    /* mbedTLS 3.x requires an RNG for private-key parsing (blinding). */
    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)key, ksz, NULL, 0,
                             mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGW(TAG, "private key did not parse");
        goto out;
    }
    if (mbedtls_pk_check_pair(&crt.pk, &pk, mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGW(TAG, "private key does not match the leaf certificate");
        goto out;
    }

    /* STRENGTH FLOOR. mbedTLS parses (and would happily serve) an RSA-512
     * key: its 2048-bit x509 profile floor governs CHAIN VERIFICATION only,
     * never the server's own key. A factorable key lets a LAN attacker
     * impersonate the box to every pinned client and harvest the Basic
     * credentials TLS is recommended to protect — so the same gate that
     * keeps a broken upload from killing the server also refuses a weak one:
     * RSA below 2048 bits, EC below 255 bits (P-256 and up; our own
     * generated identity is P-256 = 256), any other key type. Distinct
     * error code so the API can name the floor instead of blaming a parse. */
    {
        mbedtls_pk_type_t kt = mbedtls_pk_get_type(&pk);
        size_t bits = mbedtls_pk_get_bitlen(&pk);
        bool weak;
        if (kt == MBEDTLS_PK_RSA)
            weak = bits < 2048;
        else if (kt == MBEDTLS_PK_ECKEY || kt == MBEDTLS_PK_ECDSA)
            weak = bits < 255;
        else
            weak = true;   /* nothing else belongs in a TLS server key */
        if (weak) {
            ESP_LOGW(TAG, "key too weak for a TLS server: type %d, %u bits "
                          "(floor: RSA 2048, EC 255)", (int)kt, (unsigned)bits);
            ret = ESP_ERR_NOT_SUPPORTED;
            goto out;
        }
    }
    ret = ESP_OK;

out:
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&crt);
    return ret;
}

/* ------------------------------------------------------------ persistence */

static esp_err_t persist(db_tls_source_t source)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_TLS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t src = (uint8_t)source;
    /* Key first: it is the larger risk on a nearly-full 24 KB partition, and
     * failing before the cert is touched leaves the OLD stored pair intact
     * and consistent. */
    err = nvs_set_blob(h, NVS_K_KEY, s_key_pem, s_key_len + 1);
    if (err == ESP_OK) err = nvs_set_blob(h, NVS_K_CERT, s_cert_pem, s_cert_len + 1);
    if (err == ESP_OK) err = nvs_set_u8(h, NVS_K_SRC, src);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
        ESP_LOGE(TAG, "persisting the TLS identity failed: %s",
                 esp_err_to_name(err));
    return err;
}

/* Write the rejected-notice record. BEST-EFFORT by contract: the replacement
 * identity is what keeps HTTPS alive, the notice only explains it, so a
 * failed notice write must never block the replacement — worst case the
 * operator loses the explanation, never the server. */
static void notice_persist(const char *why)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_TLS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_K_REJ, why, strlen(why) + 1);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "could not persist the rejection notice (%s) — the "
                      "replacement proceeds without it", esp_err_to_name(err));
}

static esp_err_t notice_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_TLS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return ESP_OK;   /* no namespace, no notice */
    err = nvs_erase_key(h, NVS_K_REJ);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   /* already gone */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t db_tls_load(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();   /* see the comment at s_lock */

    nvs_handle_t h;
    if (nvs_open(DB_TLS_NS, NVS_READONLY, &h) != ESP_OK)
        return ESP_OK;   /* nothing stored — normal on every box without TLS */

    size_t clen = sizeof(s_cert_pem);
    size_t klen = sizeof(s_key_pem);
    esp_err_t ec = nvs_get_blob(h, NVS_K_CERT, s_cert_pem, &clen);
    esp_err_t ek = nvs_get_blob(h, NVS_K_KEY, s_key_pem, &klen);
    uint8_t src = DB_TLS_GENERATED;
    (void)nvs_get_u8(h, NVS_K_SRC, &src);

    /* The standing rejected notice, if any, reloads before the pair is even
     * looked at: after a replacement the stored pair validates (as GENERATED)
     * and the detection below never runs again — and after a DELETE there may
     * be no pair at all — so NVS is the only place the notice survives a
     * reboot. Erased solely by a successful upload or an explicit ack. */
    {
        size_t rlen = sizeof(s_rejected_reason);
        state_lock();
        if (nvs_get_blob(h, NVS_K_REJ, s_rejected_reason, &rlen) == ESP_OK &&
            rlen > 0) {
            s_rejected_reason[sizeof(s_rejected_reason) - 1] = '\0';
            s_custom_rejected = true;
        }
        state_unlock();
    }
    nvs_close(h);

    if (ec != ESP_OK || ek != ESP_OK) return ESP_OK;

    s_cert_pem[sizeof(s_cert_pem) - 1] = '\0';
    s_key_pem[sizeof(s_key_pem) - 1] = '\0';
    s_cert_len = strlen(s_cert_pem);
    s_key_len = strlen(s_key_pem);
    if (s_cert_len == 0 || s_key_len == 0) return ESP_OK;

    /* Anti-brick: a stored pair that no longer validates must NOT be served —
     * the HTTPS server would refuse to start with no recovery path. Either
     * way the pair is REPLACED: erased here, re-minted and PERSISTED by the
     * next db_tls_ensure(), so the box serves one stable generated identity
     * from then on instead of retrying bytes that will never start
     * validating. For GENERATED that was never controversial — flash
     * corruption is the only way our own pair gets here, nothing user-owned
     * is lost. For PROVIDED (the concrete case from our own history: a pair
     * accepted under v0.8.0's rules refused by v0.9.0's key-strength floor)
     * the replacement deserves its why spelled out, because v0.9.1 shipped
     * the opposite — keep the stored pair byte-identical "for inspection",
     * serve a RAM-only stand-in — and that design was rejected: this system
     * has NO retrieval path. /cert.pem serves the ACTIVE certificate and the
     * API never returns private keys (our own security rule), so the
     * preserved pair could never be inspected by anyone, while the
     * unpersisted stand-in changed the served fingerprint every boot and
     * broke pinning clients again and again. Preservation without retrieval
     * is pure cost; the uploader has the originals by construction, and a
     * corrupt blob is worthless. What a PROVIDED pair gets instead is a
     * persisted NOTICE naming the reason, standing until the operator
     * uploads a fixed pair or dismisses it. */
    esp_err_t verr = validate_pair(s_cert_pem, s_cert_len, s_key_pem, s_key_len);
    if (verr != ESP_OK) {
        const char *why = NULL;
        if (src == DB_TLS_PROVIDED) {
            /* Best reason first: pem_scan names a structural wound (truncated
             * block, wrong label) in a sentence; otherwise reuse the exact
             * verdict sentence the upload 400 would have used. */
            why = db_pem_scan_pair(s_cert_pem, s_cert_len,
                                   s_key_pem, s_key_len);
            if (!why)
                why = (verr == ESP_ERR_NOT_SUPPORTED) ? DB_TLS_MSG_KEY_WEAK
                                                      : DB_TLS_MSG_PAIR_INVALID;
            state_lock();
            s_custom_rejected = true;
            s_rejected_this_boot = true;
            snprintf(s_rejected_reason, sizeof(s_rejected_reason), "%s", why);
            state_unlock();
            ESP_LOGE(TAG, "stored uploaded TLS identity failed validation — "
                          "replacing it with a generated one (%s)", why);
        } else {
            ESP_LOGE(TAG, "stored generated TLS identity failed validation — "
                          "discarding it");
        }
        /* ORDER: erase first, notice second, replacement last (at the next
         * db_tls_ensure). The erase frees the pair's one-to-eight KB, which
         * on a nearly-full 24 KB partition may be exactly what lets the
         * small notice write — and the replacement identity after it —
         * succeed. Both later steps are allowed to fail without undoing the
         * erase: notice_persist is best-effort by contract, and a failed
         * mint lands in start_servers()' plain-HTTP fallback + retry. The
         * clear also scrubs the bad private key out of RAM. */
        db_tls_clear();
        if (why) notice_persist(why);
        return ESP_OK;
    }

    /* The flip under the lock, though the buffer fill above was bare: nothing
     * reads the buffers while !s_ready, and releasing the lock is what
     * publishes their contents to the reader on the other httpd task. */
    state_lock();
    s_source = (src == DB_TLS_PROVIDED) ? DB_TLS_PROVIDED : DB_TLS_GENERATED;
    s_ready = true;
    state_unlock();

    char fp[65];
    if (db_tls_get_fingerprint(fp, sizeof(fp)) == ESP_OK)
        ESP_LOGI(TAG, "loaded %s TLS identity, fingerprint %s",
                 s_source == DB_TLS_PROVIDED ? "provided" : "generated", fp);
    return ESP_OK;
}

/* ------------------------------------------------------------ generation */

static esp_err_t generate_identity(const char *hostname)
{
    esp_err_t ret = ESP_FAIL;
    const int64_t t0 = esp_timer_get_time();

    mbedtls_pk_context       key;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    mbedtls_x509write_cert   crt;

    mbedtls_pk_init(&key);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
    mbedtls_x509write_crt_init(&crt);

    /* The hardware RNG as an explicit strong source — a true RNG here because
     * Wi-Fi (and with it the RF subsystem) is up before the HTTP server that
     * can trigger this ever starts. See db_tls.h. */
    mbedtls_entropy_add_source(&entropy, esp_entropy, NULL, 32,
                               MBEDTLS_ENTROPY_SOURCE_STRONG);

    const char *pers = "db_tls_keygen";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        ESP_LOGE(TAG, "drbg seed failed");
        goto out;
    }

    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                            mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "P-256 keygen failed");
        goto out;
    }

    memset(s_key_pem, 0, sizeof(s_key_pem));
    if (mbedtls_pk_write_key_pem(&key, (unsigned char *)s_key_pem,
                                 sizeof(s_key_pem)) != 0) {
        ESP_LOGE(TAG, "key PEM export failed");
        goto out;
    }
    s_key_len = strlen(s_key_pem);

    /* Subject == issuer: a self-signed leaf. The CN is the hostname purely
     * for human readability in browser cert viewers — pinning clients never
     * check the name, which is exactly why the box stays reachable by mDNS
     * name and by IP alike. */
    char subject[96];
    snprintf(subject, sizeof(subject), "CN=%s,O=Klingelbox",
             (hostname && hostname[0]) ? hostname : "klingelbox");

    unsigned char serial_raw[16];
    esp_fill_random(serial_raw, sizeof(serial_raw));
    serial_raw[0] &= 0x7F;   /* keep the DER INTEGER positive */

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0) {
        ESP_LOGE(TAG, "name encoding failed");
        goto out;
    }
    /* mbedTLS 3.x replaced the MPI-based serial setter with a raw-bytes one. */
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serial_raw,
                                             sizeof(serial_raw)) != 0) {
        ESP_LOGE(TAG, "serial encoding failed");
        goto out;
    }
    if (mbedtls_x509write_crt_set_validity(&crt, DB_TLS_NOT_BEFORE,
                                           DB_TLS_NOT_AFTER) != 0) {
        ESP_LOGE(TAG, "validity encoding failed");
        goto out;
    }
    mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    mbedtls_x509write_crt_set_subject_key_identifier(&crt);
    mbedtls_x509write_crt_set_authority_key_identifier(&crt);
    mbedtls_x509write_crt_set_key_usage(&crt,
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_AGREEMENT);
    mbedtls_x509write_crt_set_ns_cert_type(&crt,
        MBEDTLS_X509_NS_CERT_TYPE_SSL_SERVER);

    memset(s_cert_pem, 0, sizeof(s_cert_pem));
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char *)s_cert_pem,
                                  sizeof(s_cert_pem),
                                  mbedtls_ctr_drbg_random, &drbg) != 0) {
        ESP_LOGE(TAG, "certificate PEM export failed");
        goto out;
    }
    s_cert_len = strlen(s_cert_pem);

    /* Must goto, not return: an early return would skip the cleanup label and
     * leak the pk/drbg/entropy/crt contexts.
     *
     * ALWAYS persisted — including the identity that replaces a rejected
     * upload (db_tls_load erased that pair before we got here). One stored
     * generated identity, one fingerprint across boots, whatever brought the
     * mint about. */
    ret = persist(DB_TLS_GENERATED);
    if (ret != ESP_OK) goto out;

    /* Same publication pattern as db_tls_load: generation only ever runs
     * while !s_ready, so the buffers were private until this flip. */
    state_lock();
    s_source = DB_TLS_GENERATED;
    s_ready = true;
    state_unlock();
    ESP_LOGI(TAG, "generated ECDSA P-256 identity in %lld ms, valid to 2056",
             (esp_timer_get_time() - t0) / 1000);

out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_pk_free(&key);
    return ret;
}

/* ------------------------------------------------------------ public API */

bool db_tls_ready(void) { return s_ready; }

db_tls_source_t db_tls_source(void) { return s_source; }

bool db_tls_custom_rejected(char *reason_out, size_t reason_sz)
{
    /* Under the lock like every cross-task read: the reason buffer is
     * written by db_tls_load and cleared by set/ack on the main task,
     * while GET /api/config may read it from either httpd task. */
    state_lock();
    bool rejected = s_custom_rejected;
    if (rejected && reason_out && reason_sz)
        snprintf(reason_out, reason_sz, "%s", s_rejected_reason);
    state_unlock();
    return rejected;
}

bool db_tls_rejection_is_fresh(void)
{
    /* Single-word read of a flag only ever set once, before any server task
     * exists (inside db_tls_load) — no lock needed. */
    return s_rejected_this_boot;
}

esp_err_t db_tls_ensure(const char *hostname)
{
    if (s_ready) return ESP_OK;
    db_tls_load();
    if (s_ready) return ESP_OK;
    ESP_LOGI(TAG, "no stored TLS identity — generating one");
    return generate_identity(hostname);
}

esp_err_t db_tls_get_cert_pem(const char **pem, size_t *len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (pem) *pem = s_cert_pem;
    /* esp_https_server wants the length INCLUDING the NUL for PEM input. */
    if (len) *len = s_cert_len + 1;
    return ESP_OK;
}

esp_err_t db_tls_get_key_pem(const char **pem, size_t *len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (pem) *pem = s_key_pem;
    if (len) *len = s_key_len + 1;
    return ESP_OK;
}

esp_err_t db_tls_dup_cert_pem(char **pem, size_t *len)
{
    /* A per-request heap copy rather than the borrowed pointer of
     * db_tls_get_cert_pem: this is the ONE read that happens on another
     * httpd task (:80's GET /cert.pem), and streaming the live buffer there
     * races the install/clear paths above. Copying under the lock and
     * serving the copy costs at most DB_TLS_CERT_MAX bytes of heap for the
     * duration of one send; holding the lock across the send instead would
     * let one slow client block a certificate rotation. */
    if (!pem) return ESP_ERR_INVALID_ARG;
    *pem = NULL;

    state_lock();
    if (!s_ready) {
        state_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = malloc(s_cert_len + 1);
    if (copy) {
        memcpy(copy, s_cert_pem, s_cert_len + 1);
        if (len) *len = s_cert_len;
    }
    state_unlock();

    if (!copy) return ESP_ERR_NO_MEM;
    *pem = copy;
    return ESP_OK;
}

esp_err_t db_tls_get_fingerprint(char *out, size_t out_sz)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!out || out_sz < 65) return ESP_ERR_INVALID_ARG;

    /* Parse back to DER: the fingerprint clients pin is over the DER bytes of
     * the LEAF, not the PEM text (which varies with line wrapping). This is
     * what `openssl x509 -fingerprint -sha256` prints, colons aside. */
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)s_cert_pem,
                               s_cert_len + 1) != 0) {
        mbedtls_x509_crt_free(&crt);
        return ESP_FAIL;
    }
    uint8_t digest[32];
    mbedtls_sha256(crt.raw.p, crt.raw.len, digest, 0);
    mbedtls_x509_crt_free(&crt);

    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, out_sz - (size_t)i * 2, "%02x", digest[i]);
    return ESP_OK;
}

esp_err_t db_tls_set_identity(const char *cert_pem, size_t cert_len,
                              const char *key_pem, size_t key_len)
{
    if (!cert_pem || !key_pem || cert_len == 0 || key_len == 0)
        return ESP_ERR_INVALID_ARG;
    if (cert_len >= DB_TLS_CERT_MAX || key_len >= DB_TLS_KEY_MAX)
        return ESP_ERR_INVALID_SIZE;

    esp_err_t rc = validate_pair(cert_pem, cert_len, key_pem, key_len);
    if (rc != ESP_OK) return rc;

    /* Only now touch the live buffers: a rejected upload must leave the
     * active identity byte-for-byte intact. Under the lock from the first
     * mutated byte to the final state flip — s_ready stays TRUE across an
     * install (the old identity keeps serving :443 until the restart), so
     * this is exactly the window a concurrent /cert.pem fetch on the :80
     * helper would otherwise read half-swapped. */
    state_lock();
    memcpy(s_cert_pem, cert_pem, cert_len);
    s_cert_pem[cert_len] = '\0';
    s_cert_len = cert_len;
    memcpy(s_key_pem, key_pem, key_len);
    s_key_pem[key_len] = '\0';
    s_key_len = key_len;

    rc = persist(DB_TLS_PROVIDED);
    if (rc != ESP_OK) {
        /* RAM now disagrees with flash; drop RAM so nothing serves a pair
         * that will vanish at reboot. The caller reports the store error. */
        s_ready = false;
        state_unlock();
        return rc;
    }
    s_source = DB_TLS_PROVIDED;
    s_ready = true;
    /* A validated, persisted upload is the happy end of a standing rejected
     * notice: the operator delivered the fixed pair the notice was asking
     * for, so it has nothing left to say. */
    s_custom_rejected = false;
    s_rejected_reason[0] = '\0';
    state_unlock();
    /* Best-effort like notice_persist, and for the same reason: the install
     * above already succeeded, and a notice that survives into the next boot
     * merely overstates a problem the fingerprint now disproves. */
    (void)notice_erase();

    char fp[65];
    db_tls_get_fingerprint(fp, sizeof(fp));
    ESP_LOGI(TAG, "installed provided TLS identity (%d certificate(s)), "
                  "fingerprint %s", db_pem_cert_count(s_cert_pem, s_cert_len), fp);
    return ESP_OK;
}

esp_err_t db_tls_clear(void)
{
    state_lock();
    s_ready = false;
    s_cert_len = s_key_len = 0;
    memset(s_key_pem, 0, sizeof(s_key_pem));
    memset(s_cert_pem, 0, sizeof(s_cert_pem));
    state_unlock();

    /* NVS_K_REJ deliberately NOT erased: the notice is its own record with
     * its own dismissals (a successful upload, or the explicit ack) — a
     * key rotation must not silently swallow the explanation of why the
     * previous upload vanished. */
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_TLS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return ESP_OK;   /* nothing stored, nothing to clear */
    nvs_erase_key(h, NVS_K_CERT);
    nvs_erase_key(h, NVS_K_KEY);
    nvs_erase_key(h, NVS_K_SRC);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t db_tls_ack_rejection(void)
{
    state_lock();
    s_custom_rejected = false;
    s_rejected_reason[0] = '\0';
    state_unlock();
    /* The NVS record must go too, or the "dismissed" notice would resurrect
     * at the next boot — a dismissal that only lasts until a power cycle
     * would be worse than none. */
    return notice_erase();
}

/* ---- host-test hook ------------------------------------------------------- */

#ifdef DB_HOSTTEST
/* Compiled ONLY into host-test/test_node_graph (its Makefile defines
 * DB_HOSTTEST; no device build does). Forgetting the resident state — but
 * not the fake flash — is what lets one test binary reboot: the replacement
 * decision and the notice reload only ever run inside db_tls_load(), so
 * proving that the replacement identity and the notice both come back from
 * NVS (and that the notice is NOT re-detected as fresh) requires dying and
 * coming back. */
void db_tls_hosttest_reset(void)
{
    s_ready = false;
    s_source = DB_TLS_GENERATED;
    s_cert_len = s_key_len = 0;
    memset(s_cert_pem, 0, sizeof(s_cert_pem));
    memset(s_key_pem, 0, sizeof(s_key_pem));
    s_custom_rejected = false;
    s_rejected_reason[0] = '\0';
    s_rejected_this_boot = false;
}
#endif
