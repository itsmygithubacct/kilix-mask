#include "kilix_mask_rects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

static unsigned seed = 1u;

static unsigned next_random(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
}

static bool same_cells(const kmask *a, const kmask *b, uint8_t region)
{
    if (kmask_grid_width(a) != kmask_grid_width(b) ||
        kmask_grid_height(a) != kmask_grid_height(b)) {
        return false;
    }
    for (int cy = 0; cy < kmask_grid_height(a); cy++) {
        for (int cx = 0; cx < kmask_grid_width(a); cx++) {
            const bool in_a = kmask_get(a, cx, cy) == region;
            const bool in_b = kmask_get(b, cx, cy) == region;

            if (in_a != in_b) {
                (void)fprintf(stderr, "  cell %d,%d: %d vs %d\n", cx, cy,
                              in_a, in_b);
                return false;
            }
        }
    }
    return true;
}

static bool
test_cover_is_exact(void)
{
    kmask *mask = NULL;
    kmask *back = NULL;
    kmask_rect rects[512];
    size_t needed = 0u;
    size_t count;

    CHECK(kmask_create(&mask, 200, 120, 4));
    kmask_fill_rect(mask, 20, 20, 100, 60, 1u);
    kmask_fill_rect(mask, 120, 40, 180, 100, 1u);

    /* Capacity 0 reports what is needed, so a caller can size a buffer
     * without guessing. */
    CHECK(kmask_cover(mask, 1u, NULL, 0u, &needed) == 0u);
    CHECK(needed >= 2u);

    count = kmask_cover(mask, 1u, rects, 512u, &needed);
    CHECK(count == needed);
    CHECK(count >= 2u);

    /* Painting the rects onto an empty map reproduces the region exactly:
     * every cell, no more and no fewer. */
    CHECK(kmask_create(&back, 200, 120, 4));
    for (size_t i = 0u; i < count; i++) {
        kmask_fill_rect(back, rects[i].x, rects[i].y,
                        rects[i].x + rects[i].w, rects[i].y + rects[i].h, 1u);
    }
    CHECK(same_cells(mask, back, 1u));

    /* Rectangles land on cell boundaries, which is what makes that exact
     * whichever way a consumer rasterises them. */
    for (size_t i = 0u; i < count; i++) {
        CHECK(rects[i].x % 4 == 0 && rects[i].y % 4 == 0);
        CHECK(rects[i].w % 4 == 0 && rects[i].h % 4 == 0);
        CHECK(rects[i].w > 0 && rects[i].h > 0);
    }

    /* A region nobody painted needs no rectangles, which is not an error. */
    CHECK(kmask_cover(mask, 9u, rects, 512u, &needed) == 0u);
    CHECK(needed == 0u);

    kmask_free(back);
    kmask_free(mask);
    return true;
}

/*
 * The property the whole header exists for.  Without it, every
 * edit-save-reload cycle would erode a shape slightly and the drift would
 * only surface after enough of them that nobody could say which edit did
 * it.
 */
static bool
test_decompose_apply_is_a_fixpoint(void)
{
    kmask *mask = NULL;
    kmask *back = NULL;
    kmask_rect bounds;
    kmask_rect holes[1024];
    size_t needed = 0u;

    CHECK(kmask_create(&mask, 300, 180, 6));
    /* A room shape: a floor with furniture cut out of it, including an
     * L and a diagonal edge, which is where a naive cover goes ragged. */
    kmask_fill_rect(mask, 12, 12, 288, 168, 1u);
    kmask_fill_rect(mask, 60, 40, 120, 90, 0u);
    kmask_fill_rect(mask, 90, 40, 150, 60, 0u);
    kmask_fill_rect(mask, 200, 100, 240, 150, 0u);
    for (int i = 0; i < 20; i++) {
        kmask_fill_rect(mask, 150 + i * 6, 60 + i * 5, 156 + i * 6,
                        66 + i * 5, 0u);
    }

    CHECK(kmask_decompose(mask, 1u, &bounds, holes, 1024u, &needed));
    CHECK(bounds.w > 0 && bounds.h > 0);
    CHECK(needed > 0u);

    CHECK(kmask_create(&back, 300, 180, 6));
    CHECK(kmask_apply(back, 1u, &bounds, holes, needed));
    CHECK(same_cells(mask, back, 1u));

    /* And it is stable: decomposing the reconstruction gives the same
     * answer, so repeated round trips cannot drift. */
    {
        kmask_rect bounds2;
        kmask_rect holes2[1024];
        size_t needed2 = 0u;

        CHECK(kmask_decompose(back, 1u, &bounds2, holes2, 1024u, &needed2));
        CHECK(needed2 == needed);
        CHECK(bounds2.x == bounds.x && bounds2.y == bounds.y);
        CHECK(bounds2.w == bounds.w && bounds2.h == bounds.h);
        CHECK(memcmp(holes2, holes, needed * sizeof(kmask_rect)) == 0);
    }

    kmask_free(back);
    kmask_free(mask);
    return true;
}

/* The same property over shapes nobody designed, which is where a cover
 * that only works on tidy rectangles falls apart. */
static bool
test_fixpoint_over_random_shapes(void)
{
    for (int trial = 0; trial < 40; trial++) {
        kmask *mask = NULL;
        kmask *back = NULL;
        kmask_rect bounds;
        static kmask_rect holes[4096];
        size_t needed = 0u;
        const int cell = 1 + (int)(next_random() % 5u);

        CHECK(kmask_create(&mask, 120, 80, cell));
        for (int blob = 0; blob < 12; blob++) {
            const int x = (int)(next_random() % 110u);
            const int y = (int)(next_random() % 70u);
            const int w = 2 + (int)(next_random() % 30u);
            const int h = 2 + (int)(next_random() % 25u);

            kmask_fill_rect(mask, x, y, x + w, y + h,
                            (next_random() & 3u) != 0u ? 1u : 0u);
        }
        /* An unpainted region has no bounding box, so there is nothing to
         * be a fixpoint of; skip rather than assert. */
        if (!kmask_decompose(mask, 1u, &bounds, holes, 4096u, &needed)) {
            kmask_free(mask);
            continue;
        }
        CHECK(kmask_create(&back, 120, 80, cell));
        CHECK(kmask_apply(back, 1u, &bounds, holes, needed));
        if (!same_cells(mask, back, 1u)) {
            (void)fprintf(stderr, "  trial %d cell %d\n", trial, cell);
            kmask_free(back);
            kmask_free(mask);
            return false;
        }
        kmask_free(back);
        kmask_free(mask);
    }
    return true;
}

/* Both sweeps are tried because painted shapes are run-heavy in one
 * direction; a tall thin comb costs far more rectangles the wrong way. */
static bool
test_both_sweeps_are_tried(void)
{
    kmask *wide = NULL;
    kmask *tall = NULL;
    size_t wide_needed = 0u;
    size_t tall_needed = 0u;

    /* Horizontal stripes: cheap row-major, expensive column-major. */
    CHECK(kmask_create(&wide, 100, 100, 1));
    for (int y = 0; y < 100; y += 2) {
        kmask_fill_rect(wide, 0, y, 100, y + 1, 1u);
    }
    CHECK(kmask_cover(wide, 1u, NULL, 0u, &wide_needed) == 0u);

    /* The same shape turned ninety degrees. */
    CHECK(kmask_create(&tall, 100, 100, 1));
    for (int x = 0; x < 100; x += 2) {
        kmask_fill_rect(tall, x, 0, x + 1, 100, 1u);
    }
    CHECK(kmask_cover(tall, 1u, NULL, 0u, &tall_needed) == 0u);

    /* Both must be cheap.  A single-orientation cover would produce 50
     * rectangles for one of them and 5000 for the other. */
    CHECK(wide_needed == 50u);
    CHECK(tall_needed == 50u);

    kmask_free(tall);
    kmask_free(wide);
    return true;
}

static bool
test_rejections_and_capacity(void)
{
    kmask *mask = NULL;
    kmask_rect bounds;
    kmask_rect holes[4];
    size_t needed = 0u;

    CHECK(kmask_create(&mask, 100, 100, 5));

    /* Nothing painted: no bounding box exists, and an empty one would read
     * as a region covering the origin. */
    CHECK(!kmask_decompose(mask, 1u, &bounds, holes, 4u, &needed));

    kmask_fill_rect(mask, 0, 0, 100, 100, 1u);
    /* A solid region has no holes. */
    CHECK(kmask_decompose(mask, 1u, &bounds, holes, 4u, &needed));
    CHECK(needed == 0u);
    CHECK(bounds.x == 0 && bounds.y == 0 && bounds.w == 100 && bounds.h == 100);

    /* More holes than fit: reported, not truncated, because a silently
     * short list decomposes into a different room. */
    for (int i = 0; i < 10; i++) {
        kmask_fill_rect(mask, i * 10, 50, i * 10 + 5, 55, 0u);
    }
    CHECK(!kmask_decompose(mask, 1u, &bounds, holes, 4u, &needed));
    CHECK(needed > 4u);

    CHECK(!kmask_decompose(NULL, 1u, &bounds, holes, 4u, &needed));
    CHECK(!kmask_decompose(mask, 1u, NULL, holes, 4u, &needed));
    CHECK(kmask_cover(NULL, 1u, holes, 4u, &needed) == 0u);

    CHECK(!kmask_apply(NULL, 1u, &bounds, NULL, 0u));
    CHECK(!kmask_apply(mask, 1u, NULL, NULL, 0u));
    CHECK(!kmask_apply(mask, 1u, &bounds, NULL, 3u));
    kmask_free(mask);
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"cover is exact", test_cover_is_exact},
        {"decompose then apply is a fixpoint",
         test_decompose_apply_is_a_fixpoint},
        {"fixpoint over random shapes", test_fixpoint_over_random_shapes},
        {"both sweeps are tried", test_both_sweeps_are_tried},
        {"rejections and capacity", test_rejections_and_capacity}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
