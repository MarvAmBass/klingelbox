/*
 * test_version_cmp.c - Host-compiled tests for release-tag ordering
 * (main/version_cmp.h).
 *
 * Pure string logic, but the ordering contract has already been wrong once in a
 * way no on-device test would catch: a prerelease comparing EQUAL to its final
 * release means a box running "v0.7.0-rc1" is told it is up to date forever,
 * silently, and only for the handful of users who ever ran an rc. The rules
 * pinned down here are the two the header declares load-bearing — numeric
 * component compare, and prerelease-before-final — plus the documented strcmp
 * approximation between two prereleases.
 *
 * Build and run:  make test
 */
#include <stdio.h>
#include <string.h>

#include "version_cmp.h"

/* ---- micro test harness (same shape as test_rf_decode.c) ----------------- */

static int g_pass;
static int g_fail;
static const char *g_case = "";

#define CASE(name) do { g_case = (name); } while (0)

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            g_pass++;                                                           \
        } else {                                                                \
            g_fail++;                                                           \
            printf("  FAIL [%s] %s:%d: ", g_case, __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

/* ---- helpers ------------------------------------------------------------- */

/* Parse both tags (must succeed) and return the comparison. */
static int cmp(const char *a, const char *b)
{
    db_semver_t va, vb;
    CHECK(db_ver_parse(a, &va), "\"%s\" did not parse", a);
    CHECK(db_ver_parse(b, &vb), "\"%s\" did not parse", b);
    return db_ver_cmp(&va, &vb);
}

/* Every ordering claim is checked in both directions: an ordering that is not
 * antisymmetric would make "newer" depend on argument order. */
#define EXPECT_LT(a, b)                                                         \
    do {                                                                        \
        CHECK(cmp((a), (b)) < 0, "\"%s\" not < \"%s\"", (a), (b));              \
        CHECK(cmp((b), (a)) > 0, "\"%s\" not > \"%s\"", (b), (a));              \
    } while (0)

#define EXPECT_EQ(a, b)                                                         \
    do {                                                                        \
        CHECK(cmp((a), (b)) == 0, "\"%s\" != \"%s\"", (a), (b));                \
        CHECK(cmp((b), (a)) == 0, "\"%s\" != \"%s\" (swapped)", (b), (a));      \
    } while (0)

/* ---- parsing ------------------------------------------------------------- */

static void test_parse_shapes(void)
{
    CASE("parse shapes");
    struct { const char *tag; unsigned n0, n1, n2; const char *pre; } t[] = {
        { "v0.10.0",        0, 10, 0, ""       },
        { "0.10",           0, 10, 0, ""       },
        { "V1.2.3",         1,  2, 3, ""       },
        { "  v1.2.3",       1,  2, 3, ""       },
        { "v1.2.3-rc2",     1,  2, 3, "rc2"    },
        { "1.2-beta",       1,  2, 0, "beta"   },
        { "1.2.3+build7",   1,  2, 3, ""       },  /* build metadata != prerelease */
        { "1.2.3-rc1+b7",   1,  2, 3, "rc1+b7" },  /* '+' after '-' stays in pre  */
        { "1.2.",           1,  2, 0, ""       },
        { "7",              7,  0, 0, ""       },
        { "1.2.3-",         1,  2, 3, ""       },  /* empty suffix = final        */
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        db_semver_t v;
        CHECK(db_ver_parse(t[i].tag, &v), "\"%s\" did not parse", t[i].tag);
        CHECK(v.num[0] == t[i].n0 && v.num[1] == t[i].n1 && v.num[2] == t[i].n2,
              "\"%s\" -> %u.%u.%u, want %u.%u.%u", t[i].tag,
              v.num[0], v.num[1], v.num[2], t[i].n0, t[i].n1, t[i].n2);
        CHECK(strcmp(v.pre, t[i].pre) == 0,
              "\"%s\" pre \"%s\", want \"%s\"", t[i].tag, v.pre, t[i].pre);
    }
}

static void test_parse_rejects(void)
{
    CASE("parse rejects");
    const char *bad[] = { "nightly", "", "v", "-1.2.3", "rc1" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        db_semver_t v;
        CHECK(!db_ver_parse(bad[i], &v), "\"%s\" parsed but should not", bad[i]);
    }
    db_semver_t v;
    CHECK(!db_ver_parse(NULL, &v), "NULL parsed but should not");
}

/* ---- ordering ------------------------------------------------------------ */

static void test_numeric_ordering(void)
{
    CASE("numeric ordering");
    /* The reason this module exists at all: strcmp would invert these. */
    EXPECT_LT("0.9.0", "0.10.0");
    EXPECT_LT("v0.2.0", "v0.2.1");
    EXPECT_LT("v0.2.9", "v0.3.0");
    EXPECT_LT("1.9.9", "10.0.0");
    EXPECT_EQ("v1.2.3", "1.2.3");
    EXPECT_EQ("1.2", "1.2.0");
    EXPECT_EQ("1.2.3+build7", "1.2.3+build9");   /* metadata never orders */
}

static void test_prerelease_before_final(void)
{
    CASE("prerelease < final");
    /* Both directions of the bug this rule fixes: the rc runner must see the
     * final as newer, and the final runner must never be offered the rc. */
    EXPECT_LT("v0.7.0-rc1", "v0.7.0");
    EXPECT_LT("0.7.0-rc1", "0.7.0+build2");
    /* ...but only against the SAME triple: a prerelease of the next release is
     * still newer than the previous final. */
    EXPECT_LT("v0.7.0", "v0.8.0-rc1");
}

static void test_prerelease_vs_prerelease(void)
{
    CASE("prerelease vs prerelease");
    EXPECT_EQ("v0.7.0-rc1", "0.7.0-rc1");
    EXPECT_LT("v0.7.0-rc1", "v0.7.0-rc2");
    EXPECT_LT("v0.7.0-alpha", "v0.7.0-beta");
    /* The documented strcmp approximation: "rc10" < "rc9" as text. Pinned so a
     * future fix to full semver identifier compare updates this on purpose. */
    EXPECT_LT("v0.7.0-rc10", "v0.7.0-rc9");
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    printf("version_cmp host tests\n");
    printf("---------------------\n");

    test_parse_shapes();
    test_parse_rejects();
    test_numeric_ordering();
    test_prerelease_before_final();
    test_prerelease_vs_prerelease();

    printf("---------------------\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
