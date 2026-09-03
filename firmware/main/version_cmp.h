/*
 * version_cmp.h - release-tag parsing and ordering for the update checker.
 *
 * Pure string logic with a subtle ordering contract, so it lives header-only
 * and IDF-free: update_check.c includes it on the device, and
 * host-test/test_version_cmp.c compiles the exact same functions with plain
 * gcc — no second entry in any source list, and no way for the two to drift.
 *
 * TWO ORDERING RULES ARE LOAD-BEARING:
 *
 *   1. COMPONENTS COMPARE NUMERICALLY, NEVER WITH strcmp() ON THE WHOLE TAG:
 *      "0.10.0" sorts before "0.9.0" as text, which would hide every release
 *      after the ninth minor.
 *
 *   2. A PRERELEASE SORTS BEFORE ITS FINAL RELEASE (semver rule 11):
 *      "0.7.0-rc1" < "0.7.0". Both directions matter — a box on the final must
 *      never be nagged to "upgrade" to its own release candidate, and a box
 *      running an rc (installed via the manual URL path) must see the final as
 *      newer, not as equal and therefore invisible.
 *
 * Two prereleases of the same triple compare by plain strcmp() of the suffix.
 * That is a documented approximation of semver's per-identifier rules: it
 * mis-orders "rc9" vs "rc10", but the worst that can cost is a missed or
 * spurious offer BETWEEN two prereleases of one release — it can never invert
 * a final against anything.
 */
#ifndef DB_VERSION_CMP_H
#define DB_VERSION_CMP_H

#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned    num[3];  /* major.minor.patch; absent components are 0        */
    const char *pre;     /* prerelease identifiers (the text after '-'), or ""
                            for a final release. Points INTO the parsed string,
                            so it is valid only as long as that string is.     */
} db_semver_t;

/*
 * Parse "v0.10.0", "0.10", "v1.2.3-rc2", "1.2.3+build7" into *out.
 *
 * Tolerant on purpose: the leading "v" is conventional but not guaranteed, a
 * tag may carry only two components, and a pre-release/build suffix must not
 * make the whole tag unreadable. Returns false only when there is no leading
 * number at all (e.g. a tag named "nightly"), which the caller reports as an
 * unreadable release rather than silently treating as "no update".
 */
static inline bool db_ver_parse(const char *s, db_semver_t *out)
{
    out->num[0] = out->num[1] = out->num[2] = 0;
    out->pre = "";
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 'v' || *s == 'V') s++;
    if (*s < '0' || *s > '9') return false;

    for (int i = 0; i < 3; i++) {
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (digits < 6) v = v * 10 + (unsigned)(*s - '0');   /* clamp absurd input */
            digits++;
            s++;
        }
        out->num[i] = v;
        if (*s != '.') break;            /* '-rc1', '+meta' or end of string */
        s++;
        if (*s < '0' || *s > '9') break; /* "1.2." — take what we have */
    }
    /* '-' right after the numbers marks a prerelease; '+build' is ordering-
     * irrelevant metadata (semver rule 10) and anything else is noise. */
    if (*s == '-' && s[1]) out->pre = s + 1;
    return true;
}

/* -1 / 0 / +1 for a < b, a == b, a > b, under the two rules above. */
static inline int db_ver_cmp(const db_semver_t *a, const db_semver_t *b)
{
    for (int i = 0; i < 3; i++) {
        if (a->num[i] < b->num[i]) return -1;
        if (a->num[i] > b->num[i]) return 1;
    }
    if (a->pre[0] && !b->pre[0]) return -1;   /* prerelease < its final */
    if (!a->pre[0] && b->pre[0]) return 1;
    if (a->pre[0]) {                          /* both prereleases: see header */
        int c = strcmp(a->pre, b->pre);
        return (c > 0) - (c < 0);
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* DB_VERSION_CMP_H */
