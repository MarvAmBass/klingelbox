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
 * ONE MUTEX, HELD BRIEFLY. Matching runs on the dispatch task (app_main.c) —
 * which holds no other locks and may block freely; the capture task only
 * queues events to it — while the REST layer mutates from the HTTP task. The
 * metadata scan is a few hundred integer comparisons over a resident array,
 * and the similarity stage compares against frames CACHED in RAM (below), so
 * the common hot path still touches no flash. Everything that does touch
 * flash (add/delete/rename/load_frame, and a similarity cache miss past the
 * cache's eight slots) runs under the same lock, accepting that a match may
 * wait a few milliseconds behind a save. That is far preferable to a
 * lock-free scan racing an index rewrite — and blocking is harmless here
 * precisely because matching left the capture task: the radio keeps
 * listening while a match waits.
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
 * SIMILARITY MATCHING FOR RAW SIGNALS (live box, 2026-10)
 *
 * The Listen flow learned the user's "Gartentor" — 224 pulses, undecodable,
 * confidence 24 % — by joining the fragments the capture layer chopped it
 * into. The LIVE path then could not match it: raw signals matched only by
 * exact fingerprint, and this transmitter's long pattern fragments
 * DIFFERENTLY on every reception — one physical press produced six distinct
 * unmatched fingerprints inside nine seconds, one of them a clean 196-pulse
 * truncation of the stored 224. A signal the box demonstrably understood
 * offline practically never rang online.
 *
 * So the run machinery's concepts are ported to the match path, as a LAST
 * resort strictly behind decoded identity and the fingerprint:
 *
 *   1. SINGLE-BURST SIMILARITY. An unmatched burst is compared per-pulse
 *      against every stored raw signal (decoded signals are never in play —
 *      their identity match is stronger than any timing argument). Both the
 *      whole frame and a clean leading truncation are accepted, down to a
 *      per-preset minimum fraction of the stored pulse count.
 *
 *   2. RUN ASSEMBLY. Bursts that match nothing are accumulated into a rolling
 *      run — joined with the silences actually measured between them, exactly
 *      as rf_frame_join_run() rebuilds a chopped transmission in the Listen
 *      flow — and the assembled run is compared the same way. The pieces'
 *      boundaries need not agree with anything: the join reconstructs the
 *      original waveform (a cut happens INSIDE a long low, and the measured
 *      gap is that low), so the comparison is always joined-live against
 *      stored, whole waveform against whole waveform.
 *
 * UNIT MATCHING (field data, 2026-10) — the stage that finally fits the
 * Gartentor-class transmitter. That button repeats its word CONTINUOUSLY with
 * inter-repeat gaps below the capture idle threshold, so its "frames" are
 * arbitrary multi-word windows cut wherever the 512-pulse ceiling or a chance
 * gap lands — different pulse counts and fingerprints on every press, which
 * is why even stages 1 and 2 above could not be stable for it (they compare
 * window shapes, and this transmitter has none). The stable object is the
 * repeating WORD, so that is what gets compared:
 *
 *   3. SLIDING UNIT ALIGNMENT. A signal whose stored waveform is such a unit
 *      — learned directly by the Listen flow (db_signals_add_frame_ex), or
 *      derived at load time from a long stored window that itself contains
 *      >= 2 repeats (so the user's existing signals start working without a
 *      re-learn; the derived unit lives in the RAM cache only and the stored
 *      waveform is never rewritten) — is matched by rf_unit_match(): does a
 *      contiguous stretch of the incoming window consist of this unit
 *      repeating, starting at ANY phase? Every pulse of that stretch must
 *      agree cyclically, endpoints' clipping excepted, and the per-signal
 *      preset sets both the per-pulse window and the minimum number of FULL
 *      aligned repeats the stretch must carry (SIM_PRESETS). Both sides of
 *      the comparison are DESPIKED first (rf_frame_despike, 2026-10-23 field
 *      fix): sub-100 us glitch spikes insert index-shifting entries that no
 *      index-aligned comparison survives, and real windows always carry a
 *      few. The verbatim window and verbatim stored frames remain what
 *      stages 1 and 2 compare and what storage and replay use. The stage
 *      composes with 1 and 2 unchanged: an assembled run is slid over
 *      exactly like a single burst, the press window absorbs the stream of
 *      windows one held button produces, and decoded signals never
 *      participate.
 *
 * HOW THIS COMPOSES WITH THE FRAGMENT SUPPRESSOR ABOVE — read carefully, the
 * two look like rivals and are not. The suppressor answers "is this burst the
 * TAIL of the press I just handled?" (sub-75 % of its predecessor, inside
 * 750 ms) and exists to stop one press ringing twice; the assembler answers
 * "are these bursts PIECES of one press nobody has handled yet?". They divide
 * the work by burst size: a piece big enough to clear the 75 % bar is a main
 * in its own right and only the assembler can explain it, while a sub-75 %
 * tail is suppressed from firing ON ITS OWN evidence — but still offered to
 * the assembler, because the second half of a chopped transmission IS such a
 * "tail" by size, and completing the run is the one way it may legitimately
 * cause a ring (the run subsumes the unmatched main it follows, so that is
 * the press's FIRST ring, not a second one). After any fire the assembler is
 * reset and a per-signal press window (s_sim_press) absorbs the transmitter's
 * own repeats and stragglers, so the suppressor keeps its job (tails never
 * fire alone), the assembler gets its job (pieces fire once, assembled), and
 * neither can double-ring the other's press.
 *
 * FALSE-POSITIVE DISCIPLINE. A wrong match here rings a bell in someone's
 * house, so every similarity verdict requires agreement of the FULL compared
 * waveform, pulse by pulse, gaps included — never a statistic like length or
 * base width alone. The thresholds are per-signal presets (see SIM_PRESETS
 * below for the numbers and their provenance), because the right slack is a
 * property of the individual transmitter. And because similarity is an
 * inference rather than an identification, every similarity fire is labelled
 * as such in the event feed and counted separately (similarity_matches) — the
 * way the mechanism earns trust on a live box is by being auditable.
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
#include "db_storage.h"  /* breadcrumb when a write hits a full partition */
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"   /* NVS_DEFAULT_PART_NAME, for stats and the sweep */
#include "rf_group.h"    /* the run machinery's tolerances and join primitives */
#include "rf_unit.h"     /* repeating-unit extraction and sliding alignment */

static const char *TAG = "db_signals";

#define DB_SIG_NS        "dbsig"
#define DB_SIG_INDEX_KEY "index"
/* v1: the original record (db_signal_meta_v1_t, frozen in signal_store.h).
 * v2: match_tolerance + similarity_matches appended (db_signal_meta_v2_t).
 * v3: unit_repeats + unit_gap_us appended. Older indexes are migrated on
 *     load and written back (graph discipline, node_graph.c); anything
 *     NEWER or unrecognized is ignored-but-preserved exactly as before, so a
 *     rollback still finds its signals intact. */
#define DB_SIG_VERSION   3u

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
    int64_t  last_ts_us;    /* ts_us of the last MAIN frame (fragments do not
                               move the window anchor — see judge_burst)       */
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
        dst[i].last_seen_us       = 0;
        dst[i].seen_count         = 0;
        dst[i].similarity_matches = 0;
    }

    size_t len = sizeof(hdr) + (size_t)s_count * sizeof(db_signal_meta_t);
    err = nvs_set_blob(h, DB_SIG_INDEX_KEY, s_blob, len);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "index save failed: %s", esp_err_to_name(err));
        /* An index save failing for SPACE should be unreachable — the flash
         * budget below refuses adds first — so if it ever happens the budget's
         * arithmetic is wrong somewhere and the breadcrumb is exactly the
         * evidence that says so. (The deliberate budget REFUSAL in store() is
         * NOT reported here: refusing with headroom left is the mechanism
         * working, not storage trouble.) */
        db_storage_note_write_failure("signals", err);
    }
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

    /*
     * A v1 index is this build's OWN past, read through the frozen layout and
     * brought forward field by field (the node graph's migration discipline).
     * Everything migrated defaults match_tolerance to NORMAL — the zero value,
     * but set explicitly so the default is a decision and not an accident.
     */
    if (hdr.version == 1u && hdr.meta_size == sizeof(db_signal_meta_v1_t)) {
        uint32_t n = hdr.count;
        if (n > DB_SIGNAL_MAX)
            n = DB_SIGNAL_MAX;
        if (len < sizeof(hdr) + (size_t)n * sizeof(db_signal_meta_v1_t)) {
            ESP_LOGE(TAG, "v1 signal index truncated — starting empty");
            return ESP_OK;
        }
        s_count = 0;
        for (uint32_t i = 0; i < n; i++) {
            /* Copied OUT of the staging buffer first: it is a byte array
             * behind a blob header, so a record is not guaranteed aligned for
             * in-place int64 reads (same note as db_node_widen_v3). */
            db_signal_meta_v1_t old;
            memcpy(&old, s_blob + sizeof(hdr) + (size_t)i * sizeof(old),
                   sizeof(old));
            if (old.id == 0)
                continue;

            db_signal_meta_t *m = &s_meta[s_count];
            memset(m, 0, sizeof(*m));
            m->id          = old.id;
            memcpy(m->name, old.name, sizeof(m->name));
            m->name[DB_SIGNAL_NAME_MAX - 1] = '\0';
            m->origin      = old.origin;
            m->created_at  = old.created_at;
            m->fingerprint = old.fingerprint;
            m->base_us     = old.base_us;
            m->confidence  = old.confidence;
            m->pulse_count = old.pulse_count;
            m->decoded_valid = old.decoded_valid;
            memcpy(m->protocol, old.protocol, sizeof(m->protocol));
            m->protocol[RF_PROTOCOL_NAME_MAX - 1] = '\0';
            m->decoded_id     = old.decoded_id;
            m->decoded_button = old.decoded_button;
            /* The one new setting. Every signal that existed before presets
             * behaves exactly as the default preset describes. */
            m->match_tolerance = DB_SIG_TOL_NORMAL;
            s_count++;
        }
        s_index_authoritative = true;
        ESP_LOGI(TAG, "signal index migrated v1 -> v%u (%d signal(s))",
                 (unsigned)DB_SIG_VERSION, s_count);
        /* Write the current layout back so the migration is a one-off. Best
         * effort: RAM is already right, and a failed save is retried by the
         * next mutation or boot (same reasoning as node_graph's write-back). */
        save_index();
        return ESP_OK;
    }

    /* A v2 index (the preset-era layout, v0.11.0) comes forward the same way.
     * The two new fields default to 0 — "no repeat structure known", which is
     * exactly true of a signal stored before units existed; load-time unit
     * DERIVATION (sim_slot_derive below) is what makes such a signal match a
     * repeating transmitter anyway, without touching its stored bytes. */
    if (hdr.version == 2u && hdr.meta_size == sizeof(db_signal_meta_v2_t)) {
        uint32_t n = hdr.count;
        if (n > DB_SIGNAL_MAX)
            n = DB_SIGNAL_MAX;
        if (len < sizeof(hdr) + (size_t)n * sizeof(db_signal_meta_v2_t)) {
            ESP_LOGE(TAG, "v2 signal index truncated — starting empty");
            return ESP_OK;
        }
        s_count = 0;
        for (uint32_t i = 0; i < n; i++) {
            /* Copied out first: blob records are not guaranteed aligned for
             * in-place int64 reads (same note as the v1 branch). */
            db_signal_meta_v2_t old;
            memcpy(&old, s_blob + sizeof(hdr) + (size_t)i * sizeof(old),
                   sizeof(old));
            if (old.id == 0)
                continue;

            db_signal_meta_t *m = &s_meta[s_count];
            memset(m, 0, sizeof(*m));
            m->id          = old.id;
            memcpy(m->name, old.name, sizeof(m->name));
            m->name[DB_SIGNAL_NAME_MAX - 1] = '\0';
            m->origin      = old.origin;
            m->created_at  = old.created_at;
            m->fingerprint = old.fingerprint;
            m->base_us     = old.base_us;
            m->confidence  = old.confidence;
            m->pulse_count = old.pulse_count;
            m->decoded_valid = old.decoded_valid;
            memcpy(m->protocol, old.protocol, sizeof(m->protocol));
            m->protocol[RF_PROTOCOL_NAME_MAX - 1] = '\0';
            m->decoded_id      = old.decoded_id;
            m->decoded_button  = old.decoded_button;
            m->match_tolerance = (old.match_tolerance < DB_SIG_TOL__COUNT)
                                     ? old.match_tolerance : DB_SIG_TOL_NORMAL;
            s_count++;
        }
        s_index_authoritative = true;
        ESP_LOGI(TAG, "signal index migrated v2 -> v%u (%d signal(s))",
                 (unsigned)DB_SIG_VERSION, s_count);
        save_index();   /* one-off write-back, best effort (see the v1 branch) */
        return ESP_OK;
    }

    /* Beyond that, only an exact layout match is reinterpreted (same rule as
     * db_config.c). A NEWER or unknown index is left on flash untouched — a
     * rollback to the firmware that wrote it must find its signals intact —
     * and we simply start empty. */
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
        s_meta[s_count].last_seen_us       = 0;
        s_meta[s_count].seen_count         = 0;
        s_meta[s_count].similarity_matches = 0;
        /* An out-of-range preset (an index hand-edited, or written by a build
         * that knew more presets) must not index past the table. */
        if (s_meta[s_count].match_tolerance >= DB_SIG_TOL__COUNT)
            s_meta[s_count].match_tolerance = DB_SIG_TOL_NORMAL;
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

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame save (%s) failed: %s", key, esp_err_to_name(err));
        /* Same reasoning as save_index(): the budget should have refused the
         * add before this write ever started. */
        db_storage_note_write_failure("signals", err);
    }
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

/* ---- similarity matching for raw signals ----------------------------------
 *
 * See SIMILARITY MATCHING in the file header for what this is and why. The
 * mechanics live here; the policy numbers are the preset table directly below.
 */

/* Defined with the rest of the persistence below; the cache needs it for the
 * fallback load past its slot count. */
static esp_err_t load_frame_locked(uint16_t id, rf_frame_t *out);

/*
 * THE PRESET TABLE — every similarity threshold in one place, with provenance.
 *
 * Two knobs per preset:
 *
 *   tol_pct/tol_us  the per-pulse window for "the same transmitter, drifted".
 *                   NORMAL and STRICT use 25 % / 120 us — the exact window
 *                   rf_service's burst coalescing already trusts, dozens of
 *                   times per press, to say "this frame is the previous frame
 *                   again" (BURST_TOL_PCT/_US in rf_service.c; the numbers are
 *                   repeated here because that module owns the radio and this
 *                   one must not include it). LOOSE widens to the run
 *                   machinery's own window (RF_GROUP_TOL_*, 30 % / 150 us) —
 *                   the tolerance that recognized the Gartentor transmitter's
 *                   repeats as one group in the Listen flow, i.e. the widest
 *                   "same transmission" window this codebase has evidence for.
 *
 *   min_pct         the least of the stored frame an incoming waveform must
 *                   cover (as a leading, pulse-by-pulse-agreeing prefix) to
 *                   count as that signal. The anchors are measured, not
 *                   invented: the live failure's clean truncation carried
 *                   196 of 224 pulses = 87.5 %, so NORMAL sits just under it
 *                   at 85 %; the fragment suppressor's DB_FRAG_MIN_PCT (75 %)
 *                   is the floor below which a prefix is size-indistinguishable
 *                   from the tail of some OTHER long press, so LOOSE sits
 *                   exactly on it and nothing may go lower; STRICT at 95 %
 *                   accepts only a whisker of clipping — "near-exact or
 *                   nothing", for a signal whose owner has seen a false ring.
 *
 *   min_rep         the UNIT stage's bar (rf_unit_match): the fewest FULL
 *                   aligned repeats of the signal's unit an incoming window
 *                   must carry. The unit stage already demands that the
 *                   WHOLE window agree with the unit cyclically, pulse by
 *                   pulse — one foreign pulse refuses it — so min_rep is
 *                   about how much positive evidence rides on top of that:
 *                   NORMAL takes one full unit (the press window then absorbs
 *                   the rest of the stream, exactly as it absorbs repeats of
 *                   a prefix match); STRICT demands two aligned repeats —
 *                   repetition in ONE window, the same "repeats prove a
 *                   transmitter" bar rf_group ranks by, for a signal whose
 *                   owner has seen a false ring; LOOSE stays at one unit and
 *                   gets its slack from the wider per-pulse window instead,
 *                   because lowering BOTH bars at once is how a preset stops
 *                   meaning anything.
 *
 * Run gap agreement comes for free and is therefore not a third knob: an
 * assembled run embeds its measured silences as ordinary low pulses (see
 * sim_run_offer), and the stored frame — joined by the Listen flow from the
 * same machinery — embeds its own, so the per-pulse window above judges the
 * gaps exactly as rf_run_similar()'s gap_similar() would, same shape, same
 * tolerance.
 */
typedef struct {
    uint8_t  tol_pct;
    uint16_t tol_us;
    uint8_t  min_pct;   /* 0 disables the preset (OFF) */
    uint8_t  min_rep;   /* unit stage: fewest full aligned repeats */
} sim_preset_t;

static const sim_preset_t SIM_PRESETS[DB_SIG_TOL__COUNT] = {
    [DB_SIG_TOL_NORMAL] = { 25, 120, 85, 1 },
    [DB_SIG_TOL_OFF]    = {  0,   0,  0, 0 },
    [DB_SIG_TOL_STRICT] = { 25, 120, 95, 2 },
    [DB_SIG_TOL_LOOSE]  = { RF_GROUP_TOL_PCT, RF_GROUP_TOL_US, DB_FRAG_MIN_PCT, 1 },
};

/* The per-pulse window for DERIVING a unit from one stored capture: within a
 * single recording the oscillator barely moves, so this is the same 25 % /
 * 120 us "same transmission" window burst coalescing trusts (BURST_TOL_PCT/_US
 * in rf_service.c — repeated because that module owns the radio and this one
 * must not include it). Matching against the derived unit then runs under the
 * per-signal preset above, which is where cross-press drift belongs. */
#define SIM_UNIT_TOL_PCT 25
#define SIM_UNIT_TOL_US  120

/*
 * THE WAVEFORM CACHE. The hot path may not read NVS per burst, but similarity
 * needs the stored frames the metadata split deliberately left on flash. So
 * the raw signals' waveforms are cached in RAM: filled at boot and on
 * add/delete, LRU-replaced on a fallback load past the slot count.
 *
 * Eight slots, not thirty-two, and static, not heap. The arithmetic: a slot
 * is ~1.05 KB, so eight cost ~8.4 KB of BSS where all 32 would cost ~34 KB —
 * and the live box runs ~112 KB of free heap against a 48 KB capture reserve
 * plus up to ~36 KB for a raw listening session (rf_raw.h), which leaves room
 * for the former and decidedly not for the latter. Eight also covers every
 * real deployment seen so far (a handful of raw signals; most stores are
 * mostly decoded), so the NVS fallback is the exception it is allowed to be.
 * Static because this module allocates nothing at runtime by policy — and a
 * cache that could fail to exist on a fragmented heap would make matching
 * nondeterministic in exactly the situations that are hardest to debug.
 */
#define DB_SIM_CACHE_SLOTS 8

/*
 * THE DERIVED-UNIT SIDECAR. A pre-unit signal whose stored waveform is a long
 * window of a repeating stream (the user's 224-pulse Gartentor) gets its unit
 * derived HERE, when the frame enters the cache — RAM only, the stored bytes
 * are never rewritten, so deriving beats re-learning and a rollback sees the
 * flash it wrote. 128 pulses of sidecar (256 bytes a slot, ~2 KB over the
 * cache) covers any real word — EV1527-class is ~50 — where a second full
 * rf_frame_t per slot would double the cache's 8.4 KB for tail that no unit
 * ever uses; a unit longer than the sidecar is simply not kept, and that
 * signal matches by the v0.11.0 whole/prefix stages exactly as before.
 */
#define DB_SIM_UNIT_MAX_PULSES 128

typedef struct {
    bool     tried;        /* extraction attempted for this slot's frame */
    uint16_t count;        /* 0: no derived unit */
    uint8_t  first_level;
    uint16_t repeats;      /* repeats observed in the stored window */
    uint16_t durations_us[DB_SIM_UNIT_MAX_PULSES];
} sim_unit_t;

typedef struct {
    uint16_t   id;        /* 0 = empty slot */
    int64_t    used_us;   /* LRU stamp */
    rf_frame_t frame;
    sim_unit_t unit;      /* derived from `frame` — see the sidecar note */
} sim_slot_t;

static sim_slot_t s_sim_cache[DB_SIM_CACHE_SLOTS];

/* Derive the slot's unit from its frame, once per fill. A signal stored AS a
 * unit (meta->unit_repeats != 0) needs no sidecar — its frame is the unit —
 * and extraction over it would only find sub-structure that is not there.
 * Caller holds the lock. */
static void sim_slot_derive(sim_slot_t *s)
{
    static rf_unit_t  u;   /* ~1 KB — static by the no-malloc policy; the lock
                              serializes every caller */
    static rf_frame_t df;  /* the slot frame, despiked for extraction — the
                              slot's own copy stays verbatim because the
                              window stages compare against what was stored */
    const db_signal_meta_t *m = find_meta(s->id);

    s->unit.tried = true;
    s->unit.count = 0;
    if (m == NULL || m->unit_repeats != 0)
        return;
    df = s->frame;
    rf_frame_despike(&df, RF_UNIT_DESPIKE_US);
    if (!rf_unit_extract(&df, SIM_UNIT_TOL_PCT, SIM_UNIT_TOL_US, &u))
        return;
    if (u.unit.count > DB_SIM_UNIT_MAX_PULSES)
        return;   /* longer than the sidecar: whole/prefix matching remains */
    s->unit.count       = u.unit.count;
    s->unit.first_level = u.unit.first_level;
    s->unit.repeats     = u.repeats;
    memcpy(s->unit.durations_us, u.unit.durations_us,
           (size_t)u.unit.count * sizeof(uint16_t));
    ESP_LOGI(TAG, "signal %u '%s': derived a %u-pulse repeating unit from the "
                  "stored %u-pulse window (%u repeats observed)",
             (unsigned)s->id, m->name, u.unit.count, s->frame.count, u.repeats);
}

/*
 * THE RUN ASSEMBLER. Unmatched bursts arriving close enough together to be
 * pieces of one chopped press are joined — measured silences and all — into
 * one rolling frame, compared against the store after each piece. Bounded by
 * DB_RAW_MAX_PARTS, the same "pieces of ONE press" limit the raw session's
 * candidates use: a transmission in more pieces than that is a threshold
 * problem, not a matching problem. Two static kilobytes (run + join scratch).
 */
static struct {
    rf_frame_t run;        /* the pieces joined so far */
    uint8_t    pieces;
    int64_t    last_ts_us; /* ts_us of the last piece's event (its completion) */
} s_sim_run;
static rf_frame_t s_sim_scratch;   /* rf_frame_join cannot join in place */

/*
 * THE PRESS WINDOW. One physical press of a fragmenting transmitter is many
 * bursts over several seconds (the live box saw six distinct fingerprints in
 * nine), and more than one of them can legitimately satisfy similarity — the
 * first full repeat after an assembled fire, say. Firing each would ring one
 * press twice or six times, so after a similarity fire further similarity
 * hits ON THE SAME SIGNAL are absorbed while evidence keeps arriving inside
 * the fragment window, re-armed per hit exactly like burst coalescing — and,
 * exactly like burst coalescing, hard-capped (BURST_MAX_US's five seconds) so
 * a stuck transmitter surfaces as one ring per five seconds instead of
 * silence. Absorption is per-signal and similarity-only: it can never delay a
 * decoded or fingerprint match of anything, so it cannot latch the way the
 * old fragment window could.
 */
#define DB_SIM_PRESS_MAX_US (5 * 1000 * 1000)

static struct {
    uint16_t id;        /* 0: no similarity press open */
    int64_t  first_us;
    int64_t  last_us;
} s_sim_press;

/* ---- cache mechanics. Caller holds the lock for all of these. ---- */

static void sim_cache_drop(uint16_t id)
{
    for (int i = 0; i < DB_SIM_CACHE_SLOTS; i++)
        if (s_sim_cache[i].id == id)
            s_sim_cache[i].id = 0;
}

/* The slot `id` should live in: its current one, else an empty one, else the
 * least recently used. */
static sim_slot_t *sim_cache_victim(uint16_t id)
{
    sim_slot_t *lru = &s_sim_cache[0];
    for (int i = 0; i < DB_SIM_CACHE_SLOTS; i++) {
        sim_slot_t *s = &s_sim_cache[i];
        if (s->id == id)
            return s;
        if (s->id == 0)
            lru = s;                 /* empty beats evicting anyone */
        else if (lru->id != 0 && s->used_us < lru->used_us)
            lru = s;
    }
    return lru;
}

static void sim_cache_put(uint16_t id, const rf_frame_t *frame)
{
    sim_slot_t *s = sim_cache_victim(id);
    s->id      = id;
    s->used_us = esp_timer_get_time();
    s->frame   = *frame;
    sim_slot_derive(s);
}

/* The slot of raw signal `id` — frame plus derived unit — from RAM when
 * cached, from NVS when not: the one sanctioned flash read on the match path,
 * promoted into the LRU slot so a signal that is actually being pressed stays
 * resident. NULL only when the frame is unreadable, which the boot reconcile
 * makes nearly unreachable. */
static const sim_slot_t *sim_slot_get(uint16_t id)
{
    for (int i = 0; i < DB_SIM_CACHE_SLOTS; i++) {
        if (s_sim_cache[i].id == id) {
            s_sim_cache[i].used_us = esp_timer_get_time();
            if (!s_sim_cache[i].unit.tried)
                sim_slot_derive(&s_sim_cache[i]);
            return &s_sim_cache[i];
        }
    }
    {
        sim_slot_t *s = sim_cache_victim(id);
        if (load_frame_locked(id, &s->frame) != ESP_OK) {
            s->id = 0;      /* never leave a half-loaded frame under an id */
            return NULL;
        }
        s->id      = id;
        s->used_us = esp_timer_get_time();
        sim_slot_derive(s);
        return s;
    }
}

/* Boot-time fill: the first DB_SIM_CACHE_SLOTS raw signals, in store order —
 * at boot there are no seen-stats to rank by, and the LRU corrects any wrong
 * guess at the cost of one flash read on that signal's first press. Units are
 * derived here too, so the first press after boot pays no extraction. */
static void sim_cache_prefill(void)
{
    int cached = 0;
    for (int i = 0; i < s_count && cached < DB_SIM_CACHE_SLOTS; i++) {
        if (s_meta[i].decoded_valid)
            continue;
        sim_slot_t *s = &s_sim_cache[cached];
        if (load_frame_locked(s_meta[i].id, &s->frame) != ESP_OK)
            continue;
        s->id      = s_meta[i].id;
        s->used_us = esp_timer_get_time();
        sim_slot_derive(s);
        cached++;
    }
}

/* ---- the comparison and the scan. Caller holds the lock. ---- */

/* Is `a` plausibly `b`'s base width, drifted? rf_group's window, used here as
 * a metadata PREFILTER only — wide enough that it can only reject candidates
 * whose per-pulse comparison would reject anyway, cheap enough to run before
 * any frame is fetched. Skipped when either estimate is missing: a 0 base is
 * "the normalizer could not tell", not "the bases differ". */
static bool sim_base_close(uint16_t a, uint16_t b)
{
    uint32_t diff, ref, allow;

    if (a == 0 || b == 0)
        return true;
    diff  = (a > b) ? (uint32_t)(a - b) : (uint32_t)(b - a);
    ref   = (a > b) ? a : b;
    allow = (ref * RF_GROUP_TOL_PCT) / 100u;
    if (allow < RF_GROUP_TOL_US)
        allow = RF_GROUP_TOL_US;
    return diff <= allow;
}

/* The verdict: whole-frame agreement, or a clean leading truncation covering
 * at least the preset's fraction. Count mismatch the other way (incoming
 * LONGER than stored) is refused outright — matching more than was stored is
 * a claim the stored evidence cannot support. */
static bool sim_waveform_match(const rf_frame_t *stored, const rf_frame_t *in,
                               const sim_preset_t *p)
{
    if (in->count == 0 || in->count > stored->count)
        return false;
    if ((uint32_t)in->count * 100u < (uint32_t)stored->count * p->min_pct)
        return false;
    if (in->count == stored->count)
        return rf_frame_similar(stored, in, p->tol_pct, p->tol_us);
    return rf_frame_similar_prefix(stored, in, p->tol_pct, p->tol_us);
}

/*
 * Scan the stored RAW signals for one that `in` is a reception of. When
 * several qualify (a stored truncation next to the stored whole, say), the
 * strongest claim wins, ranked by how much of the comparison was anchored:
 * an exact pulse-count match (rank 2) beats a unit match (rank 1 — the whole
 * window agreed, but cyclically), which beats a prefix match (rank 0 — only
 * part of the stored frame was covered). Among unit matches more aligned
 * repeats win; among prefix matches the highest covered fraction (= the
 * shortest stored frame) wins. `in_base_us` 0 skips the base prefilter — an
 * assembled run is never re-normalized, because its full per-pulse comparison
 * is the stronger test anyway and runs on a handful of candidates at most.
 */
static db_signal_meta_t *sim_scan(const rf_frame_t *in, uint16_t in_base_us)
{
    /* The incoming window, despiked once for the unit stage (rf_unit.h's
     * RF_UNIT_DESPIKE_US; static per the no-malloc policy, the lock
     * serializes callers). Prepared lazily: most scans end in the window
     * stages or in no raw signals at all. The window stages keep comparing
     * the VERBATIM window against the verbatim stored frames — their
     * artifacts are shaped alike — while index-aligned unit matching is
     * exactly what spikes break (field data, 2026-10-23: the box's own
     * learned button never matched because every live window carried a few
     * 25-76 us spikes). */
    static rf_frame_t s_in_clean;
    bool in_clean_ready = false;

    db_signal_meta_t *best = NULL;
    int      best_rank = -1;
    uint32_t best_key  = 0;    /* rank 1: repeats (more wins);
                                  rank 0: 65535 - stored count (shorter wins) */

    for (int i = 0; i < s_count; i++) {
        db_signal_meta_t *m = &s_meta[i];
        const sim_preset_t *p;
        const sim_slot_t *slot;
        int      rank = -1;
        uint32_t key  = 0;

        if (m->decoded_valid)
            continue;      /* identity territory; similarity never applies */
        p = &SIM_PRESETS[m->match_tolerance < DB_SIG_TOL__COUNT
                             ? m->match_tolerance : DB_SIG_TOL_NORMAL];
        if (p->min_pct == 0)
            continue;      /* OFF: this signal matches by fingerprint alone */

        /* Window-shaped stages (whole frame / leading truncation), exactly
         * v0.11.0's: only when the incoming window is not LONGER than the
         * stored frame — matching more than was stored is a claim the stored
         * evidence cannot support, and the unit stage below is the honest way
         * a longer window gets matched. The base-width prefilter gates ONLY
         * these stages: it was meant as a property of the transmitter, but
         * glitch spikes drag a window's estimate far off (the live box stored
         * the same button once as base 50 and once as 245), so the unit stage
         * below must not be vetoed by it — per-pulse cyclic agreement over a
         * whole word is the stronger test anyway. */
        if (sim_base_close(in_base_us, m->base_us) &&
            in->count <= m->pulse_count &&
            (uint32_t)in->count * 100u >= (uint32_t)m->pulse_count * p->min_pct) {
            slot = sim_slot_get(m->id);
            if (slot && sim_waveform_match(&slot->frame, in, p)) {
                rank = (in->count == m->pulse_count) ? 2 : 0;
                key  = 65535u - m->pulse_count;
            }
        }

        /* The unit stage: slide the signal's unit over the incoming window
         * (any phase), requiring the preset's minimum of full repeats. The
         * unit is the stored frame itself when the signal was LEARNED as one
         * (unit_repeats != 0), else the sidecar derived at load time. */
        if (rank < 2) {
            const uint16_t *ud = NULL;
            uint16_t uc = 0;
            uint8_t  ul = 0;

            slot = sim_slot_get(m->id);
            if (slot && m->unit_repeats != 0) {
                ud = slot->frame.durations_us;
                uc = slot->frame.count;
                ul = slot->frame.first_level;
            } else if (slot && slot->unit.count != 0) {
                ud = slot->unit.durations_us;
                uc = slot->unit.count;
                ul = slot->unit.first_level;
            }
            if (ud != NULL) {
                if (!in_clean_ready) {
                    s_in_clean = *in;
                    rf_frame_despike(&s_in_clean, RF_UNIT_DESPIKE_US);
                    in_clean_ready = true;
                }
                if (s_in_clean.count >= uc) {
                    uint16_t reps = rf_unit_match(&s_in_clean, ud, uc, ul,
                                                  p->tol_pct, p->tol_us);
                    if (reps >= p->min_rep && reps > 0 &&
                        (rank < 1 || reps > key)) {
                        rank = 1;
                        key  = reps;
                    }
                }
            }
        }

        if (rank < 0)
            continue;
        if (rank > best_rank || (rank == best_rank && key > best_key)) {
            best      = m;
            best_rank = rank;
            best_key  = key;
        }
    }
    return best;
}

/* ---- the assembler. Caller holds the lock. ---- */

static void sim_run_reset(void)
{
    rf_frame_reset(&s_sim_run.run);
    s_sim_run.pieces = 0;
}

/*
 * Fold one unmatched burst into the rolling run, or start a fresh run on it.
 *
 * The silence before the new piece is MEASURED, never assumed, with the same
 * arithmetic as rf_group's gap_before(): both events are stamped at frame
 * completion, so (new ts - new airtime) - previous ts is the quiet time
 * between the pieces, give or take queue latency that affects both stamps
 * alike. The bounds are the raw machinery's own: a genuine cut gap is at
 * least the idle threshold that made the cut (DB_RAW_IDLE_US_MIN is the
 * smallest a capture channel can be armed with) and at most
 * RF_GROUP_MAX_GAP_US — the ceiling rf_group puts on any gap it would rejoin,
 * because no real pause it could have measured exceeds one RMT symbol.
 * Outside those, or past DB_RAW_MAX_PARTS pieces, or if the join overflows,
 * the old run is abandoned and the new piece starts its own — a wrong join
 * can only ever cost a match, never invent one, because whatever is
 * assembled still has to agree with a stored frame pulse for pulse.
 */
static void sim_run_offer(const rf_event_t *ev)
{
    uint32_t airtime = rf_frame_duration_us(&ev->frame);

    if (s_sim_run.pieces > 0) {
        int64_t gap = (ev->ts_us - (int64_t)airtime) - s_sim_run.last_ts_us;
        if (gap >= DB_RAW_IDLE_US_MIN && gap <= (int64_t)RF_GROUP_MAX_GAP_US &&
            s_sim_run.pieces < DB_RAW_MAX_PARTS &&
            rf_frame_join(&s_sim_scratch, &s_sim_run.run, (uint32_t)gap,
                          &ev->frame)) {
            s_sim_run.run = s_sim_scratch;
            s_sim_run.pieces++;
            s_sim_run.last_ts_us = ev->ts_us;
            return;
        }
        sim_run_reset();
    }
    s_sim_run.run        = ev->frame;
    s_sim_run.pieces     = 1;
    s_sim_run.last_ts_us = ev->ts_us;
}

/* Fire a similarity match — or absorb it into the open press window (see the
 * declaration of s_sim_press). `matched_pulses` is the count of whatever
 * waveform actually satisfied the comparison (the burst, or the assembled
 * run). Returns the id, or 0 when absorbed. */
static uint16_t sim_fire(db_signal_meta_t *m, const rf_event_t *ev,
                         uint16_t matched_pulses)
{
    if (s_sim_press.id == m->id &&
        (ev->ts_us - s_sim_press.last_us) < DB_FRAG_WINDOW_US &&
        (ev->ts_us - s_sim_press.first_us) < DB_SIM_PRESS_MAX_US) {
        s_sim_press.last_us = ev->ts_us;
        ESP_LOGD(TAG, "similarity: absorbed a repeat of signal %u '%s' into "
                      "the open press", (unsigned)m->id, m->name);
        return 0;
    }
    s_sim_press.id       = m->id;
    s_sim_press.first_us = ev->ts_us;
    s_sim_press.last_us  = ev->ts_us;

    m->last_seen_us = ev->ts_us;
    m->seen_count++;
    m->similarity_matches++;
    s_recent.matched_id = m->id;
    ESP_LOGI(TAG, "similarity-matched signal %u '%s' (%u of %u pulses, %d dBm, "
                  "similarity match %lu)",
             (unsigned)m->id, m->name, matched_pulses, m->pulse_count,
             ev->rssi_dbm, (unsigned long)m->similarity_matches);
    return m->id;
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
    /* Only after the index is final (ghosts pruned, repairs done): the cache
     * must never hold a frame the store no longer lists. */
    sim_cache_prefill();
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

    /* A fresh raw signal goes straight into the similarity cache: its first
     * real press must not pay a flash read, and we are holding the frame
     * anyway. Decoded signals never enter the cache — similarity does not
     * apply to them. */
    if (!meta->decoded_valid)
        sim_cache_put(meta->id, frame);

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

esp_err_t db_signals_add_frame_ex(const rf_frame_t *frame, const rf_decoded_t *decoded,
                                  const char *name, db_signal_origin_t origin,
                                  uint8_t unit_repeats, uint32_t unit_gap_us,
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
    /* Repeat structure travels only with an UNDECODED unit: a frame a decoder
     * claims has an identity, and bolting observed repeats onto it would be a
     * second source of truth for how that protocol transmits. */
    if (!m.decoded_valid) {
        m.unit_repeats = unit_repeats;
        m.unit_gap_us  = unit_gap_us;
    }
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

esp_err_t db_signals_add_frame(const rf_frame_t *frame, const rf_decoded_t *decoded,
                               const char *name, db_signal_origin_t origin,
                               uint16_t *id_out)
{
    /* The ordinary path: a single transmission, no repeat structure. */
    return db_signals_add_frame_ex(frame, decoded, name, origin, 0, 0, id_out);
}

void db_signals_tx_defaults(uint16_t id, uint8_t cfg_repeats, uint32_t cfg_gap_us,
                            uint8_t *repeats_out, uint32_t *gap_us_out)
{
    uint8_t  reps = cfg_repeats;
    uint32_t gap  = cfg_gap_us;

    lock();
    const db_signal_meta_t *m = find_meta(id);
    if (m && m->unit_repeats != 0) {
        /* The learned stream: as many copies as the learn capture observed,
         * separated by the measured inter-repeat gap. The stored frame ends
         * in that gap already and rf_transmit treats gap_us as a floor, so
         * the emitted period is exactly the learned one — passing the config
         * default here instead would STRETCH every gap to it and hand the
         * receiver a stream its transmitter never sends. */
        reps = m->unit_repeats;
        gap  = m->unit_gap_us;
    }
    unlock();

    if (repeats_out)
        *repeats_out = reps;
    if (gap_us_out)
        *gap_us_out = gap;
}

bool db_signals_derived_unit(uint16_t id, uint16_t *pulses_out,
                             uint16_t *repeats_out)
{
    bool have = false;

    lock();
    const db_signal_meta_t *m = find_meta(id);
    /* Only a window-signal HAS a derived unit; one learned as a unit reports
     * through its own unit_repeats metadata. sim_slot_get fills the cache
     * slot — and with it the sidecar — if the waveform is not resident yet,
     * so this answer is the same one matching would act on. */
    if (m && m->unit_repeats == 0) {
        const sim_slot_t *slot = sim_slot_get(id);
        if (slot && slot->unit.count != 0) {
            if (pulses_out)
                *pulses_out = slot->unit.count;
            if (repeats_out)
                *repeats_out = slot->unit.repeats;
            have = true;
        }
    }
    unlock();
    return have;
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

esp_err_t db_signals_set_tolerance(uint16_t id, db_sig_tolerance_t tol)
{
    if ((unsigned)tol >= DB_SIG_TOL__COUNT)
        return ESP_ERR_INVALID_ARG;

    lock();
    db_signal_meta_t *m = find_meta(id);
    if (!m) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    /* Refused rather than stored-and-ignored: a setting that silently does
     * nothing teaches the user it is safe to loosen, right up until the
     * signal is re-registered as raw and it suddenly is not. */
    if (m->decoded_valid) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t before = m->match_tolerance;
    m->match_tolerance = (uint8_t)tol;
    esp_err_t err = save_index();
    if (err != ESP_OK)
        m->match_tolerance = before;   /* RAM and flash must not disagree */
    unlock();

    if (err == ESP_OK)
        ESP_LOGI(TAG, "signal %u match tolerance set to %u", (unsigned)id,
                 (unsigned)tol);
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

    /* The recent-burst reference may name the signal we just removed — and so
     * may the similarity cache and the open press window. A stale cache entry
     * would keep MATCHING a deleted signal, which is worse than stale: it is
     * a bell the user explicitly disconnected still ringing. */
    if (s_recent.matched_id == id)
        s_recent.matched_id = 0;
    sim_cache_drop(id);
    if (s_sim_press.id == id)
        s_sim_press.id = 0;
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

    if (!frag) {
        s_recent.was_fragment = false;
        /* A new main frame becomes the reference — AND the window anchor.
         * Fragments update neither: a run of tails must be measured against
         * the real frame, not against each other, and the window must run out
         * 750 ms after the REAL frame rather than being refreshed by every
         * tail. The old code refreshed last_ts_us on fragments too, which let
         * a sustained noise storm chain the window indefinitely: one large
         * frame as reference, then every later frame under 75 % of its pulse
         * count — a legitimate doorbell press included — judged a tail for as
         * long as sub-750 ms traffic kept arriving. A suppressor must never
         * be able to latch. */
        s_recent.last_ts_us = ev->ts_us;
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

uint16_t db_signals_match_ex(const rf_event_t *ev, db_match_kind_t *kind_out)
{
    if (kind_out)
        *kind_out = DB_MATCH_NONE;
    if (!ev || ev->frame.count == 0 || s_count == 0)
        return 0;

    lock();

    bool fragment = judge_burst(ev);

    db_match_kind_t kind = DB_MATCH_NONE;
    db_signal_meta_t *hit = NULL;
    if (ev->decoded.valid) {
        hit = match_decoded(&ev->decoded);
        if (hit)
            kind = DB_MATCH_IDENTITY;
    }
    if (!hit && ev->fingerprint != 0) {
        /* Fingerprint fallback. It already encodes the pulse count and every
         * pulse as a multiple of the frame's own estimated base, so it is stable
         * across oscillator drift and cannot collide across different lengths. */
        for (int i = 0; i < s_count; i++) {
            if (s_meta[i].fingerprint == ev->fingerprint) {
                hit = &s_meta[i];
                kind = DB_MATCH_FINGERPRINT;
                break;
            }
        }
    }

    /* The similarity stage, strictly behind identity and fingerprint. The
     * burst is tried ALONE first: if it is a reception of a stored raw signal
     * in its own right, whatever the assembler holds is a different, stale
     * story and is dropped. Only a burst that explains nothing by itself is
     * offered to the assembler as a possible piece — and only an undecoded
     * one, because a decoded frame is a complete protocol word by
     * construction, not a fragment of something longer. */
    db_signal_meta_t *sim = NULL;
    bool sim_as_run = false;
    uint16_t sim_pulses = 0;
    if (!hit) {
        sim = sim_scan(&ev->frame, ev->norm.base_us);
        if (sim) {
            sim_pulses = ev->frame.count;
            sim_run_reset();
        } else if (!ev->decoded.valid) {
            sim_run_offer(ev);
            if (s_sim_run.pieces >= 2) {
                sim = sim_scan(&s_sim_run.run, 0);
                if (sim) {
                    sim_as_run = true;
                    sim_pulses = s_sim_run.run.count;
                    /* Consumed: trailing pieces of this press must start a
                     * new (and necessarily non-matching) run, not re-complete
                     * this one. */
                    sim_run_reset();
                }
            }
        }
    }

    if (fragment && !sim_as_run) {
        /* Whatever it looks like on its own, it is the tail of the press we
         * just handled. Firing here would either double-ring the chime or,
         * worse, ring the WRONG one. The one exception — sim_as_run — falls
         * through on purpose: there the "tail" is the piece that COMPLETED an
         * assembled run whose earlier pieces matched nothing, so the fire
         * below is that press's first ring, not a second one (see the
         * suppressor/assembler composition note in the file header). */
        /* WARN when the suppressed frame would have matched: that is exactly
         * the case a wedged suppressor would hide, and the one worth seeing
         * on a box with no serial console. Plain tails stay at debug. */
        if (hit)
            ESP_LOGW(TAG, "suppressed a %u-pulse tail fragment that matches "
                          "signal %u '%s' (%u-pulse reference frame %lld ms ago)",
                     ev->frame.count, (unsigned)hit->id, hit->name,
                     s_recent.ref_pulses,
                     (long long)((ev->ts_us - s_recent.last_ts_us) / 1000));
        else if (sim) {
            ESP_LOGW(TAG, "suppressed a %u-pulse tail fragment that resembles "
                          "signal %u '%s'", ev->frame.count,
                     (unsigned)sim->id, sim->name);
            /* The tail is fresh evidence of the same press: keep the press
             * window open so the NEXT straggler is absorbed too, rather than
             * firing because the window ran out mid-press. */
            if (s_sim_press.id == sim->id)
                s_sim_press.last_us = ev->ts_us;
        } else
            ESP_LOGD(TAG, "ignoring %u-pulse tail fragment after a %u-pulse frame",
                     ev->frame.count, s_recent.ref_pulses);
        unlock();
        return 0;
    }

    uint16_t id = 0;
    if (hit) {
        hit->last_seen_us = ev->ts_us;
        hit->seen_count++;
        id = hit->id;
        s_recent.matched_id = id;
        /* An identified press supersedes whatever the assembler was piecing
         * together, and its own trailing fragments must not re-complete it. */
        sim_run_reset();
        ESP_LOGI(TAG, "matched signal %u '%s' (%u repeats, %d dBm, seen %lu)",
                 (unsigned)id, hit->name, ev->repeats, ev->rssi_dbm,
                 (unsigned long)hit->seen_count);
    } else if (sim) {
        id = sim_fire(sim, ev, sim_pulses);
        kind = id ? DB_MATCH_SIMILARITY : DB_MATCH_NONE;
    }

    unlock();
    if (kind_out)
        *kind_out = id ? kind : DB_MATCH_NONE;
    return id;
}

uint16_t db_signals_match(const rf_event_t *ev)
{
    return db_signals_match_ex(ev, NULL);
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
    /* The similarity machinery is resident state like everything above: a
     * simulated reboot that kept the cache would hide exactly the prefill
     * and invalidation bugs these tests exist to catch. */
    memset(s_sim_cache, 0, sizeof(s_sim_cache));
    sim_run_reset();
    memset(&s_sim_press, 0, sizeof(s_sim_press));
}
#endif
