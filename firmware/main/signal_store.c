/*
 * signal_store.c - Persisted signals and button matching (see the header).
 *
 * NVS LAYOUT ("dbsig" namespace)
 *
 *     "index"  ->  [ sig_hdr_t { version, meta_size, count } ][ meta x count ]
 *     "f1".."f<id>" -> [ frm_hdr_t { count, first_level } ][ uint16 durations ]
 *
 * The split is the header's storage decision made concrete: the index is small
 * (~100 bytes per signal) and is read once at boot into RAM, while a frame is
 * ~1 KB and is fetched only when someone actually wants to replay or draw it.
 * The frame blob is written in its used length, not sizeof(rf_frame_t): storing
 * the full 1 KB array for a 49-pulse EV1527 frame would waste 90 % of the flash
 * page and, worse, multiply the erase/rewrite cost of every registration.
 *
 * ONE MUTEX, HELD BRIEFLY. Matching runs on the capture task (which holds the
 * radio lock at that moment), while the REST layer mutates from the HTTP task.
 * The metadata scan is a few hundred integer comparisons over a resident array —
 * no flash, no allocation — so serialising the two is cheap. Everything that
 * touches flash (add/delete/rename/load_frame) also runs under the same lock,
 * accepting that a capture-time match may wait a few milliseconds behind a save.
 * That is far preferable to a lock-free scan racing an index rewrite.
 *
 * READERS ON OTHER TASKS GET COPIES, NOT POINTERS. The pointer-returning
 * accessors exist for the HTTP task alone — the task that also runs every
 * delete, so nothing can compact the array under a pointer it is still using.
 * The MQTT bridge and the dispatch task run concurrently with HTTP on the
 * other core; for them db_signals_get_copy()/db_signals_snapshot() copy the
 * records out under the mutex, because a live pointer read against a
 * delete-in-progress can change which signal it describes between two field
 * reads. See the threading contract in signal_store.h.
 *
 * THE PARTITION IS SMALLER THAN THE ADVERTISED STORE. 32 signals at the
 * 512-pulse maximum are ~33 KB of frame blobs — more than the original 24 KB
 * NVS partition holds in total, and OTA never enlarges a partition table, so
 * boxes first flashed before the table grew keep 24 KB forever. store()
 * therefore checks the partition's ACTUAL free space before every add and
 * refuses with ESP_ERR_NVS_NOT_ENOUGH_SPACE while there is still headroom
 * left for the index rewrite and for the config/graph blobs, so filling the
 * signal store can never wedge every other save on the box.
 *
 * THE TRAILING-FRAGMENT PROBLEM (bench, 2026-08-31)
 *
 * A single real press of the user's doorbell yields the 49-pulse frame and then,
 * occasionally, a second 33-pulse frame with the SAME base (~290 us) and the
 * same RSSI: the tail of the transmitter's burst, clipped by the capture idle
 * threshold. rf_service's burst coalescing cannot merge it (it is not a similar
 * frame), so it surfaces as its own event a quarter of a second later.
 *
 * It must not be reported as a DIFFERENT button, because it is the same press.
 * That is prevented by one rule, applied to every burst before anything else:
 * a burst arriving within DB_FRAG_WINDOW_US of the previous one, with fewer than
 * DB_FRAG_MIN_PCT of its pulses, is a tail fragment and is dropped. Pulse count
 * alone is never used as a signal/noise discriminator — the bench proved the two
 * populations overlap — it is only ever used RELATIVE to the burst it follows,
 * which is exactly the relation a clipped tail has to its own frame.
 *
 * NOTE THAT THIS RULE IS ABOUT MATCHING ONLY. It used to do double duty as an
 * admission filter for a separate "learn mode", which decided whether a burst
 * was even allowed to be offered to the user. That mode is gone: registering a
 * button now goes through a listening session (rf_raw.h), which keeps every
 * frame, groups the repeats and ranks them, and detects a chopped transmission
 * properly instead of discarding its pieces. Nothing in this file may filter a
 * signal out of a user's sight any more; it only decides what an incoming burst
 * MATCHES.
 *
 * DIAGNOSTICS. This module reports into the shared db_diag vocabulary only where
 * it has something the radio layer could not know: what a registration actually
 * stored (PROTOCOL_DECODED with the identity, or UNKNOWN_PROTOCOL_RAW with the
 * fingerprint of the raw frame that is now replayable).
 */
#include "signal_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db_diag.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"   /* NVS_DEFAULT_PART_NAME, for stats and the sweep */

static const char *TAG = "db_signals";

#define DB_SIG_NS        "dbsig"
#define DB_SIG_INDEX_KEY "index"
#define DB_SIG_VERSION   1u

/* Trailing-fragment rejection — see the file header for the bench data.
 * 750 ms: the tail is flushed one burst-window (250 ms) after its parent, so the
 * window spans that comfortably while staying far below the interval between two
 * deliberate presses. 75 %: the observed tail was 33 of 49 pulses (67 %), and a
 * repeat of the SAME button always carries the full pulse count, so a genuine
 * second press can never fall under the bar. */
#define DB_FRAG_WINDOW_US 750000
#define DB_FRAG_MIN_PCT   75u

/* ---- persisted layout ---------------------------------------------------- */

typedef struct {
    uint32_t version;
    uint32_t meta_size;   /* sizeof(db_signal_meta_t) when written */
    uint32_t count;
} sig_hdr_t;

typedef struct {
    uint16_t count;
    uint8_t  first_level;
    uint8_t  reserved;
} frm_hdr_t;

/* ---- resident state ------------------------------------------------------ */

static db_signal_meta_t  s_meta[DB_SIGNAL_MAX];
static int               s_count;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool              s_ready;

/* One staging buffer for every NVS transfer, reused under the lock. Sized for
 * the index, which is the larger of the two blobs. No malloc anywhere in this
 * module: a doorbell must not be able to fail a registration because the heap
 * is fragmented. */
static uint8_t s_blob[sizeof(sig_hdr_t) + DB_SIGNAL_MAX * sizeof(db_signal_meta_t)];

/* What the previous burst looked like, for the fragment test. */
static struct {
    int64_t  last_ts_us;    /* rf_event_t.ts_us of the last burst examined     */
    uint16_t ref_pulses;    /* pulse count of the last NON-fragment burst      */
    uint16_t matched_id;    /* what that burst matched, 0 if nothing           */
    bool     was_fragment;  /* the verdict for last_ts_us, cached so a repeat
                               question about the same burst is free           */
} s_recent;

/* ---- small helpers ------------------------------------------------------- */

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static db_signal_meta_t *find_meta(uint16_t id)
{
    for (int i = 0; i < s_count; i++)
        if (s_meta[i].id == id)
            return &s_meta[i];
    return NULL;
}

/* Lowest free id from 1 up. Reusing a freed id keeps the numbers a user sees
 * small and stable, and the frame key ("f<id>") is erased on delete, so there is
 * never a stale frame to inherit. */
static uint16_t next_free_id(void)
{
    for (uint16_t id = 1; id <= DB_SIGNAL_MAX * 2; id++)
        if (!find_meta(id))
            return id;
    return 0;
}

static void frame_key(char *dst, size_t cap, uint16_t id)
{
    snprintf(dst, cap, "f%u", (unsigned)id);
}

/* Unix seconds if the clock has been set (SNTP may never run on an offline box),
 * 0 otherwise — the API contract says 0 means "clock unknown", which is far more
 * honest than stamping 1970. */
static int64_t now_unix(void)
{
    time_t t = time(NULL);
    return (t > 1600000000) ? (int64_t)t : 0;
}

/* ---- persistence --------------------------------------------------------- */

/* Caller holds the lock. */
static esp_err_t save_index(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_SIG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "index save: nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    sig_hdr_t hdr = { .version   = DB_SIG_VERSION,
                      .meta_size = sizeof(db_signal_meta_t),
                      .count     = (uint32_t)s_count };
    memcpy(s_blob, &hdr, sizeof(hdr));

    db_signal_meta_t *dst = (db_signal_meta_t *)(s_blob + sizeof(hdr));
    for (int i = 0; i < s_count; i++) {
        dst[i] = s_meta[i];
        /* Live stats are explicitly not persisted (see the header): writing a
         * seen_count to flash on every press would wear the part out. */
        dst[i].last_seen_us = 0;
        dst[i].seen_count   = 0;
    }

    size_t len = sizeof(hdr) + (size_t)s_count * sizeof(db_signal_meta_t);
    err = nvs_set_blob(h, DB_SIG_INDEX_KEY, s_blob, len);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
        ESP_LOGE(TAG, "index save failed: %s", esp_err_to_name(err));
    return err;
}

/* True only when load_index() positively parsed an index written in THIS
 * build's exact layout. reconcile_flash() keys on it: an ignored newer-layout
 * index means the frame blobs belong to the firmware that wrote it, and
 * "sweeping orphans" against an index we chose not to read would erase every
 * one of that firmware's waveforms — the precise rollback destruction the
 * ignore rule exists to prevent. */
static bool s_index_authoritative;

/* Caller holds the lock. */
static esp_err_t load_index(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_SIG_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored signals yet (%s)", esp_err_to_name(err));
        return ESP_OK;
    }

    size_t len = sizeof(s_blob);
    err = nvs_get_blob(h, DB_SIG_INDEX_KEY, s_blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len < sizeof(sig_hdr_t)) {
        ESP_LOGI(TAG, "signal index absent/short (%s) — starting empty",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    sig_hdr_t hdr;
    memcpy(&hdr, s_blob, sizeof(hdr));

    /* Same rule as db_config.c: only an exact layout match is reinterpreted. A
     * mismatched index is left on flash untouched (a rollback to the firmware
     * that wrote it must find its signals intact) and we simply start empty. */
    if (hdr.version != DB_SIG_VERSION || hdr.meta_size != sizeof(db_signal_meta_t)) {
        ESP_LOGW(TAG, "signal index layout v%u/%u is not this build's "
                      "(v%u/%u) — ignoring it",
                 (unsigned)hdr.version, (unsigned)hdr.meta_size,
                 (unsigned)DB_SIG_VERSION, (unsigned)sizeof(db_signal_meta_t));
        return ESP_OK;
    }

    uint32_t n = hdr.count;
    if (n > DB_SIGNAL_MAX)
        n = DB_SIGNAL_MAX;
    if (len < sizeof(hdr) + (size_t)n * sizeof(db_signal_meta_t)) {
        ESP_LOGE(TAG, "signal index truncated — starting empty");
        return ESP_OK;
    }

    const db_signal_meta_t *src = (const db_signal_meta_t *)(s_blob + sizeof(hdr));
    s_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (src[i].id == 0)
            continue;   /* defensive: id 0 means "none" and must never be stored */
        s_meta[s_count] = src[i];
        s_meta[s_count].name[DB_SIGNAL_NAME_MAX - 1] = '\0';
        s_meta[s_count].protocol[RF_PROTOCOL_NAME_MAX - 1] = '\0';
        s_meta[s_count].last_seen_us = 0;
        s_meta[s_count].seen_count   = 0;
        s_count++;
    }
    s_index_authoritative = true;
    return ESP_OK;
}

/* Caller holds the lock. */
static esp_err_t save_frame(uint16_t id, const rf_frame_t *frame)
{
    if (!frame || frame->count == 0 || frame->count > RF_FRAME_MAX_PULSES)
        return ESP_ERR_INVALID_ARG;

    char key[16];
    frame_key(key, sizeof(key), id);

    frm_hdr_t fh = { .count = frame->count, .first_level = frame->first_level,
                     .reserved = 0 };
    size_t len = sizeof(fh) + (size_t)frame->count * sizeof(uint16_t);
    if (len > sizeof(s_blob))
        return ESP_ERR_INVALID_SIZE;

    memcpy(s_blob, &fh, sizeof(fh));
    memcpy(s_blob + sizeof(fh), frame->durations_us,
           (size_t)frame->count * sizeof(uint16_t));

    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_SIG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(h, key, s_blob, len);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
        ESP_LOGE(TAG, "frame save (%s) failed: %s", key, esp_err_to_name(err));
    return err;
}

/* Caller holds the lock. An already-absent frame counts as success — the goal
 * is "no such blob on flash", however we got there. The return value matters
 * to delete(): "the blob is gone" and "flash would not let go of it" lead to
 * different recoveries there. */
static esp_err_t erase_frame(uint16_t id)
{
    char key[16];
    frame_key(key, sizeof(key), id);

    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_SIG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK)
        err = nvs_commit(h);
    else if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    else
        ESP_LOGW(TAG, "frame erase (%s): %s", key, esp_err_to_name(err));
    nvs_close(h);
    return err;
}

/* ---- flash budget ---------------------------------------------------------
 *
 * The store must refuse an add BEFORE it starts writing, with an error that
 * names the real problem, because "the partition is full" has very different
 * consequences from "this one request failed": once NVS is genuinely full,
 * config saves, graph edits and the deferred switch-position flush all start
 * failing too — some of them silently. The reserve below is what keeps that
 * state unreachable through this module, whatever the partition size under us
 * happens to be (24 KB on boxes flashed before the table grew, 80 KB after).
 */

/* NVS accounting is in 32-byte entries. A blob costs its data rounded up to
 * entries, plus one entry per chunk header, plus one for the blob index — and
 * a chunk cannot span a page, so a blob a little over a page boundary takes a
 * second chunk. 4 entries of overhead covers two chunks plus the index with
 * one spare; every blob this module writes fits in two chunks (the largest,
 * the full index, is under two pages). Deliberately a slight OVERestimate —
 * the price of refusing one add early is a user deleting a signal they could
 * technically have kept; the price of underestimating is the wedged-box state
 * this exists to prevent. */
static size_t entries_for_blob(size_t len)
{
    return 4u + (len + 31u) / 32u;
}

/* Entries kept free for everything that is NOT a signal frame: rewriting the
 * signal index costs a full extra copy while the old one still exists (~110
 * entries at 32 signals — already counted separately by store()), and beyond
 * that the config blob (~50), the graph's nodes blob (~90) and links blob
 * (~15) must each stay REWRITABLE, which likewise needs room for a fresh copy
 * next to the old. 160 entries ≈ 5 KB covers those three with margin. */
#define DB_SIG_RESERVE_ENTRIES 160u

/* Would adding a frame of `frame_len` bytes (plus the grown index rewrite)
 * still leave the reserve free? Caller holds the lock. */
static bool store_has_room(size_t frame_len)
{
    nvs_stats_t st;
    if (nvs_get_stats(NVS_DEFAULT_PART_NAME, &st) != ESP_OK)
        return true;   /* no stats, no verdict — let the write itself decide */

    size_t index_len = sizeof(sig_hdr_t) +
                       ((size_t)s_count + 1u) * sizeof(db_signal_meta_t);
    size_t need = entries_for_blob(frame_len) + entries_for_blob(index_len) +
                  DB_SIG_RESERVE_ENTRIES;
    if (st.available_entries >= need)
        return true;

    ESP_LOGE(TAG, "store full: %u NVS entries free, this add needs %u "
                  "(%u-byte frame) plus a %u-entry reserve — delete a signal "
                  "or two first",
             (unsigned)st.available_entries,
             (unsigned)(need - DB_SIG_RESERVE_ENTRIES),
             (unsigned)frame_len, (unsigned)DB_SIG_RESERVE_ENTRIES);
    return false;
}

/* ---- init ---------------------------------------------------------------- */

/*
 * RECONCILE THE INDEX AND THE FRAME BLOBS AFTER A BADLY-TIMED POWER CUT.
 *
 * delete() commits two independent NVS writes, so a power cut between them
 * necessarily leaves one of two mismatches on flash, and this sweep repairs
 * both at every boot:
 *
 *   GHOST   an index entry whose frame blob is gone (the frame was erased but
 *           the shrunken index never committed — only reachable through
 *           delete()'s full-partition fallback path). It is dropped from the
 *           loaded index: the user deleted it, it has no waveform to replay,
 *           and listing it would be advertising a signal that errors on every
 *           use. The pruned index is written back best-effort; if THAT write
 *           fails, RAM is still correct and the next successful save or boot
 *           finishes the job.
 *
 *   ORPHAN  a frame blob no index entry names (the normal delete order,
 *           interrupted after the index committed — or the remnant of a
 *           pre-fix firmware's failed delete). Invisible but not free: at up
 *           to ~1 KB each, orphans eat exactly the space the flash budget
 *           above is defending. Erased outright — an id absent from the
 *           committed index is deleted BY DEFINITION, because the index
 *           commit is the moment a mutation becomes real (see store()'s
 *           rollback, which relies on the same rule).
 *
 * Caller holds the lock. Runs before repair_synthesized_ev1527(), so that
 * one-time repair never wastes a rebuild on a ghost.
 */
static void reconcile_flash(void)
{
    /* Repairs are only meaningful against an index this build actually read.
     * An absent, truncated or NEWER-layout index loads as empty ON PURPOSE
     * (rollback preservation, see load_index) — sweeping "orphans" against
     * that emptiness would erase waveforms that are not ours to judge. */
    if (!s_index_authoritative)
        return;

    nvs_handle_t h;
    if (nvs_open(DB_SIG_NS, NVS_READWRITE, &h) != ESP_OK)
        return;   /* no namespace yet — nothing stored, nothing to reconcile */

    /* Ghosts first: with them gone, the orphan scan below sees the true
     * membership. Only a definite NOT_FOUND drops an entry — any other
     * verdict (transient NVS trouble) keeps it, because deleting a user's
     * signal on a maybe is precisely what this module must never do. */
    int kept = 0, ghosts = 0;
    for (int i = 0; i < s_count; i++) {
        char key[16];
        frame_key(key, sizeof(key), s_meta[i].id);
        if (nvs_find_key(h, key, NULL) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "signal %u '%s' has no stored waveform (interrupted "
                          "delete) — completing the delete",
                     (unsigned)s_meta[i].id, s_meta[i].name);
            ghosts++;
            continue;
        }
        s_meta[kept++] = s_meta[i];
    }
    if (ghosts > 0) {
        for (int i = kept; i < s_count; i++)
            memset(&s_meta[i], 0, sizeof(s_meta[i]));
        s_count = kept;
        save_index();   /* best-effort: RAM is already right (see above) */
    }

    /* Orphans: every "f<id>" blob in the namespace whose id the index does not
     * carry. Collected first, erased after — erasing under a live iterator is
     * asking the NVS internals a question they do not promise to answer. The
     * array bounds the sweep at one store's worth per boot; anything past that
     * (unreachable in practice) waits for the next one. */
    uint16_t orphan[DB_SIGNAL_MAX];
    int n_orphan = 0;

    nvs_iterator_t it = NULL;
    esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, DB_SIG_NS,
                                 NVS_TYPE_BLOB, &it);
    while (r == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (info.key[0] == 'f') {
            char *end = NULL;
            long v = strtol(info.key + 1, &end, 10);
            if (end && *end == '\0' && v > 0 && v <= 0xFFFF &&
                !find_meta((uint16_t)v) && n_orphan < DB_SIGNAL_MAX)
                orphan[n_orphan++] = (uint16_t)v;
        }
        r = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    for (int i = 0; i < n_orphan; i++) {
        char key[16];
        frame_key(key, sizeof(key), orphan[i]);
        ESP_LOGW(TAG, "erasing orphaned frame blob %s (interrupted delete)", key);
        nvs_erase_key(h, key);
    }
    if (n_orphan > 0)
        nvs_commit(h);
    nvs_close(h);
}

/*
 * ONE-TIME REPAIR OF SYNTHESIZED EV1527 FRAMES WRITTEN BY THE OLD ENCODER
 * (bench, 2026-08-31).
 *
 * rf_ev1527_build() used to emit its sync pair at the FRONT of the frame, which
 * — once rf_transmit added its inter-frame gap behind it — put two long lows
 * into every on-air period and stopped real receivers decoding the word at all
 * (see the long note in rf_ev1527.c). The encoder is fixed, but a virtual signal
 * is a FRAME ON FLASH, not a recipe: every one created before the fix still
 * holds the broken waveform, and the user has no way to know that the cure is
 * "delete it and make it again". Since we know exactly what those frames are —
 * synthesized, EV1527, and re-derivable from the address and button we stored
 * alongside them — rebuilding them at boot is strictly better than making
 * someone re-do their setup.
 *
 * This is deliberately narrow. It touches nothing captured (a recording is
 * evidence and is never rewritten), nothing undecoded, and nothing that does not
 * still carry the old sync-first shape, so it is idempotent and a second boot is
 * a no-op. Frames are only rewritten if the rebuild succeeds; a failure leaves
 * the old frame exactly where it was.
 *
 * Caller holds the lock.
 */
#define DB_EV1527_SYNTH_PULSES 50u   /* 24 bit-pairs + the 2-pulse sync */

/* Defined further down with the rest of the persistence, but needed here: this
 * repair has to run before anything else can observe a stale frame. */
static esp_err_t load_frame_locked(uint16_t id, rf_frame_t *out);

static void repair_synthesized_ev1527(void)
{
    /* ~1.6 KB of buffers: static, because db_signals_init() runs on app_main's
     * stack and this module allocates nothing at runtime by policy. */
    static rf_frame_t   frame;
    static rf_norm_t    norm;
    static rf_decoded_t dec;
    int repaired = 0;

    for (int i = 0; i < s_count; i++) {
        db_signal_meta_t *m = &s_meta[i];

        if (m->origin != DB_ORIGIN_SYNTHESIZED || !m->decoded_valid)
            continue;
        if (strncmp(m->protocol, "ev1527", RF_PROTOCOL_NAME_MAX) != 0)
            continue;
        if (load_frame_locked(m->id, &frame) != ESP_OK)
            continue;
        if (frame.count != DB_EV1527_SYNTH_PULSES || frame.first_level != 1)
            continue;
        /* The tell: pulse 0 is the one-base sync high and pulse 1 the 31x sync
         * low. In a repaired frame those two are a bit pair, so their ratio is
         * 3 or 1/3 — nowhere near the 8x this asks for. */
        if (frame.durations_us[0] == 0 ||
            frame.durations_us[1] < 8u * (uint32_t)frame.durations_us[0])
            continue;

        if (!rf_ev1527_build(m->decoded_id, m->decoded_button, m->base_us, &frame))
            continue;
        if (save_frame(m->id, &frame) != ESP_OK)
            continue;

        /* The durations moved, so the fingerprint moved with them. Re-derive it
         * from the frame the same way the capture path would, rather than
         * patching fields by hand. */
        rf_normalize(&frame, &norm);
        rf_decode(&frame, &norm, &dec);
        m->fingerprint = rf_fingerprint(&frame, &norm);
        m->base_us     = norm.base_us;
        m->confidence  = norm.confidence;
        m->pulse_count = frame.count;
        repaired++;

        ESP_LOGW(TAG, "rebuilt virtual signal %u '%s' (EV1527 id=0x%05lX btn=0x%X): "
                      "its sync gap was at the wrong end of the frame",
                 (unsigned)m->id, m->name,
                 (unsigned long)m->decoded_id, m->decoded_button);
    }

    if (repaired > 0 && save_index() == ESP_OK)
        /* DB_EVENT_TEXT_MAX is tight — say the actionable thing and stop. */
        db_events_push(DB_EV_SYSTEM, 0, 0, 0, 0,
                       "Rebuilt %d virtual signal(s): their sync gap was at the "
                       "wrong end", repaired);
}

esp_err_t db_signals_init(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (!s_lock)
        return ESP_ERR_NO_MEM;
    if (s_ready)
        return ESP_OK;

    lock();
    esp_err_t err = load_index();
    reconcile_flash();
    repair_synthesized_ev1527();
    s_ready = true;
    int n = s_count;
    unlock();

    ESP_LOGI(TAG, "%d signal(s) loaded (capacity %d)", n, DB_SIGNAL_MAX);
    return err;
}

/* ---- read-only accessors ------------------------------------------------- */

int db_signals_count(void) { return s_count; }

/*
 * The two POINTER-RETURNING accessors take no lock, and locking inside them
 * would not help: the hazard is not the lookup but the pointer OUTLIVING it,
 * still being dereferenced while delete() compacts the array underneath. So
 * the contract (signal_store.h) restricts them to the HTTP task — the task
 * every mutation runs on, where nothing can move the array between a lookup
 * and its last use. Any other task must use db_signals_get_copy() /
 * db_signals_snapshot() below, which hold the lock for the whole copy.
 */
const db_signal_meta_t *db_signals_list(void) { return s_meta; }

const db_signal_meta_t *db_signals_get(uint16_t id)
{
    return find_meta(id);
}

esp_err_t db_signals_get_copy(uint16_t id, db_signal_meta_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    lock();
    const db_signal_meta_t *m = find_meta(id);
    if (m)
        *out = *m;
    unlock();
    return m ? ESP_OK : ESP_ERR_NOT_FOUND;
}

int db_signals_snapshot(db_signal_meta_t *out, int max)
{
    if (!out || max <= 0)
        return 0;
    lock();
    int n = (s_count < max) ? s_count : max;
    memcpy(out, s_meta, (size_t)n * sizeof(*out));
    unlock();
    return n;
}

/* Caller holds the lock. */
static esp_err_t load_frame_locked(uint16_t id, rf_frame_t *out)
{
    if (!find_meta(id))
        return ESP_ERR_NOT_FOUND;

    char key[16];
    frame_key(key, sizeof(key), id);

    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_SIG_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;

    size_t len = sizeof(s_blob);
    err = nvs_get_blob(h, key, s_blob, &len);
    nvs_close(h);

    if (err == ESP_OK && len >= sizeof(frm_hdr_t)) {
        frm_hdr_t fh;
        memcpy(&fh, s_blob, sizeof(fh));
        size_t need = sizeof(fh) + (size_t)fh.count * sizeof(uint16_t);
        if (fh.count == 0 || fh.count > RF_FRAME_MAX_PULSES || len < need) {
            ESP_LOGE(TAG, "frame %s is malformed (count=%u, %u bytes)",
                     key, fh.count, (unsigned)len);
            err = ESP_ERR_INVALID_SIZE;
        } else {
            rf_frame_reset(out);
            out->count       = fh.count;
            out->first_level = fh.first_level ? 1 : 0;
            memcpy(out->durations_us, s_blob + sizeof(fh),
                   (size_t)fh.count * sizeof(uint16_t));
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_SIZE;
    }

    if (err != ESP_OK)
        ESP_LOGE(TAG, "load frame %u: %s", (unsigned)id, esp_err_to_name(err));
    return err;
}

esp_err_t db_signals_load_frame(uint16_t id, rf_frame_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;

    lock();
    esp_err_t err = load_frame_locked(id, out);
    unlock();
    return err;
}

/* ---- creation ------------------------------------------------------------ */

/* Fill a metadata record from an analysed burst. Caller holds the lock. */
static void meta_from_parts(db_signal_meta_t *m, uint16_t id, const char *name,
                            db_signal_origin_t origin, const rf_frame_t *frame,
                            const rf_norm_t *norm, const rf_decoded_t *decoded,
                            rf_fingerprint_t fp)
{
    memset(m, 0, sizeof(*m));
    m->id = id;
    snprintf(m->name, sizeof(m->name), "%s",
             (name && name[0]) ? name : "Unnamed signal");
    m->origin      = (uint8_t)origin;
    m->created_at  = now_unix();
    m->fingerprint = fp;
    m->base_us     = norm ? norm->base_us : 0;
    m->confidence  = norm ? norm->confidence : 0;
    m->pulse_count = frame ? frame->count : 0;

    if (decoded && decoded->valid) {
        m->decoded_valid  = true;
        snprintf(m->protocol, sizeof(m->protocol), "%s", decoded->protocol);
        m->decoded_id     = decoded->id;
        m->decoded_button = decoded->button;
        if (m->base_us == 0)
            m->base_us = decoded->base_us;
    }
}

/* Insert a fully-built record plus its frame. Caller holds the lock. */
static esp_err_t store(const db_signal_meta_t *meta, const rf_frame_t *frame,
                       uint16_t *id_out)
{
    if (s_count >= DB_SIGNAL_MAX) {
        ESP_LOGE(TAG, "cannot store '%s': the store is full (%d signals)",
                 meta->name, DB_SIGNAL_MAX);
        return ESP_ERR_NO_MEM;
    }

    /* The OTHER way the store gets full: the partition itself. Checked before
     * anything is written, so a refusal leaves no half-added state behind, and
     * reported as its own error — the API must be able to say "delete some
     * signals" rather than shrugging with a generic failure. See the flash
     * budget section above for the arithmetic. */
    size_t frame_len = sizeof(frm_hdr_t) +
                       (size_t)frame->count * sizeof(uint16_t);
    if (!store_has_room(frame_len))
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;

    esp_err_t err = save_frame(meta->id, frame);
    if (err != ESP_OK)
        return err;

    s_meta[s_count] = *meta;
    s_count++;

    err = save_index();
    if (err != ESP_OK) {
        /* Roll back so RAM and flash cannot disagree — a signal that exists
         * until the next reboot is the worst of both worlds. */
        s_count--;
        erase_frame(meta->id);
        return err;
    }

    if (id_out)
        *id_out = meta->id;
    ESP_LOGI(TAG, "stored signal %u '%s' (%u pulses, base %u us, conf %u%%)",
             (unsigned)meta->id, meta->name, meta->pulse_count,
             meta->base_us, meta->confidence);
    return ESP_OK;
}

esp_err_t db_signals_add_event(const rf_event_t *ev, const char *name,
                               db_signal_origin_t origin, uint16_t *id_out)
{
    if (!ev || ev->frame.count == 0)
        return ESP_ERR_INVALID_ARG;

    lock();
    uint16_t id = next_free_id();
    if (id == 0) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    db_signal_meta_t m;
    meta_from_parts(&m, id, name, origin, &ev->frame, &ev->norm, &ev->decoded,
                    ev->fingerprint);
    esp_err_t err = store(&m, &ev->frame, id_out);
    unlock();
    return err;
}

esp_err_t db_signals_add_frame(const rf_frame_t *frame, const rf_decoded_t *decoded,
                               const char *name, db_signal_origin_t origin,
                               uint16_t *id_out)
{
    if (!frame || frame->count == 0)
        return ESP_ERR_INVALID_ARG;

    /* Analyse the frame the same way the capture path would, so an imported or
     * synthesized signal carries the same fingerprint/base/confidence as one
     * that arrived over the air — otherwise a synthesized signal transmitted by
     * this box and heard back by it would not match itself. norm is ~600 bytes;
     * it is static because this can be called from the HTTP task's stack. */
    static rf_norm_t norm;
    static rf_decoded_t self_decoded;

    lock();
    rf_normalize(frame, &norm);
    const rf_decoded_t *dec = decoded;
    if (!dec || !dec->valid) {
        rf_decode(frame, &norm, &self_decoded);
        dec = &self_decoded;
    }
    rf_fingerprint_t fp = rf_fingerprint(frame, &norm);

    uint16_t id = next_free_id();
    if (id == 0) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    db_signal_meta_t m;
    meta_from_parts(&m, id, name, origin, frame, &norm, dec, fp);
    esp_err_t err = store(&m, frame, id_out);

    /* Copy out what the report needs before dropping the lock. */
    bool     rep_decoded = m.decoded_valid;
    uint32_t rep_id      = m.decoded_id;
    uint8_t  rep_btn     = m.decoded_button;
    uint16_t rep_pulses  = m.pulse_count;
    rf_fingerprint_t rep_fp = m.fingerprint;
    char rep_name[DB_SIGNAL_NAME_MAX];
    char rep_proto[RF_PROTOCOL_NAME_MAX];
    memcpy(rep_name, m.name, sizeof(rep_name));
    memcpy(rep_proto, m.protocol, sizeof(rep_proto));
    unlock();

    /* Registering something heard off the air is the one moment this module
     * knows something the radio layer could not: whether what the user just
     * kept is a protocol we understand or an opaque waveform that is
     * nonetheless fully replayable. Both are successes and both are reported —
     * the second one especially, because a box that stayed silent about it is
     * what made unknown protocols feel unsupported. */
    if (err == ESP_OK && origin == DB_ORIGIN_CAPTURED) {
        if (rep_decoded)
            db_diag_report(DB_DIAG_PROTOCOL_DECODED,
                           "registered '%s' as %s id=0x%lX btn=0x%X",
                           rep_name, rep_proto, (unsigned long)rep_id, rep_btn);
        else
            db_diag_report(DB_DIAG_UNKNOWN_PROTOCOL_RAW,
                           "registered '%s' raw: %u pulses, fp %08lx (replayable)",
                           rep_name, rep_pulses, (unsigned long)rep_fp);
    }
    return err;
}

esp_err_t db_signals_rename(uint16_t id, const char *name)
{
    if (!name || !name[0])
        return ESP_ERR_INVALID_ARG;

    lock();
    db_signal_meta_t *m = find_meta(id);
    if (!m) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(m->name, sizeof(m->name), "%s", name);
    esp_err_t err = save_index();
    unlock();

    if (err == ESP_OK)
        ESP_LOGI(TAG, "signal %u renamed to '%s'", (unsigned)id, name);
    return err;
}

esp_err_t db_signals_delete(uint16_t id)
{
    lock();
    int idx = -1;
    for (int i = 0; i < s_count; i++)
        if (s_meta[i].id == id) { idx = i; break; }
    if (idx < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    db_signal_meta_t removed = s_meta[idx];   /* for the rollback below */

    for (int i = idx; i < s_count - 1; i++)
        s_meta[i] = s_meta[i + 1];
    s_count--;
    memset(&s_meta[s_count], 0, sizeof(s_meta[s_count]));

    /*
     * ORDER MATTERS, FOR POWER LOSS. The two flash writes cannot be atomic
     * together, so one of them commits first — and the index must be that one.
     * A cut after the index commit leaves an ORPHANED frame blob, which lists
     * nowhere and is swept at the next boot; the other order leaves a GHOST: a
     * signal that is listed but whose waveform is gone, i.e. a delete that
     * visibly un-happened. (Both leftovers are repaired by reconcile_flash()
     * at init, but only one of them was ever visible to the user.)
     *
     * The exception is a partition too full to rewrite the index, because the
     * rewrite needs room for the new copy while the old one still exists. In
     * exactly that case the order flips — free the ~1 KB frame, then retry —
     * since delete is the only shovel the user has to dig a full store out
     * with, and the safe order would refuse to dig.
     */
    esp_err_t err = save_index();
    if (err == ESP_OK) {
        erase_frame(id);
    } else if (erase_frame(id) == ESP_OK) {
        err = save_index();
        if (err != ESP_OK) {
            /* Frame gone, old index still on flash: the ghost resurrects at
             * the next boot only until reconcile_flash() prunes it (no frame,
             * no listing). RAM is already what the user asked for, so this IS
             * a successful delete — just one whose durability arrives at the
             * next index write or reboot instead of now. */
            ESP_LOGW(TAG, "signal %u deleted, but the smaller index could not "
                          "be written (%s) — flash catches up at the next "
                          "save or boot", (unsigned)id, esp_err_to_name(err));
            err = ESP_OK;
        }
    } else {
        /* Neither write took, so flash still holds the signal in full. Put the
         * RAM entry back — the same rollback discipline as store() — because a
         * signal that is "deleted" until the next reboot and then resurrects
         * is the worst of both worlds. The caller gets the honest error. */
        for (int i = s_count; i > idx; i--)
            s_meta[i] = s_meta[i - 1];
        s_meta[idx] = removed;
        s_count++;
        unlock();
        ESP_LOGE(TAG, "signal %u NOT deleted: %s", (unsigned)id,
                 esp_err_to_name(err));
        return err;
    }

    /* The recent-burst reference may name the signal we just removed. */
    if (s_recent.matched_id == id)
        s_recent.matched_id = 0;
    unlock();

    ESP_LOGI(TAG, "signal %u deleted", (unsigned)id);
    return err;
}

/* ---- matching ------------------------------------------------------------ */

/*
 * Judge a burst against its predecessor and remember it. Returns true when the
 * burst is a trailing fragment of the previous one (see the file header).
 *
 * A repeat question about the same burst (identified by its ts_us) re-uses the
 * verdict rather than comparing the burst against itself.
 *
 * Caller holds the lock.
 */
static bool judge_burst(const rf_event_t *ev)
{
    if (ev->ts_us == s_recent.last_ts_us && s_recent.last_ts_us != 0)
        return s_recent.was_fragment;

    bool frag = false;
    if (s_recent.ref_pulses > 0 &&
        (ev->ts_us - s_recent.last_ts_us) < DB_FRAG_WINDOW_US &&
        (uint32_t)ev->frame.count * 100u <
            (uint32_t)s_recent.ref_pulses * DB_FRAG_MIN_PCT) {
        frag = true;
    }

    s_recent.last_ts_us   = ev->ts_us;
    s_recent.was_fragment = frag;
    if (!frag) {
        /* A new main frame becomes the reference. Fragments deliberately do NOT
         * update it: a run of tails must all be measured against the real
         * frame, not against each other. */
        s_recent.ref_pulses = ev->frame.count;
        s_recent.matched_id = 0;
    }
    return frag;
}

/* Decoded identity is the strongest evidence available: it survives the timing
 * drift that fingerprints only tolerate, and two different remotes cannot share
 * one address+button. Caller holds the lock. */
static db_signal_meta_t *match_decoded(const rf_decoded_t *d)
{
    for (int i = 0; i < s_count; i++) {
        db_signal_meta_t *m = &s_meta[i];
        if (!m->decoded_valid)
            continue;
        if (m->decoded_id != d->id || m->decoded_button != d->button)
            continue;
        if (strncmp(m->protocol, d->protocol, RF_PROTOCOL_NAME_MAX) != 0)
            continue;
        return m;
    }
    return NULL;
}

uint16_t db_signals_match(const rf_event_t *ev)
{
    if (!ev || ev->frame.count == 0 || s_count == 0)
        return 0;

    lock();

    bool fragment = judge_burst(ev);

    db_signal_meta_t *hit = NULL;
    if (ev->decoded.valid)
        hit = match_decoded(&ev->decoded);
    if (!hit && ev->fingerprint != 0) {
        /* Fingerprint fallback. It already encodes the pulse count and every
         * pulse as a multiple of the frame's own estimated base, so it is stable
         * across oscillator drift and cannot collide across different lengths. */
        for (int i = 0; i < s_count; i++) {
            if (s_meta[i].fingerprint == ev->fingerprint) {
                hit = &s_meta[i];
                break;
            }
        }
    }

    if (fragment) {
        /* Whatever it looks like, it is the tail of the press we just handled.
         * Firing here would either double-ring the chime or, worse, ring the
         * WRONG one. Debug level: this is expected behaviour, not a fault. */
        ESP_LOGD(TAG, "ignoring %u-pulse tail fragment after a %u-pulse frame%s",
                 ev->frame.count, s_recent.ref_pulses,
                 hit ? " (it would have matched a stored signal)" : "");
        unlock();
        return 0;
    }

    uint16_t id = 0;
    if (hit) {
        hit->last_seen_us = ev->ts_us;
        hit->seen_count++;
        id = hit->id;
        s_recent.matched_id = id;
        ESP_LOGI(TAG, "matched signal %u '%s' (%u repeats, %d dBm, seen %lu)",
                 (unsigned)id, hit->name, ev->repeats, ev->rssi_dbm,
                 (unsigned long)hit->seen_count);
    }

    unlock();
    return id;
}

/* ---- virtual signals ----------------------------------------------------- */

/* Caller holds the lock. True if some stored signal already owns this address,
 * whatever button nibble it carries. Used ONLY when drawing a random address:
 * "brand new, nothing on this box uses it" is the whole promise of that draw, so
 * there the bar is the address and not the full code. */
static bool address_taken(uint32_t id20)
{
    for (int i = 0; i < s_count; i++) {
        if (s_meta[i].decoded_valid &&
            strncmp(s_meta[i].protocol, "ev1527", RF_PROTOCOL_NAME_MAX) == 0 &&
            s_meta[i].decoded_id == id20)
            return true;
    }
    return false;
}

/* Caller holds the lock. The stored signal carrying this exact identity, if any.
 * Same key as match_decoded() — see the header for why that is the right bar. */
static db_signal_meta_t *find_decoded(const char *protocol, uint32_t id, uint8_t button)
{
    for (int i = 0; i < s_count; i++) {
        db_signal_meta_t *m = &s_meta[i];
        if (!m->decoded_valid)
            continue;
        if (m->decoded_id != id || m->decoded_button != button)
            continue;
        if (strncmp(m->protocol, protocol, RF_PROTOCOL_NAME_MAX) != 0)
            continue;
        return m;
    }
    return NULL;
}

const db_signal_meta_t *db_signals_find_decoded(const char *protocol,
                                                uint32_t id, uint8_t button)
{
    if (!protocol)
        return NULL;
    lock();
    const db_signal_meta_t *m = find_decoded(protocol, id, button);
    unlock();
    return m;
}

esp_err_t db_signals_create_virtual(const char *name, uint32_t id20, uint8_t button4,
                                    uint16_t base_us, bool allow_duplicate,
                                    uint16_t *id_out)
{
    id20 &= 0x000FFFFFu;
    button4 &= 0x0Fu;

    if (id20 == 0) {
        /* Draw a fresh 20-bit address. esp_random() is a hardware RNG here (the
         * radio/Wi-Fi is up by the time anyone can reach the REST API), and the
         * collision check is against OUR store only — we cannot know what the
         * neighbours transmit, which is why the address is 20 bits wide in the
         * first place. */
        lock();
        for (int tries = 0; tries < 32 && id20 == 0; tries++) {
            uint32_t cand = esp_random() & 0x000FFFFFu;
            if (cand == 0 || address_taken(cand))
                continue;
            id20 = cand;
        }
        unlock();

        if (id20 == 0) {
            ESP_LOGE(TAG, "could not draw a free EV1527 address");
            return ESP_ERR_NOT_FOUND;
        }
    } else if (!allow_duplicate) {
        /* Refuse only what the MATCHER cannot tell apart: the same protocol,
         * address AND button. Refusing the address alone (which this used to do)
         * banned the single most ordinary thing an EV1527 user has — a
         * multi-button remote, four buttons on one factory-burned address — and
         * it is also what stopped the reporter re-creating a code they had
         * captured. Neither is a hazard; an ambiguous match is. */
        lock();
        const db_signal_meta_t *clash = find_decoded("ev1527", id20, button4);
        uint16_t clash_id = clash ? clash->id : 0;
        unlock();
        if (clash_id != 0) {
            ESP_LOGW(TAG, "EV1527 id=0x%05lX btn=0x%X is already signal %u",
                     (unsigned long)id20, button4, (unsigned)clash_id);
            return ESP_ERR_INVALID_STATE;   /* the API maps this to 409 */
        }
    }

    /* 1 KB — never on the caller's stack; the HTTP task's is not that generous. */
    static rf_frame_t frame;
    if (!rf_ev1527_build(id20, button4, base_us, &frame)) {
        ESP_LOGE(TAG, "rf_ev1527_build failed (id=0x%05lX btn=0x%X base=%u)",
                 (unsigned long)id20, button4, base_us);
        return ESP_ERR_INVALID_ARG;
    }

    /* Let add_frame re-derive the decode from the synthesized waveform rather
     * than asserting it: if the encoder and the decoder ever disagree, the
     * signal that gets stored is the one the receiver would actually hear. */
    esp_err_t err = db_signals_add_frame(&frame, NULL, name,
                                         DB_ORIGIN_SYNTHESIZED, id_out);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "virtual signal: EV1527 id=0x%05lX btn=0x%X, %u pulses",
                 (unsigned long)id20, button4, frame.count);
    return err;
}

/* ---- host-test hook ------------------------------------------------------- */

#ifdef DB_HOSTTEST
/* Compiled ONLY into host-test/test_node_graph (its Makefile defines
 * DB_HOSTTEST; no device build does). Forgetting the resident state is what
 * lets one test binary simulate several BOOTS against the same fake flash —
 * the power-cut repairs (reconcile_flash) only ever run inside
 * db_signals_init(), so testing them requires dying and coming back. */
void db_signals_hosttest_reset(void)
{
    memset(s_meta, 0, sizeof(s_meta));
    s_count = 0;
    s_ready = false;
    s_index_authoritative = false;
    memset(&s_recent, 0, sizeof(s_recent));
}
#endif
