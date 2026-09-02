/*
 * node_migrate.h - frozen on-flash node layouts, and the widening that brings
 * them forward. See node_graph.c for the version chain this belongs to.
 *
 * THE STORED BYTES ARE NOT THE CURRENT STRUCT. node_graph.c writes the node
 * array to NVS as a raw fixed-size record array with the record size in the blob
 * header, so the moment a field is added to db_node_t the stored records stop
 * describing themselves. Reading them back through today's db_node_t would
 * misread every field after the new one. That is why the OLD layout is frozen
 * here, byte for byte, and copied across field by field — never cast, never
 * memcpy'd wholesale, never "it is probably the same".
 *
 * NOTHING IN THIS FILE MAY EVER CHANGE. db_node_v3_t describes bytes that are
 * already sitting in somebody's flash; editing it does not migrate their box,
 * it corrupts it. A future v5 adds a db_node_v4_t beside it and leaves this one
 * exactly as it is.
 *
 * NO ESP-IDF. This is pure struct copying, which means host-test/ can prove a
 * real v3 blob widens correctly with plain gcc rather than by flashing a box and
 * hoping. node_graph.h itself is IDF-light enough to compile against two tiny
 * stubs; keeping it that way is part of what this file is for.
 */
#ifndef DB_NODE_MIGRATE_H
#define DB_NODE_MIGRATE_H

#include <stdint.h>

#include "node_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * db_node_t as it was stored under layout v1, v2 and v3.
 *
 * All three shipped with byte-identical records — v1->v2 and v2->v3 only ever
 * changed what a `type` VALUE meant, never where a field sat — so one frozen
 * struct covers every blob written before v4. v4 appended `mqtt_enabled`.
 */
typedef struct {
    uint16_t id;
    uint8_t  type;
    bool     enabled;
    char     name[DB_NODE_NAME_MAX];
    uint16_t signal_id;
    int8_t   gpio_pin;
    bool     gpio_active_low;
    uint16_t gpio_debounce_ms;
    uint8_t  repeats;
    uint32_t gap_us;
    uint32_t window_ms;
    uint8_t  group_mode;
    char     topic[DB_NODE_TOPIC_MAX];
    int16_t  ui_x, ui_y;
} db_node_v3_t;

/*
 * Widen `n` records of the v3 layout at `src` into `dst`.
 *
 * `src` is deliberately a void pointer: it points into the raw NVS staging
 * buffer, at whatever alignment the blob header left it, so the records are
 * copied out one at a time rather than accessed in place.
 *
 * Every v3 field is carried across unchanged and `mqtt_enabled` is set TRUE, so
 * a graph drawn before this firmware behaves on the next boot exactly as it did
 * on the last one. That default is the whole point of the flag being opt-OUT:
 * migrating a user's graph must never take anything off their broker.
 */
void db_node_widen_v3(db_node_t *dst, const void *src, int n);

#ifdef __cplusplus
}
#endif

#endif /* DB_NODE_MIGRATE_H */
