/*
 * rf_group.c - Grouping, ranking, fragment detection and rejoining.
 *
 * See rf_group.h for WHY any of this exists. Three implementation notes, each of
 * which has a failure mode that looks like something else:
 *
 * 1. NOTHING IS EVER DISCARDED. Every observation joins a group; a group of one
 *    is a perfectly valid candidate that simply has no repeat evidence yet. This
 *    file contains no threshold that can make a frame vanish, and it must stay
 *    that way — a filter here would recreate the exact bug this module replaced.
 *
 * 2. THE SORT IS INSERTION SORT, ON PURPOSE. n <= 32, the comparison is not
 *    trivially cheap, and insertion sort is stable — which matters, because the
 *    tie-break is "whichever arrived first" and a stable sort gets that for free
 *    from the build order rather than from a second comparison nobody would
 *    remember to keep consistent.
 *
 * 3. THE GAP IS MEASURED, NEVER ASSUMED. ts_us is the completion time and
 *    airtime_us the frame's own length, so the silence before a frame is
 *    (ts - airtime) - previous_ts. It is a lower bound on the truth (the queue
 *    adds latency to ts), which is the right direction to be wrong in: a gap
 *    reported slightly short rejoins a run slightly tight, whereas an invented
 *    gap could be wrong by any amount in either direction.
 */
#include "rf_group.h"

#include <string.h>

/* ---- scoring ------------------------------------------------------------- */

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

uint32_t rf_group_score(const rf_group_t *g)
{
    if (g == NULL || g->count == 0) {
        return 0;
    }

    uint32_t reps = min_u32(g->count, RF_SCORE_REPEAT_CAP);
    uint32_t score = reps * RF_SCORE_PER_REPEAT;

    if (g->decoded_valid) {
        score += RF_SCORE_DECODED;
    }
    score += (uint32_t)g->confidence * RF_SCORE_CONF_MUL;
    score += min_u32(g->pulse_count, RF_SCORE_LEN_CAP);
    return score;
}

/* ---- grouping ------------------------------------------------------------ */

/*
 * Which member of a group gets shown and replayed. Every member has the same
 * pulse count (rf_frame_similar insists on it), so length cannot discriminate:
 * what is left is a decode, then the cleanest timing, then the loudest
 * reception — in that order, because a decoded exemplar lets the UI name the
 * candidate and a clean one replays most faithfully.
 */
static bool better_exemplar(const rf_obs_t *cand, const rf_obs_t *cur)
{
    if (cand->decoded_valid != cur->decoded_valid) {
        return cand->decoded_valid;
    }
    if (cand->confidence != cur->confidence) {
        return cand->confidence > cur->confidence;
    }
    return cand->rssi_dbm > cur->rssi_dbm;
}

static bool obs_usable(const rf_obs_t *o)
{
    return o != NULL && o->frame != NULL && o->frame->count > 0;
}

int rf_group_build(const rf_obs_t *obs, int n, rf_group_t *out, int max)
{
    int ngroups = 0;

    if (obs == NULL || out == NULL || max <= 0) {
        return 0;
    }
    if (n > RF_GROUP_MAX_OBS) {
        n = RF_GROUP_MAX_OBS;
    }

    for (int i = 0; i < n; i++) {
        if (!obs_usable(&obs[i])) {
            continue;    /* an empty slot is not an observation at all */
        }

        /* Join an existing group if this is the same waveform again. */
        int hit = -1;
        for (int g = 0; g < ngroups; g++) {
            const rf_obs_t *rep = &obs[out[g].rep];
            if (rf_frame_similar(rep->frame, obs[i].frame,
                                 RF_GROUP_TOL_PCT, RF_GROUP_TOL_US)) {
                hit = g;
                break;
            }
        }

        if (hit < 0) {
            if (ngroups >= max) {
                /* More distinct waveforms than the caller can hold. Dropping the
                 * tail is the only option, and it is the right one: the caller's
                 * capacity matches the session's, so this cannot be reached with
                 * a well-formed session. */
                continue;
            }
            rf_group_t *g = &out[ngroups++];
            memset(g, 0, sizeof(*g));
            g->rep           = (uint16_t)i;
            g->count         = 1;
            g->member[0]     = (uint16_t)i;
            g->first_us      = obs[i].ts_us;
            g->last_us       = obs[i].ts_us;
            g->best_rssi_dbm = obs[i].rssi_dbm;
            g->confidence    = obs[i].confidence;
            g->decoded_valid = obs[i].decoded_valid;
            g->pulse_count   = obs[i].frame->count;
            continue;
        }

        rf_group_t *g = &out[hit];
        if (g->count < RF_GROUP_MAX_OBS) {
            g->member[g->count] = (uint16_t)i;
        }
        g->count++;
        g->last_us = obs[i].ts_us;
        if (obs[i].rssi_dbm > g->best_rssi_dbm) {
            g->best_rssi_dbm = obs[i].rssi_dbm;
        }
        if (better_exemplar(&obs[i], &obs[g->rep])) {
            g->rep           = (uint16_t)i;
            g->confidence    = obs[i].confidence;
            g->decoded_valid = obs[i].decoded_valid;
            g->pulse_count   = obs[i].frame->count;
        }
    }

    for (int g = 0; g < ngroups; g++) {
        out[g].score = rf_group_score(&out[g]);
    }

    /* Stable insertion sort, best first — see note (2). */
    for (int i = 1; i < ngroups; i++) {
        rf_group_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].score < key.score) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return ngroups;
}

/* ---- fragmentation ------------------------------------------------------- */

/* The silence before obs[i], measured rather than assumed. Clamped at zero:
 * ts_us carries queue latency, so an unlucky pair can appear to overlap. */
static uint32_t gap_before(const rf_obs_t *obs, int i)
{
    int64_t start = obs[i].ts_us - (int64_t)obs[i].airtime_us;
    int64_t gap   = start - obs[i - 1].ts_us;

    if (gap <= 0) {
        return 0;
    }
    if (gap > (int64_t)0xFFFFFFFF) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)gap;
}

/*
 * Is the boundary between obs[i-1] and obs[i] a cut rather than a pause? Both
 * conditions are required and neither is sufficient — see the header.
 */
static bool boundary_is_a_cut(const rf_obs_t *obs, int i, uint32_t idle_us, uint32_t gap)
{
    uint64_t window = (uint64_t)idle_us * RF_FRAG_GAP_FACTOR;

    if (idle_us == 0 || (uint64_t)gap > window) {
        return false;      /* the transmitter stopped; this is a real pause */
    }
    /* Identical neighbours are the transmitter repeating itself, which is
     * evidence of authenticity, not of a chopped frame. */
    return !rf_frame_similar(obs[i - 1].frame, obs[i].frame,
                             RF_GROUP_TOL_PCT, RF_GROUP_TOL_US);
}

int rf_frag_find_runs(const rf_obs_t *obs, int n, uint32_t idle_us,
                      rf_run_t *out, int max)
{
    int nruns = 0;

    if (obs == NULL || out == NULL || max <= 0) {
        return 0;
    }
    if (n > RF_GROUP_MAX_OBS) {
        n = RF_GROUP_MAX_OBS;
    }

    int i = 0;
    while (i < n && nruns < max) {
        if (!obs_usable(&obs[i])) {
            i++;
            continue;
        }

        rf_run_t run;
        memset(&run, 0, sizeof(run));
        run.member[0]    = (uint16_t)i;
        run.gap_us[0]    = 0;
        run.count        = 1;
        run.total_pulses = obs[i].frame->count;

        int j = i + 1;
        while (j < n && obs_usable(&obs[j])) {
            uint32_t gap = gap_before(obs, j);
            if (!boundary_is_a_cut(obs, j, idle_us, gap)) {
                break;
            }
            if (run.count >= RF_GROUP_MAX_OBS) {
                break;
            }
            run.gap_us[run.count] = gap;
            run.member[run.count] = (uint16_t)j;
            run.count++;
            /* One inserted gap per join, worst case — it may fold into an
             * existing LOW instead, so this is an upper bound. */
            run.total_pulses += (uint32_t)obs[j].frame->count + 1u;
            j++;
        }

        if (run.count >= 2) {
            run.joinable = (run.total_pulses <= RF_FRAME_MAX_PULSES);
            for (uint16_t k = 1; k < run.count && run.joinable; k++) {
                if (run.gap_us[k] > RF_GROUP_MAX_GAP_US) {
                    run.joinable = false;
                }
            }
            out[nruns++] = run;
            i = j;
        } else {
            i++;
        }
    }
    return nruns;
}

/* Two measured silences that are the same silence, drifted. Same two-tolerance
 * shape as rf_frame_similar, and for the same reason: gaps stretch with the
 * oscillator, and a purely relative window is too tight on a short one. */
static bool gap_similar(uint32_t a, uint32_t b)
{
    uint32_t diff  = (a > b) ? (a - b) : (b - a);
    uint32_t ref   = (a > b) ? a : b;
    uint32_t allow = (ref * RF_GROUP_TOL_PCT) / 100u;

    if (allow < RF_GROUP_TOL_US) {
        allow = RF_GROUP_TOL_US;
    }
    return diff <= allow;
}

bool rf_run_similar(const rf_obs_t *obs, const rf_run_t *a, const rf_run_t *b)
{
    if (obs == NULL || a == NULL || b == NULL) {
        return false;
    }
    if (a->count != b->count || a->count == 0) {
        return false;
    }
    for (uint16_t k = 0; k < a->count; k++) {
        const rf_obs_t *oa = &obs[a->member[k]];
        const rf_obs_t *ob = &obs[b->member[k]];
        if (!obs_usable(oa) || !obs_usable(ob)) {
            return false;
        }
        if (!rf_frame_similar(oa->frame, ob->frame,
                              RF_GROUP_TOL_PCT, RF_GROUP_TOL_US)) {
            return false;
        }
        if (k > 0 && !gap_similar(a->gap_us[k], b->gap_us[k])) {
            return false;
        }
    }
    return true;
}

/* ---- rejoining ----------------------------------------------------------- */

/* Append one duration, refusing rather than truncating. */
static bool push(rf_frame_t *f, uint32_t us)
{
    if (us > 0xFFFFu) {
        return false;
    }
    return rf_frame_push(f, (uint16_t)us);
}

bool rf_frame_join(rf_frame_t *out, const rf_frame_t *a, uint32_t gap_us,
                   const rf_frame_t *b)
{
    if (out == NULL || a == NULL || b == NULL) {
        return false;
    }
    if (a->count == 0 || b->count == 0 ||
        a->count > RF_FRAME_MAX_PULSES || b->count > RF_FRAME_MAX_PULSES) {
        return false;
    }

    rf_frame_reset(out);
    out->first_level = a->first_level;
    for (uint16_t i = 0; i < a->count; i++) {
        if (!rf_frame_push(out, a->durations_us[i])) {
            rf_frame_reset(out);
            return false;
        }
    }

    /* A frame recorded starting on a LOW opens with silence that is part of the
     * same gap, not a pulse of its own. Absorb it, or the join would claim a
     * HIGH edge that never happened. */
    uint32_t silence = gap_us;
    uint16_t bstart  = 0;
    if (b->first_level == 0) {
        silence += b->durations_us[0];
        bstart   = 1;
    }
    if (bstart >= b->count) {
        rf_frame_reset(out);
        return false;    /* b was nothing but silence */
    }

    if (rf_frame_level_at(out, (uint16_t)(out->count - 1)) == 0) {
        /* a already ends LOW: extend it rather than starting a second LOW. */
        uint32_t merged = (uint32_t)out->durations_us[out->count - 1] + silence;
        if (merged > 0xFFFFu) {
            rf_frame_reset(out);
            return false;
        }
        out->durations_us[out->count - 1] = (uint16_t)merged;
    } else if (!push(out, silence)) {
        rf_frame_reset(out);
        return false;
    }

    for (uint16_t i = bstart; i < b->count; i++) {
        if (!rf_frame_push(out, b->durations_us[i])) {
            rf_frame_reset(out);
            return false;
        }
    }
    return true;
}

uint16_t rf_run_joined_pulses(const rf_obs_t *obs, const rf_run_t *run)
{
    if (obs == NULL || run == NULL || run->count == 0) {
        return 0;
    }

    uint32_t total = 0;
    bool ends_high = false;

    for (uint16_t k = 0; k < run->count; k++) {
        const rf_obs_t *o = &obs[run->member[k]];
        if (!obs_usable(o)) {
            return 0;
        }
        if (k > 0) {
            if (ends_high) {
                total++;                        /* the gap becomes its own LOW */
            }
            if (o->frame->first_level == 0) {
                total--;                        /* its leading silence merges  */
            }
        }
        total += o->frame->count;
        ends_high = rf_frame_level_at(o->frame,
                                      (uint16_t)(o->frame->count - 1)) != 0;
    }
    return (uint16_t)(total > RF_FRAME_MAX_PULSES ? RF_FRAME_MAX_PULSES : total);
}

bool rf_frame_join_run(rf_frame_t *out, const rf_obs_t *obs, const rf_run_t *run)
{
    if (out == NULL || obs == NULL || run == NULL || run->count == 0) {
        return false;
    }

    const rf_obs_t *first = &obs[run->member[0]];
    if (!obs_usable(first)) {
        return false;
    }
    *out = *first->frame;

    /* Joining in place would alias, so each step builds into scratch and copies
     * back. 1 KB on the stack is too much for the tasks that call this, hence
     * the static — every caller is already serialized by the session lock. */
    static rf_frame_t scratch;

    for (uint16_t k = 1; k < run->count; k++) {
        const rf_obs_t *nxt = &obs[run->member[k]];
        if (!obs_usable(nxt)) {
            rf_frame_reset(out);
            return false;
        }
        if (!rf_frame_join(&scratch, out, run->gap_us[k], nxt->frame)) {
            rf_frame_reset(out);
            return false;
        }
        *out = scratch;
    }
    return true;
}
