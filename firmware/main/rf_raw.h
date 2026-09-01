/*
 * rf_raw.h - A time-boxed recording of everything the radio hears, with the
 *            normal pipeline's assumptions switched off — and the ranking that
 *            turns that recording into something a person can act on.
 *
 * THIS IS NOW THE ONLY WAY A BUTTON IS REGISTERED. There used to be a second,
 * separate "learn mode" which admitted a burst as a candidate only if it
 * repeated at least twice AND normalized to at least 65 % confidence. Both
 * numbers were measured on EV1527 remotes, so a transmitter of any other shape
 * never became a candidate at all: the screen stayed empty and the box said
 * nothing. That is the inverse of what this project promises. The gate is gone.
 * Detection is protocol-agnostic and permissive — everything the radio hands up
 * becomes a candidate — and decoding is an ANNOTATION that ranks a candidate
 * higher, never a condition for it existing. See rf_group.h for the ranking, and
 * db_raw_get_candidates() below for what a caller gets.
 *
 * WHY THIS EXISTS. The founding promise of this box is that a signal nobody can
 * decode is still capturable and replayable. The ordinary receive path does not
 * keep that promise on its own, because four filters sit in front of it and each
 * one encodes an assumption that happens to be true for EV1527-class remotes:
 *
 *   1. rf_service's RSSI squelch at -75 dBm      — "a real press is loud"
 *   2. rf_capture's min_pulses = 32              — "a real frame is long"
 *   3. rf_capture's 8 ms idle threshold          — "frames end after 8 ms of quiet"
 *   4. rf_service's burst coalescing (250 ms)    — "repeats are near-identical"
 *
 * A transmitter that violates any of them is not merely mis-decoded: it is
 * silently invisible, and the user is told nothing at all. That is the worst
 * possible failure mode for a diagnostic device. A raw session relaxes all four
 * and records the result verbatim.
 *
 * THE DIAGNOSTIC THAT MATTERS. "Nothing was received" and "something was
 * received but did not fit our assumptions" are completely different problems —
 * the first is a radio/frequency/antenna fault, the second is a threshold to
 * loosen — and only the second is fixable by trimming a capture. So this module
 * reports, separately: whether carrier energy was ever sensed at all, the peak
 * and quietest RSSI over the session, how many frames were rejected by the
 * (much lower) noise floor, how many were too SHORT even for the relaxed
 * minimum, and how many were too LONG for RF_FRAME_MAX_PULSES and therefore
 * thrown away by the capture layer before we ever saw them.
 *
 * MEMORY. RAM only, never flash: a session is scratch data for a person staring
 * at a screen, and writing 36 KB of noise to NVS would wear it out for nothing.
 * The slots are allocated when a session starts and freed when it is discarded,
 * so an idle box pays 128 bytes of pointers and nothing else. The allocation is
 * checked and refused cleanly — this heap also carries Wi-Fi buffers and an open
 * HTTP connection, and an out-of-memory crash while diagnosing a radio fault
 * would be a spectacularly unhelpful thing to do.
 *
 * WHY STOP DOES NOT FREE. Recording and inspecting are separate phases. A
 * session auto-stops (buffer full or time up) and the frames must survive that,
 * or there is nothing to trim, replay or save — which is the entire point.
 * db_raw_stop() therefore ends the recording and keeps the buffer; the memory
 * goes back on db_raw_discard(), when the next session starts, or after
 * DB_RAW_HOLD_SECS of nobody looking.
 *
 * THREADING. Written by rf_service's capture task, read by httpd workers. One
 * mutex covers everything. This module NEVER touches the radio or the capture
 * channel: the reconfiguration those need is the radio owner's job, so sessions
 * are started and stopped through rf_service_raw_start()/rf_service_raw_stop()
 * and there is no lock-ordering question to get wrong.
 */
#ifndef DB_RF_RAW_H
#define DB_RF_RAW_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "rf_capture.h"
#include "rf_decode.h"
#include "rf_frame.h"
#include "rf_group.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Slots in a session. 32 x ~1.1 KB is ~36 KB — a large but affordable bite out
 * of the ~140 KB this box has free, and it is handed straight back afterwards. */
#define DB_RAW_MAX_FRAMES 32

/* A stopped session is kept this long for inspection, then freed on its own so
 * a box nobody came back to does not hold 36 KB until the next reboot. */
#define DB_RAW_HOLD_SECS 600

/* Defaults and bounds for the relaxed filters. Every one of these is a number
 * the normal path treats as a constant; here they are all arguments.
 *
 * The RSSI floor deserves a word, because it is the one filter a session keeps.
 * Switching the squelch off entirely IS offered (DB_RAW_RSSI_OFF) but is not the
 * default, and the reason is measured rather than theoretical: with no carrier
 * the CC1101's AGC winds to full gain and hands up a continuous stream of noise
 * frames. Relaxing min_pulses to 4 and then removing the floor as well fills all
 * 32 slots with AGC hash in about three seconds — locking out the very press the
 * user is trying to see, which is the opposite of what this feature is for.
 *
 * FIELD DATA (2026-09-01, on the author's box, indoors): the noise population
 * sat at -79..-86 dBm, ~10 dB hotter than the original bench measurement of
 * -91..-97, and real presses were -24..-42 dBm. The default was -85, which on
 * that box sits INSIDE the noise population and fills every slot with hash; it
 * is now -80, which sits at the top of it. That is still 5 dB more permissive
 * than the normal path's -75 — a remote noticeably weaker than anything the box
 * would ordinarily accept still gets recorded — and it leaves 38 dB of margin
 * below the quietest real press ever measured here. A caller that knows better
 * should say so: the UI reads the live band level from GET /api/radio and sets
 * the floor a few dB above whatever the room is actually doing, which is the
 * only approach that is right on both a quiet bench and a noisy flat. Every
 * rejection is counted (dropped_floor) so a session that finds nothing can say
 * WHY rather than showing an empty list. */
#define DB_RAW_SECONDS_DEFAULT   30
#define DB_RAW_SECONDS_MIN        5
#define DB_RAW_SECONDS_MAX      300
#define DB_RAW_IDLE_US_DEFAULT 8000
#define DB_RAW_IDLE_US_MIN     1000
/* The RMT symbol duration field is 15 bits at the 1 MHz capture resolution. */
#define DB_RAW_IDLE_US_MAX    32000
#define DB_RAW_MIN_PULSES_DEFAULT 4
#define DB_RAW_MIN_PULSES_MIN     2
#define DB_RAW_MIN_PULSES_MAX    64
#define DB_RAW_RSSI_DEFAULT     (-80)
#define DB_RAW_RSSI_MIN        (-120)
#define DB_RAW_RSSI_MAX           (0)
/* Anything at or below this is "no floor at all" — nothing the chip can report
 * is quieter, so the test can never reject a frame. */
#define DB_RAW_RSSI_OFF        (-120)

typedef struct {
    uint32_t seconds;         /* hard stop, whichever comes first with capacity */
    uint32_t idle_us;         /* silence that ends a frame (the frame boundary) */
    uint16_t min_pulses;      /* shortest frame the capture layer will hand up  */
    int16_t  rssi_floor_dbm;  /* DB_RAW_RSSI_OFF disables the test entirely     */
} db_raw_cfg_t;

/* Fill *out with the defaults above. */
void db_raw_cfg_default(db_raw_cfg_t *out);

/* Everything the UI shows about one captured frame, computed once when the
 * frame arrives rather than on every poll. The decode is attempted exactly as
 * the normal path would attempt it — a raw session changes what REACHES the
 * decoders, not what they do. */
typedef struct {
    uint16_t index;          /* 1-based, stable for the life of the session */
    int64_t  ts_us;
    int16_t  rssi_dbm;
    uint16_t pulse_count;
    uint32_t airtime_us;
    uint16_t base_us;
    uint8_t  confidence;
    bool     decoded_valid;
    char     protocol[RF_PROTOCOL_NAME_MAX];
    uint32_t decoded_id;
    uint8_t  decoded_button;
    char     text[RF_DECODE_TEXT_MAX];
} db_raw_summary_t;

/* The most pieces one rejoined candidate may be assembled from. A transmission
 * cut into more than this is not a threshold that is slightly wrong, it is a
 * threshold that is wrong by an order of magnitude, and the honest answer there
 * is "raise the frame boundary and record again" rather than a stitched-up
 * Frankenstein of sixteen scraps. */
#define DB_RAW_MAX_PARTS 8

/* Runs of fragments tracked per session. */
#define DB_RAW_MAX_RUNS 16

/*
 * One ranked candidate: either a group of frames that are the same waveform
 * repeated, or a rejoined run of fragments that one transmission was cut into.
 * The two are deliberately the same type — the user's question ("is this the
 * thing that rings my bell?") is identical for both, and so is the answer
 * (transmit it and listen).
 *
 * `seen` is the evidence that drives ranking: a real remote repeats itself, and
 * counting that needs no protocol knowledge whatsoever. `decoded_valid` and
 * `confidence` only break ties — see rf_group.h for why that ordering is the
 * whole point of this rework.
 */
typedef struct {
    uint16_t id;             /* 1-based rank; stable while the frame set is */
    uint16_t seen;           /* how many times this waveform was heard      */
    bool     merged;         /* assembled from fragments rather than heard whole */
    uint8_t  part_count;     /* 1 unless merged                             */
    uint16_t part_index[DB_RAW_MAX_PARTS];  /* the frame indices it is made of */
    uint32_t part_gap_us[DB_RAW_MAX_PARTS]; /* measured silence before each part */

    uint16_t pulse_count;    /* of what would actually be transmitted */
    uint32_t airtime_us;
    uint16_t base_us;
    uint8_t  confidence;
    int16_t  best_rssi_dbm;
    int64_t  first_us;
    int64_t  last_us;
    uint32_t score;

    bool     decoded_valid;
    char     protocol[RF_PROTOCOL_NAME_MAX];
    uint32_t decoded_id;
    uint8_t  decoded_button;
    char     text[RF_DECODE_TEXT_MAX];
} db_raw_candidate_t;

typedef struct {
    bool         running;        /* recording right now */
    bool         held;           /* a finished session is still in memory */
    db_raw_cfg_t cfg;
    uint32_t     elapsed_s;
    uint32_t     remaining_s;    /* 0 once stopped */
    uint16_t     count;
    uint16_t     capacity;
    const char  *stop_reason;    /* "", "time", "full", "user", "radio" */

    /* Why frames did not make it, split by cause — see the file header. */
    uint32_t dropped_floor;      /* below rssi_floor_dbm (we saw and rejected)  */
    uint32_t dropped_short;      /* below min_pulses (capture layer rejected)   */
    uint32_t dropped_full;       /* hit RF_FRAME_MAX_PULSES, thrown away        */
    uint32_t overruns;           /* consumer too slow; frame lost               */
    uint32_t dropped_capacity;   /* arrived after every slot was taken          */
    uint32_t heard;              /* frames handed up by the capture layer       */

    /* Did the radio see anything at all? */
    bool     carrier_seen;
    bool     band_sampled;
    int16_t  peak_rssi_dbm;
    int16_t  quiet_rssi_dbm;

    /* The fragmentation verdict, which is a diagnosis and not a statistic: if
     * `frag_runs` is non-zero the frame boundary fired INSIDE a transmission and
     * chopped it up, and the fix is one number. `frag_suggest_idle_us` is what
     * that number should become — derived from the largest gap actually
     * measured, so it is a fact about this room rather than a guess. */
    uint16_t frag_runs;          /* how many transmissions were cut up      */
    uint16_t frag_frames;        /* frames involved in those cuts           */
    uint16_t frag_joined;        /* runs we could stitch back together      */
    uint32_t frag_max_gap_us;    /* the widest cut we saw                   */
    uint32_t frag_suggest_idle_us;   /* 0 when there is nothing to suggest  */

    uint16_t candidates;         /* ranked candidates currently available   */
} db_raw_state_t;

/*
 * Allocate the slots and begin recording. Returns:
 *   ESP_ERR_INVALID_STATE  a session is already running
 *   ESP_ERR_NO_MEM         the heap cannot take it (nothing is allocated)
 * Any previously held session is discarded first. `cfg` is clamped, not
 * rejected: a value outside the bounds above is a UI bug, not a user error.
 *
 * Call rf_service_raw_start() rather than this — the capture channel has to be
 * reconfigured to match, and only the radio owner may do that.
 */
esp_err_t db_raw_start(const db_raw_cfg_t *cfg);

/* End the recording, keep the frames. `reason` must be a string literal (it is
 * stored by pointer); NULL means "decide from the session state", which is what
 * the auto-stop passes — it yields "full" or "time". */
void db_raw_stop(const char *reason);

/* Free everything. Safe at any time. */
void db_raw_discard(void);

/* True while frames are being recorded. */
bool db_raw_active(void);

/* True when the session has run out of time or slots and should be stopped.
 * Checked by the capture task, which is the only context that may act on it. */
bool db_raw_expired(void);

/* True when a stopped session has sat unread for DB_RAW_HOLD_SECS. */
bool db_raw_stale(void);

/* ---- the recording side, called from rf_service's capture task ---- */

/* Offer one freshly captured frame. Applies the RSSI floor, analyses and stores
 * it, or counts the reason it was dropped. */
void db_raw_offer(const rf_frame_t *frame, int rssi_dbm);

/* Periodic band observation: is there carrier energy, and how loud is the band
 * when no frame is arriving? This is what separates "the antenna is not
 * connected" from "the thresholds are wrong". */
void db_raw_note_band(int rssi_dbm, bool carrier);

/* Latest capture-layer counters. Accumulated rather than assigned, because
 * rf_capture zeroes its statistics every time the channel is re-created — which
 * happens on every transmit. */
void db_raw_note_capture_stats(const rf_capture_stats_t *st);

/* ---- the reading side, called from httpd workers ---- */

void db_raw_get_state(db_raw_state_t *out);

/* Copy up to `max` summaries, oldest first. Returns how many were written. */
int  db_raw_get_summaries(db_raw_summary_t *out, int max);

/* One frame by its 1-based index. false when there is no such frame. */
bool db_raw_get_summary(uint16_t index, db_raw_summary_t *out);
bool db_raw_copy_frame(uint16_t index, rf_frame_t *out);

/*
 * Copy pulses [from, to) of frame `index` into *out, adjusting first_level for
 * the offset — levels alternate, so a trim starting on an odd index starts on
 * the opposite level and a frame that forgot to say so would replay inverted.
 *
 * `to` == 0 means "to the end". Bounds are clamped, so a UI cannot ask for
 * something out of range; false means the frame does not exist or the selection
 * is empty.
 */
bool db_raw_copy_slice(uint16_t index, uint16_t from, uint16_t to, rf_frame_t *out);

/* ---- ranked candidates ----
 *
 * The analysis is rebuilt only when the frame set has changed, so polling this
 * once a second costs nothing between presses. Candidate ids are positions in
 * the ranked list and are therefore stable only while no new frame arrives —
 * which is the case that matters, since a user acts on a stopped session.
 */

/* Copy up to `max` candidates, best first. Returns how many were written. */
int db_raw_get_candidates(db_raw_candidate_t *out, int max);

/* One candidate by its 1-based id. */
bool db_raw_get_candidate(uint16_t id, db_raw_candidate_t *out);

/*
 * The waveform a candidate stands for, sliced to [from, to) exactly as
 * db_raw_copy_slice() does. For a plain candidate that is one recorded frame;
 * for a merged one it is its pieces stitched back together with the silences
 * that were measured between them. Either way it is what transmit and save
 * operate on, so what the user tests is exactly what the user keeps.
 */
bool db_raw_copy_candidate(uint16_t id, uint16_t from, uint16_t to, rf_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DB_RF_RAW_H */
