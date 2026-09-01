/*
 * rf_raw.c - The raw capture session. See rf_raw.h for why it exists.
 *
 * Three implementation notes worth stating, because each has a failure mode
 * that looks like something else:
 *
 * 1. THE ANALYSIS HAPPENS ONCE, WHEN THE FRAME ARRIVES. rf_normalize() plus
 *    rf_decode() over a 512-pulse frame is cheap, but the UI polls this session
 *    once a second and would otherwise re-derive 32 of them on every poll. So
 *    each slot carries its finished summary. The rf_norm_t scratch buffer is a
 *    file static, not a local: it is ~520 bytes and the capture task runs on a
 *    4 KB stack.
 *
 * 2. THE CAPTURE LAYER'S COUNTERS RESET UNDERNEATH US. rf_capture_init()
 *    memsets its whole state, and the channel is torn down and re-created on
 *    every transmit (GDO0 cannot be bound by RX and TX at once). A session that
 *    replayed a frame halfway through would therefore see dropped_full go
 *    backwards. note_capture_stats() accumulates across those resets instead of
 *    assigning, which is the only way these numbers stay honest.
 *
 * 3. THE SLOTS ARE 32 SEPARATE ALLOCATIONS, NOT ONE ARRAY. ~36 KB in one
 *    contiguous block is a lot to ask of an internal heap that is also carrying
 *    Wi-Fi buffers; 32 x ~1.1 KB is not. Fragmentation is exactly the condition
 *    a diagnostic feature has to survive, since the box has been up for days by
 *    the time anyone reaches for it.
 *
 * 4. THE ANALYSIS SCRATCH IS PART OF THE SESSION, NOT PART OF THE MODULE.
 *    Grouping 32 frames needs ~7 KB of working arrays. Holding that permanently
 *    would tax every box that never opens this screen; allocating it on demand
 *    would risk failing at the exact moment somebody is diagnosing a fault. So
 *    it is allocated WITH the slots and freed WITH them: a session's cost is one
 *    number, and an idle box pays none of it.
 *
 * 5. MERGED CANDIDATES ARE NEVER MATERIALIZED UNTIL ASKED FOR. A rejoined
 *    fragment run is a kilobyte; a session can contain sixteen of them. So a
 *    merged candidate stores only the indices of its pieces and the silences
 *    measured between them, and the actual frame is stitched together in the
 *    caller's buffer at transmit/save/inspect time. Two runs are recognised as
 *    the same transmission (i.e. "seen twice") by comparing pieces and gaps
 *    directly — see rf_run_similar() — rather than by joining them first.
 */
#include "rf_raw.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "rf_raw";

/* Refuse to start unless this much heap would remain afterwards. Wi-Fi, lwIP
 * and an open HTTP connection all draw on the same pool, and crashing the box
 * while it is being used to diagnose a radio fault helps nobody. */
#define DB_RAW_HEAP_RESERVE (48 * 1024)

/* The band is sampled at most this often — one SPI read per interval, not one
 * per captured frame, so a flood of noise frames cannot turn into an SPI flood. */
#define DB_RAW_BAND_INTERVAL_US (200 * 1000)

typedef struct {
    rf_frame_t       frame;
    db_raw_summary_t sum;
} db_raw_slot_t;

static db_raw_slot_t *s_slot[DB_RAW_MAX_FRAMES];

/* The grouping/ranking working set — see note (4). One allocation, alive for
 * exactly as long as the frames it describes. */
typedef struct {
    rf_obs_t           obs[DB_RAW_MAX_FRAMES];
    /* obs[] skips empty slots, so its indices are its own. This maps them back. */
    uint16_t           slot_of[DB_RAW_MAX_FRAMES];
    rf_group_t         group[DB_RAW_MAX_FRAMES];
    rf_run_t           run[DB_RAW_MAX_RUNS];
    db_raw_candidate_t cand[DB_RAW_MAX_FRAMES + DB_RAW_MAX_RUNS];
    int                ncand;
    /* Where a merged candidate is stitched together, so neither an httpd
     * worker's stack nor a second permanent kilobyte has to carry it. */
    rf_frame_t         joined;
    /* The frame count the analysis was built from. A session only ever appends,
     * so "unchanged" is a sufficient cache key and needs no dirty flag. */
    uint16_t           built_for;
    bool               built;
} db_raw_analysis_t;

static db_raw_analysis_t *s_an;

static struct {
    bool         running;
    bool         held;
    db_raw_cfg_t cfg;
    int64_t      started_us;
    int64_t      stopped_us;
    int64_t      last_read_us;
    int64_t      last_band_us;
    uint16_t     count;
    uint16_t     next_index;
    const char  *stop_reason;

    uint32_t dropped_floor;
    uint32_t dropped_capacity;
    bool     carrier_seen;
    bool     band_sampled;
    int16_t  peak_rssi_dbm;
    int16_t  quiet_rssi_dbm;

    /* Fragmentation verdict, recomputed with the analysis. */
    uint16_t frag_runs;
    uint16_t frag_frames;
    uint16_t frag_joined;
    uint32_t frag_max_gap_us;

    /* Accumulated capture-layer counters — see note (2) in the file header. */
    rf_capture_stats_t acc;
    rf_capture_stats_t last;
} s;

static SemaphoreHandle_t s_lock;

/* ~520 bytes of scratch; see note (1). Only touched under the lock. */
static rf_norm_t s_norm;

static void lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* ---- configuration ------------------------------------------------------- */

void db_raw_cfg_default(db_raw_cfg_t *out)
{
    if (out == NULL) {
        return;
    }
    out->seconds        = DB_RAW_SECONDS_DEFAULT;
    out->idle_us        = DB_RAW_IDLE_US_DEFAULT;
    out->min_pulses     = DB_RAW_MIN_PULSES_DEFAULT;
    out->rssi_floor_dbm = DB_RAW_RSSI_DEFAULT;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void clamp_cfg(db_raw_cfg_t *c)
{
    c->seconds    = clamp_u32(c->seconds, DB_RAW_SECONDS_MIN, DB_RAW_SECONDS_MAX);
    c->idle_us    = clamp_u32(c->idle_us, DB_RAW_IDLE_US_MIN, DB_RAW_IDLE_US_MAX);
    c->min_pulses = (uint16_t)clamp_u32(c->min_pulses, DB_RAW_MIN_PULSES_MIN,
                                        DB_RAW_MIN_PULSES_MAX);
    if (c->rssi_floor_dbm < DB_RAW_RSSI_MIN) c->rssi_floor_dbm = DB_RAW_RSSI_MIN;
    if (c->rssi_floor_dbm > DB_RAW_RSSI_MAX) c->rssi_floor_dbm = DB_RAW_RSSI_MAX;
}

/* ---- allocation ---------------------------------------------------------- */

/* Caller holds the lock. */
static void free_slots(void)
{
    for (int i = 0; i < DB_RAW_MAX_FRAMES; i++) {
        if (s_slot[i] != NULL) {
            heap_caps_free(s_slot[i]);
            s_slot[i] = NULL;
        }
    }
    if (s_an != NULL) {
        heap_caps_free(s_an);
        s_an = NULL;
    }
}

/* Caller holds the lock. All-or-nothing: a partial allocation is rolled back so
 * a session never runs with fewer slots than it advertises. */
static esp_err_t alloc_slots(void)
{
    for (int i = 0; i < DB_RAW_MAX_FRAMES; i++) {
        s_slot[i] = heap_caps_malloc(sizeof(db_raw_slot_t), MALLOC_CAP_8BIT);
        if (s_slot[i] == NULL) {
            ESP_LOGE(TAG, "out of memory at slot %d of %d (%u bytes each)",
                     i, DB_RAW_MAX_FRAMES, (unsigned)sizeof(db_raw_slot_t));
            free_slots();
            return ESP_ERR_NO_MEM;
        }
    }
    s_an = heap_caps_calloc(1, sizeof(*s_an), MALLOC_CAP_8BIT);
    if (s_an == NULL) {
        ESP_LOGE(TAG, "out of memory for the %u-byte analysis scratch",
                 (unsigned)sizeof(*s_an));
        free_slots();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---- session lifecycle --------------------------------------------------- */

/* Caller holds the lock. */
static void reset_session(void)
{
    s.running          = false;
    s.held             = false;
    s.count            = 0;
    s.next_index       = 0;
    s.stop_reason      = "";
    s.dropped_floor    = 0;
    s.dropped_capacity = 0;
    s.carrier_seen     = false;
    s.band_sampled     = false;
    s.peak_rssi_dbm    = 0;
    s.quiet_rssi_dbm   = 0;
    s.frag_runs        = 0;
    s.frag_frames      = 0;
    s.frag_joined      = 0;
    s.frag_max_gap_us  = 0;
    memset(&s.acc, 0, sizeof(s.acc));
    memset(&s.last, 0, sizeof(s.last));
    if (s_an != NULL) {
        s_an->built     = false;
        s_an->ncand     = 0;
        s_an->built_for = 0;
    }
}

esp_err_t db_raw_start(const db_raw_cfg_t *cfg)
{
    db_raw_cfg_t want;

    if (cfg != NULL) {
        want = *cfg;
    } else {
        db_raw_cfg_default(&want);
    }
    clamp_cfg(&want);

    lock();
    if (s.running) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    /* A held session is scratch data; the user asking for a new one has said
     * what they think of it. Freeing first also means the heap check below is
     * asked the question that actually matters. */
    free_slots();
    reset_session();

    size_t need = (size_t)DB_RAW_MAX_FRAMES * sizeof(db_raw_slot_t) +
                  sizeof(db_raw_analysis_t);
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_now < need + DB_RAW_HEAP_RESERVE) {
        ESP_LOGE(TAG, "refusing to start: %u bytes free, %u needed plus a %u reserve",
                 (unsigned)free_now, (unsigned)need, (unsigned)DB_RAW_HEAP_RESERVE);
        unlock();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = alloc_slots();
    if (err != ESP_OK) {
        unlock();
        return err;
    }

    s.cfg          = want;
    s.running      = true;
    s.held         = true;
    s.started_us   = esp_timer_get_time();
    s.stopped_us   = 0;
    s.last_read_us = s.started_us;
    s.last_band_us = 0;
    unlock();

    ESP_LOGI(TAG, "session started: %" PRIu32 " s, idle %" PRIu32 " us, "
                  "min %u pulses, floor %d dBm, %d slots (%u bytes)",
             want.seconds, want.idle_us, (unsigned)want.min_pulses,
             (int)want.rssi_floor_dbm, DB_RAW_MAX_FRAMES, (unsigned)need);
    return ESP_OK;
}

void db_raw_stop(const char *reason)
{
    lock();
    if (s.running) {
        s.running     = false;
        s.stopped_us  = esp_timer_get_time();
        /* NULL means "you worked it out for yourself" — which only the auto-stop
         * does, and there are exactly two ways for that to happen. Telling them
         * apart matters: a session that filled in half a second is a squelch
         * problem, one that ran out of time is not. */
        s.stop_reason = reason ? reason
                               : (s.count >= DB_RAW_MAX_FRAMES ? "full" : "time");
        ESP_LOGI(TAG, "session stopped (%s): %u frames, %" PRIu32 " below floor, "
                      "%" PRIu32 " too long, %" PRIu32 " too short",
                 s.stop_reason, (unsigned)s.count, s.dropped_floor,
                 s.acc.dropped_full + s.last.dropped_full,
                 s.acc.dropped_short + s.last.dropped_short);
    }
    unlock();
}

void db_raw_discard(void)
{
    lock();
    free_slots();
    reset_session();
    unlock();
}

bool db_raw_active(void)
{
    bool a;
    lock();
    a = s.running;
    unlock();
    return a;
}

bool db_raw_expired(void)
{
    bool done;
    lock();
    done = s.running &&
           (s.count >= DB_RAW_MAX_FRAMES ||
            (esp_timer_get_time() - s.started_us) >= (int64_t)s.cfg.seconds * 1000000);
    unlock();
    return done;
}

bool db_raw_stale(void)
{
    bool old;
    lock();
    old = s.held && !s.running &&
          (esp_timer_get_time() - s.last_read_us) > (int64_t)DB_RAW_HOLD_SECS * 1000000;
    unlock();
    return old;
}

/* ---- recording ----------------------------------------------------------- */

void db_raw_offer(const rf_frame_t *frame, int rssi_dbm)
{
    if (frame == NULL || frame->count == 0) {
        return;
    }

    lock();
    if (!s.running) {
        unlock();
        return;
    }

    /* The floor is the ONE filter a raw session keeps, and it is 15 dB more
     * permissive than the normal path's. Switching it off entirely is offered in
     * the UI; it is not the default, because AGC hash fills 32 slots in under a
     * second. Either way the rejection is counted, so "the band was busy but
     * everything was quiet" is a visible answer rather than an empty list. */
    if (s.cfg.rssi_floor_dbm > DB_RAW_RSSI_OFF && rssi_dbm < s.cfg.rssi_floor_dbm) {
        s.dropped_floor++;
        unlock();
        return;
    }

    if (s.count >= DB_RAW_MAX_FRAMES) {
        s.dropped_capacity++;
        unlock();
        return;
    }

    db_raw_slot_t *slot = s_slot[s.count];
    if (slot == NULL) {          /* cannot happen; refuse rather than fault */
        unlock();
        return;
    }

    slot->frame = *frame;

    rf_decoded_t decoded;
    rf_normalize(&slot->frame, &s_norm);
    bool ok = rf_decode(&slot->frame, &s_norm, &decoded);

    db_raw_summary_t *sum = &slot->sum;
    memset(sum, 0, sizeof(*sum));
    sum->index       = ++s.next_index;
    sum->ts_us       = esp_timer_get_time();
    sum->rssi_dbm    = (int16_t)rssi_dbm;
    sum->pulse_count = slot->frame.count;
    sum->airtime_us  = rf_frame_duration_us(&slot->frame);
    sum->base_us     = s_norm.base_us;
    sum->confidence  = s_norm.confidence;
    if (ok && decoded.valid) {
        sum->decoded_valid  = true;
        snprintf(sum->protocol, sizeof(sum->protocol), "%s", decoded.protocol);
        sum->decoded_id     = decoded.id;
        sum->decoded_button = decoded.button;
        snprintf(sum->text, sizeof(sum->text), "%s", decoded.text);
    }
    s.count++;
    unlock();
}

void db_raw_note_band(int rssi_dbm, bool carrier)
{
    int64_t now = esp_timer_get_time();

    lock();
    if (!s.running || (s.last_band_us && (now - s.last_band_us) < DB_RAW_BAND_INTERVAL_US)) {
        unlock();
        return;
    }
    s.last_band_us = now;
    if (carrier) {
        s.carrier_seen = true;
    }
    if (!s.band_sampled) {
        s.band_sampled   = true;
        s.peak_rssi_dbm  = (int16_t)rssi_dbm;
        s.quiet_rssi_dbm = (int16_t)rssi_dbm;
    } else {
        if (rssi_dbm > s.peak_rssi_dbm)  s.peak_rssi_dbm  = (int16_t)rssi_dbm;
        if (rssi_dbm < s.quiet_rssi_dbm) s.quiet_rssi_dbm = (int16_t)rssi_dbm;
    }
    unlock();
}

/* One counter's worth of "accumulate across a reset" — see note (2). */
static void accumulate(uint32_t *acc, uint32_t *last, uint32_t now)
{
    if (now < *last) {
        *acc += *last;      /* the channel was re-created; bank what it had */
    }
    *last = now;
}

void db_raw_note_capture_stats(const rf_capture_stats_t *st)
{
    if (st == NULL) {
        return;
    }
    lock();
    if (s.running) {
        accumulate(&s.acc.frames,        &s.last.frames,        st->frames);
        accumulate(&s.acc.dropped_short, &s.last.dropped_short, st->dropped_short);
        accumulate(&s.acc.dropped_full,  &s.last.dropped_full,  st->dropped_full);
        accumulate(&s.acc.overruns,      &s.last.overruns,      st->overruns);
    }
    unlock();
}

/* ---- the analysis: grouping, ranking, fragment detection ------------------
 *
 * All of the actual thinking is in rf_group.c, which is pure and host-tested.
 * What lives here is only the translation between this module's slots and that
 * module's observations, plus the caching that keeps a 1 Hz poll free.
 */

/* Defined with the other readers below; needed here too. Caller holds the lock. */
static const db_raw_slot_t *find(uint16_t index);

/* Caller holds the lock. Fills the candidate table from the current slots. */
static void build_analysis(void)
{
    if (s_an == NULL) {
        return;
    }
    if (s_an->built && s_an->built_for == s.count) {
        return;                 /* nothing has arrived since we last looked */
    }

    s_an->ncand     = 0;
    s_an->built     = true;
    s_an->built_for = s.count;
    s.frag_runs       = 0;
    s.frag_frames     = 0;
    s.frag_joined     = 0;
    s.frag_max_gap_us = 0;

    int n = 0;
    for (int i = 0; i < s.count && i < DB_RAW_MAX_FRAMES; i++) {
        if (s_slot[i] == NULL) {
            continue;
        }
        const db_raw_summary_t *sum = &s_slot[i]->sum;
        s_an->slot_of[n] = (uint16_t)i;
        rf_obs_t *o = &s_an->obs[n++];
        o->frame         = &s_slot[i]->frame;
        o->index         = sum->index;
        o->ts_us         = sum->ts_us;
        o->airtime_us    = sum->airtime_us;
        o->rssi_dbm      = sum->rssi_dbm;
        o->confidence    = sum->confidence;
        o->decoded_valid = sum->decoded_valid;
    }
    if (n == 0) {
        return;
    }

    /* --- repeated waveforms, ranked --- */
    int ng = rf_group_build(s_an->obs, n, s_an->group, DB_RAW_MAX_FRAMES);
    for (int g = 0; g < ng && s_an->ncand < (int)(sizeof(s_an->cand) / sizeof(s_an->cand[0])); g++) {
        const rf_group_t *gr = &s_an->group[g];
        const rf_obs_t   *rep = &s_an->obs[gr->rep];
        const db_raw_summary_t *sum = &s_slot[s_an->slot_of[gr->rep]]->sum;

        db_raw_candidate_t *c = &s_an->cand[s_an->ncand++];
        memset(c, 0, sizeof(*c));
        c->seen          = gr->count;
        c->merged        = false;
        c->part_count    = 1;
        c->part_index[0] = rep->index;
        c->pulse_count   = gr->pulse_count;
        c->airtime_us    = rep->airtime_us;
        c->base_us       = sum->base_us;
        c->confidence    = gr->confidence;
        c->best_rssi_dbm = gr->best_rssi_dbm;
        c->first_us      = gr->first_us;
        c->last_us       = gr->last_us;
        c->score         = gr->score;
        c->decoded_valid = gr->decoded_valid;
        if (gr->decoded_valid) {
            memcpy(c->protocol, sum->protocol, sizeof(c->protocol));
            memcpy(c->text, sum->text, sizeof(c->text));
            c->decoded_id     = sum->decoded_id;
            c->decoded_button = sum->decoded_button;
        }
    }

    /* --- transmissions the frame boundary cut into pieces --- */
    int nr = rf_frag_find_runs(s_an->obs, n, s.cfg.idle_us, s_an->run, DB_RAW_MAX_RUNS);
    for (int r = 0; r < nr; r++) {
        s.frag_runs++;
        s.frag_frames = (uint16_t)(s.frag_frames + s_an->run[r].count);
        for (uint16_t k = 1; k < s_an->run[r].count; k++) {
            if (s_an->run[r].gap_us[k] > s.frag_max_gap_us) {
                s.frag_max_gap_us = s_an->run[r].gap_us[k];
            }
        }
    }

    /* Runs that are the same transmission cut the same way are the SAME
     * candidate seen more than once — which is the repeat evidence that makes a
     * rejoined candidate rank above the scraps it was built from. */
    bool taken[DB_RAW_MAX_RUNS];
    memset(taken, 0, sizeof(taken));
    for (int r = 0; r < nr; r++) {
        if (taken[r] || !s_an->run[r].joinable ||
            s_an->run[r].count > DB_RAW_MAX_PARTS) {
            continue;
        }
        taken[r] = true;

        uint16_t seen = 1;
        int64_t  last = s_an->obs[s_an->run[r].member[s_an->run[r].count - 1]].ts_us;
        for (int q = r + 1; q < nr; q++) {
            if (!taken[q] && rf_run_similar(s_an->obs, &s_an->run[r], &s_an->run[q])) {
                taken[q] = true;
                seen++;
                last = s_an->obs[s_an->run[q].member[s_an->run[q].count - 1]].ts_us;
            }
        }
        s.frag_joined++;

        if (s_an->ncand >= (int)(sizeof(s_an->cand) / sizeof(s_an->cand[0]))) {
            continue;
        }

        const rf_run_t *run = &s_an->run[r];
        db_raw_candidate_t *c = &s_an->cand[s_an->ncand++];
        memset(c, 0, sizeof(*c));
        c->seen       = seen;
        c->merged     = true;
        c->part_count = (uint8_t)run->count;
        c->first_us   = s_an->obs[run->member[0]].ts_us;
        c->last_us    = last;

        /* The exemplar's analysis stands for the merged whole: it is the piece
         * a decoder was most likely to have claimed, and base_us is a property
         * of the transmitter rather than of where the cut fell. */
        /* pulse_count is what will ACTUALLY be transmitted, which is NOT the
         * sum of the pieces: rf_run_joined_pulses() knows how the join folds
         * gaps into trailing LOWs. Being one or two out here is not cosmetic —
         * it is the upper bound of the trim handles the user then drags.
         * Airtime, by contrast, IS the sum: folding moves time about, it never
         * creates or destroys any. */
        c->pulse_count = rf_run_joined_pulses(s_an->obs, run);

        uint16_t best = 0;
        for (uint16_t k = 0; k < run->count && k < DB_RAW_MAX_PARTS; k++) {
            const rf_obs_t *o = &s_an->obs[run->member[k]];
            c->part_index[k]  = o->index;
            c->part_gap_us[k] = run->gap_us[k];
            c->airtime_us    += o->airtime_us + run->gap_us[k];
            if (k == 0 || o->rssi_dbm > c->best_rssi_dbm) {
                c->best_rssi_dbm = o->rssi_dbm;
            }
            const rf_obs_t *b = &s_an->obs[run->member[best]];
            bool better = (o->decoded_valid && !b->decoded_valid) ||
                          (o->decoded_valid == b->decoded_valid &&
                           o->confidence > b->confidence);
            if (better) {
                best = k;
            }
        }

        const db_raw_summary_t *bs = &s_slot[s_an->slot_of[run->member[best]]]->sum;
        c->base_us       = bs->base_us;
        c->confidence    = bs->confidence;
        c->decoded_valid = bs->decoded_valid;
        if (bs->decoded_valid) {
            memcpy(c->protocol, bs->protocol, sizeof(c->protocol));
            memcpy(c->text, bs->text, sizeof(c->text));
            c->decoded_id     = bs->decoded_id;
            c->decoded_button = bs->decoded_button;
        }

        /* Scored on the same scale as an ordinary group, so the two kinds sort
         * against each other honestly. A rejoined whole legitimately outranks
         * its own fragments: same repeat count, more pulses. */
        rf_group_t as_group;
        memset(&as_group, 0, sizeof(as_group));
        as_group.count         = c->seen;
        as_group.confidence    = c->confidence;
        as_group.decoded_valid = c->decoded_valid;
        as_group.pulse_count   = c->pulse_count;
        c->score = rf_group_score(&as_group);
    }

    /* One ranked list, both kinds in it. Insertion sort: n is tiny and stability
     * keeps the group/run construction order as the tie-break. */
    for (int i = 1; i < s_an->ncand; i++) {
        db_raw_candidate_t key = s_an->cand[i];
        int j = i - 1;
        while (j >= 0 && s_an->cand[j].score < key.score) {
            s_an->cand[j + 1] = s_an->cand[j];
            j--;
        }
        s_an->cand[j + 1] = key;
    }
    for (int i = 0; i < s_an->ncand; i++) {
        s_an->cand[i].id = (uint16_t)(i + 1);
    }
}

/*
 * Caller holds the lock. Copies [from, to) of *src into *out, fixing up
 * first_level for the offset — levels alternate, so a slice beginning on an odd
 * index begins on the opposite level and a frame that forgot to say so would
 * replay inverted.
 *
 * memmove, not memcpy, and no temporary: *out is allowed to BE *src (which is
 * how a merged candidate is sliced in place), and a 1 KB stack temporary in an
 * httpd worker is not something this module is willing to spend.
 */
static bool slice_into(const rf_frame_t *src, uint16_t from, uint16_t to, rf_frame_t *out)
{
    uint16_t count = src->count;

    if (count > RF_FRAME_MAX_PULSES) {
        count = RF_FRAME_MAX_PULSES;    /* defensive */
    }
    if (to == 0 || to > count) {
        to = count;
    }
    if (from >= to) {
        return false;
    }

    uint16_t n  = (uint16_t)(to - from);
    uint8_t  fl = (uint8_t)(src->first_level ^ (from & 1u));

    memmove(out->durations_us, &src->durations_us[from],
            (size_t)n * sizeof(out->durations_us[0]));
    out->first_level = fl;
    out->count       = n;
    return true;
}

int db_raw_get_candidates(db_raw_candidate_t *out, int max)
{
    int n = 0;

    if (out == NULL || max <= 0) {
        return 0;
    }
    lock();
    s.last_read_us = esp_timer_get_time();
    build_analysis();
    if (s_an != NULL) {
        for (int i = 0; i < s_an->ncand && n < max; i++) {
            out[n++] = s_an->cand[i];
        }
    }
    unlock();
    return n;
}

bool db_raw_get_candidate(uint16_t id, db_raw_candidate_t *out)
{
    bool ok = false;

    if (out == NULL) {
        return false;
    }
    lock();
    s.last_read_us = esp_timer_get_time();
    build_analysis();
    if (s_an != NULL && id > 0 && (int)id <= s_an->ncand) {
        *out = s_an->cand[id - 1];
        ok = true;
    }
    unlock();
    return ok;
}

bool db_raw_copy_candidate(uint16_t id, uint16_t from, uint16_t to, rf_frame_t *out)
{
    bool ok = false;

    if (out == NULL) {
        return false;
    }
    lock();
    s.last_read_us = esp_timer_get_time();
    build_analysis();

    if (s_an != NULL && id > 0 && (int)id <= s_an->ncand) {
        const db_raw_candidate_t *c = &s_an->cand[id - 1];

        if (!c->merged) {
            const db_raw_slot_t *slot = find(c->part_index[0]);
            if (slot != NULL) {
                ok = slice_into(&slot->frame, from, to, out);
            }
        } else {
            /* Rebuild the run in obs terms and stitch it. rf_frame_join_run()
             * uses a static scratch and we hold the lock, which is the
             * serialization it documents as its requirement. */
            rf_run_t run;
            rf_obs_t obs[DB_RAW_MAX_PARTS];
            memset(&run, 0, sizeof(run));
            bool good = true;

            for (uint8_t k = 0; k < c->part_count && k < DB_RAW_MAX_PARTS; k++) {
                const db_raw_slot_t *slot = find(c->part_index[k]);
                if (slot == NULL) {
                    good = false;
                    break;
                }
                obs[k].frame      = &slot->frame;
                obs[k].index      = c->part_index[k];
                obs[k].ts_us      = slot->sum.ts_us;
                obs[k].airtime_us = slot->sum.airtime_us;
                run.member[k]     = k;
                run.gap_us[k]     = c->part_gap_us[k];
                run.count++;
            }
            if (good && run.count >= 2 &&
                rf_frame_join_run(&s_an->joined, obs, &run)) {
                ok = slice_into(&s_an->joined, from, to, out);
            }
        }
    }
    unlock();
    return ok;
}

/* ---- reading ------------------------------------------------------------- */

void db_raw_get_state(db_raw_state_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    lock();
    int64_t now = esp_timer_get_time();
    s.last_read_us = now;

    out->running     = s.running;
    out->held        = s.held;
    out->cfg         = s.cfg;
    out->count       = s.count;
    out->capacity    = DB_RAW_MAX_FRAMES;
    out->stop_reason = s.stop_reason ? s.stop_reason : "";

    if (s.held) {
        int64_t end = s.running ? now : s.stopped_us;
        int64_t el  = (end - s.started_us) / 1000000;
        out->elapsed_s = (uint32_t)(el < 0 ? 0 : el);
        if (s.running) {
            int64_t left = (int64_t)s.cfg.seconds - el;
            out->remaining_s = (uint32_t)(left < 0 ? 0 : left);
        }
    }

    out->dropped_floor    = s.dropped_floor;
    out->dropped_capacity = s.dropped_capacity;
    out->dropped_short    = s.acc.dropped_short + s.last.dropped_short;
    out->dropped_full     = s.acc.dropped_full  + s.last.dropped_full;
    out->overruns         = s.acc.overruns      + s.last.overruns;
    out->heard            = s.acc.frames        + s.last.frames;

    out->carrier_seen   = s.carrier_seen;
    out->band_sampled   = s.band_sampled;
    out->peak_rssi_dbm  = s.peak_rssi_dbm;
    out->quiet_rssi_dbm = s.quiet_rssi_dbm;

    /* The fragmentation verdict falls out of the same analysis the candidate
     * list comes from, so build it here rather than making the caller ask
     * twice. Cached on the frame count: between presses this is free. */
    build_analysis();
    out->candidates      = (uint16_t)(s_an != NULL ? s_an->ncand : 0);
    out->frag_runs       = s.frag_runs;
    out->frag_frames     = s.frag_frames;
    out->frag_joined     = s.frag_joined;
    out->frag_max_gap_us = s.frag_max_gap_us;

    /* What the frame boundary SHOULD be: comfortably past the widest cut we
     * actually measured, rounded to something a human would type, and never
     * below where it already is. Derived from this room rather than guessed. */
    out->frag_suggest_idle_us = 0;
    if (s.frag_runs > 0) {
        uint32_t want = s.frag_max_gap_us * 3u;
        if (want < s.cfg.idle_us * 2u) {
            want = s.cfg.idle_us * 2u;
        }
        want = ((want + 999u) / 1000u) * 1000u;       /* whole milliseconds */
        if (want > DB_RAW_IDLE_US_MAX) {
            want = DB_RAW_IDLE_US_MAX;
        }
        if (want > s.cfg.idle_us) {
            out->frag_suggest_idle_us = want;
        }
    }
    unlock();
}

int db_raw_get_summaries(db_raw_summary_t *out, int max)
{
    int n = 0;

    if (out == NULL || max <= 0) {
        return 0;
    }
    lock();
    s.last_read_us = esp_timer_get_time();
    for (int i = 0; i < s.count && n < max; i++) {
        if (s_slot[i] != NULL) {
            out[n++] = s_slot[i]->sum;
        }
    }
    unlock();
    return n;
}

/* Caller holds the lock. Indices are 1-based and assigned in arrival order, so
 * the slot is simply index-1 — but the check is written against the stored
 * index so it stays correct if the buffer ever becomes a ring. */
static const db_raw_slot_t *find(uint16_t index)
{
    if (index == 0 || index > s.count) {
        return NULL;
    }
    const db_raw_slot_t *slot = s_slot[index - 1];
    if (slot == NULL || slot->sum.index != index) {
        return NULL;
    }
    return slot;
}

bool db_raw_get_summary(uint16_t index, db_raw_summary_t *out)
{
    bool ok = false;

    if (out == NULL) {
        return false;
    }
    lock();
    s.last_read_us = esp_timer_get_time();
    const db_raw_slot_t *slot = find(index);
    if (slot != NULL) {
        *out = slot->sum;
        ok = true;
    }
    unlock();
    return ok;
}

bool db_raw_copy_frame(uint16_t index, rf_frame_t *out)
{
    return db_raw_copy_slice(index, 0, 0, out);
}

bool db_raw_copy_slice(uint16_t index, uint16_t from, uint16_t to, rf_frame_t *out)
{
    bool ok = false;

    if (out == NULL) {
        return false;
    }
    lock();
    s.last_read_us = esp_timer_get_time();
    const db_raw_slot_t *slot = find(index);
    if (slot != NULL) {
        ok = slice_into(&slot->frame, from, to, out);
    }
    unlock();
    return ok;
}
