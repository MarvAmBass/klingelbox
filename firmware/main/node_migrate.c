/* node_migrate.c - see node_migrate.h. Frozen layouts; nothing here may drift. */
#include "node_migrate.h"

#include <string.h>

void db_node_widen_v3(db_node_t *dst, const void *src, int n)
{
    if (!dst || !src || n <= 0)
        return;

    const unsigned char *p = (const unsigned char *)src;

    for (int i = 0; i < n; i++) {
        /* Copied OUT of the buffer first. The staging buffer is a byte array
         * behind a blob header, so the record is not guaranteed to be aligned
         * for a uint32_t read in place. */
        db_node_v3_t old;
        memcpy(&old, p + (size_t)i * sizeof(db_node_v3_t), sizeof(old));

        db_node_t *d = &dst[i];
        /* Zero first so the padding — and any field a future layout adds
         * between this one and the next migration — starts from a known value
         * rather than from whatever the caller's array held. */
        memset(d, 0, sizeof(*d));

        d->id               = old.id;
        d->type             = old.type;
        d->enabled          = old.enabled;
        memcpy(d->name, old.name, sizeof(d->name));
        d->name[sizeof(d->name) - 1] = '\0';
        d->signal_id        = old.signal_id;
        d->gpio_pin         = old.gpio_pin;
        d->gpio_active_low  = old.gpio_active_low;
        d->gpio_debounce_ms = old.gpio_debounce_ms;
        d->repeats          = old.repeats;
        d->gap_us           = old.gap_us;
        d->window_ms        = old.window_ms;
        d->group_mode       = old.group_mode;
        memcpy(d->topic, old.topic, sizeof(d->topic));
        d->topic[sizeof(d->topic) - 1] = '\0';
        d->ui_x             = old.ui_x;
        d->ui_y             = old.ui_y;

        /* The one new field. TRUE, so nothing a user already has on their
         * broker disappears because they took an update. */
        d->mqtt_enabled     = true;
    }
}
