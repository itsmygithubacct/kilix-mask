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

/*
 * A deliberately naive transcription of the greedy cover, kept only here:
 * scan for the first remaining cell in sweep order, grow the primary run,
 * thicken while the whole run stays inside, emit, clear, repeat.  The
 * library is free to reach the same rectangles faster, but they must be
 * the same rectangles - count, order and coordinates - or a saved room
 * stops matching what the tool showed while it was being painted.
 */
static bool ref_taken(const uint8_t *cells, int width, int height,
                      int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= width || cy >= height) {
        return false;
    }
    return cells[(size_t)cy * (size_t)width + (size_t)cx] != 0u;
}

static size_t ref_sweep(const uint8_t *cells, int width, int height,
                        int cell, int origin_x, int origin_y,
                        bool column_major, kmask_rect *out, size_t capacity)
{
    uint8_t *remaining = malloc((size_t)width * (size_t)height);
    size_t count = 0u;

    if (remaining == NULL) {
        return 0u;
    }
    (void)memcpy(remaining, cells, (size_t)width * (size_t)height);
    for (;;) {
        int cx = -1;
        int cy = -1;
        int rect_w = 1;
        int rect_h = 1;

        for (int outer = 0;
             outer < (column_major ? width : height) && cx < 0; outer++) {
            for (int inner = 0;
                 inner < (column_major ? height : width); inner++) {
                const int x = column_major ? outer : inner;
                const int y = column_major ? inner : outer;

                if (ref_taken(remaining, width, height, x, y)) {
                    cx = x;
                    cy = y;
                    break;
                }
            }
        }
        if (cx < 0) {
            break;
        }
        if (column_major) {
            while (ref_taken(remaining, width, height, cx, cy + rect_h)) {
                rect_h++;
            }
            for (;;) {
                bool whole_run = true;

                for (int i = 0; i < rect_h && whole_run; i++) {
                    whole_run =
                        ref_taken(remaining, width, height, cx + rect_w,
                                  cy + i);
                }
                if (!whole_run) {
                    break;
                }
                rect_w++;
            }
        } else {
            while (ref_taken(remaining, width, height, cx + rect_w, cy)) {
                rect_w++;
            }
            for (;;) {
                bool whole_run = true;

                for (int i = 0; i < rect_w && whole_run; i++) {
                    whole_run =
                        ref_taken(remaining, width, height, cx + i,
                                  cy + rect_h);
                }
                if (!whole_run) {
                    break;
                }
                rect_h++;
            }
        }
        for (int dy = 0; dy < rect_h; dy++) {
            for (int dx = 0; dx < rect_w; dx++) {
                remaining[(size_t)(cy + dy) * (size_t)width +
                          (size_t)(cx + dx)] = 0u;
            }
        }
        if (out != NULL && count < capacity) {
            out[count].x = (origin_x + cx) * cell;
            out[count].y = (origin_y + cy) * cell;
            out[count].w = rect_w * cell;
            out[count].h = rect_h * cell;
        }
        count++;
    }
    free(remaining);
    return count;
}

static size_t ref_cover_cells(const uint8_t *cells, int width, int height,
                              int cell, int origin_x, int origin_y,
                              kmask_rect *out, size_t capacity,
                              size_t *needed)
{
    const size_t row_count =
        ref_sweep(cells, width, height, cell, origin_x, origin_y, false,
                  NULL, 0u);
    const size_t column_count =
        ref_sweep(cells, width, height, cell, origin_x, origin_y, true,
                  NULL, 0u);
    const bool column_major = column_count < row_count;
    const size_t total = column_major ? column_count : row_count;

    if (needed != NULL) {
        *needed = total;
    }
    if (out == NULL || capacity == 0u) {
        return 0u;
    }
    (void)ref_sweep(cells, width, height, cell, origin_x, origin_y,
                    column_major, out, capacity);
    return total < capacity ? total : capacity;
}

static uint8_t *ref_grid_of(const kmask *mask, uint8_t region, bool inverse,
                            int x0, int y0, int width, int height)
{
    uint8_t *cells = malloc((size_t)width * (size_t)height);

    if (cells == NULL) {
        return NULL;
    }
    for (int cy = 0; cy < height; cy++) {
        for (int cx = 0; cx < width; cx++) {
            const bool in =
                kmask_get(mask, x0 + cx, y0 + cy) == region;

            cells[(size_t)cy * (size_t)width + (size_t)cx] =
                (in != inverse) ? 1u : 0u;
        }
    }
    return cells;
}

static bool ref_decompose(const kmask *mask, uint8_t region,
                          kmask_rect *bounds, kmask_rect *holes,
                          size_t capacity, size_t *needed)
{
    const int cell = kmask_cell(mask);
    int min_cx = kmask_grid_width(mask);
    int min_cy = kmask_grid_height(mask);
    int max_cx = -1;
    int max_cy = -1;
    uint8_t *inverse;
    size_t count = 0u;

    *needed = 0u;
    for (int cy = 0; cy < kmask_grid_height(mask); cy++) {
        for (int cx = 0; cx < kmask_grid_width(mask); cx++) {
            if (kmask_get(mask, cx, cy) != region) {
                continue;
            }
            if (cx < min_cx) { min_cx = cx; }
            if (cy < min_cy) { min_cy = cy; }
            if (cx > max_cx) { max_cx = cx; }
            if (cy > max_cy) { max_cy = cy; }
        }
    }
    if (max_cx < 0) {
        return false;
    }
    bounds->x = min_cx * cell;
    bounds->y = min_cy * cell;
    bounds->w = (max_cx - min_cx + 1) * cell;
    bounds->h = (max_cy - min_cy + 1) * cell;
    inverse = ref_grid_of(mask, region, true, min_cx, min_cy,
                          max_cx - min_cx + 1, max_cy - min_cy + 1);
    if (inverse == NULL) {
        return false;
    }
    (void)ref_cover_cells(inverse, max_cx - min_cx + 1, max_cy - min_cy + 1,
                          cell, min_cx, min_cy, holes, capacity, &count);
    free(inverse);
    *needed = count;
    return count <= capacity;
}

/* Its own stream, so inserting this test does not shuffle what the other
 * random-shape tests happen to paint. */
static unsigned ref_random(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 16;
}

#define REF_RECTS_MAX 8192u

/* Cover, decompose and the round trip, all against the reference, all on
 * the same mask. */
static bool matches_reference(const kmask *mask, uint8_t region)
{
    static kmask_rect rects[REF_RECTS_MAX];
    static kmask_rect ref_rects[REF_RECTS_MAX];
    static kmask_rect holes[REF_RECTS_MAX];
    static kmask_rect ref_holes[REF_RECTS_MAX];
    kmask_rect bounds;
    kmask_rect ref_bounds;
    kmask *back = NULL;
    uint8_t *cells;
    size_t needed = 0u;
    size_t ref_needed = 0u;
    size_t count;
    size_t ref_count;
    bool ok;
    bool ref_ok;

    /* The cover: same count asked blind, same rectangles written out. */
    CHECK(kmask_cover(mask, region, NULL, 0u, &needed) == 0u);
    cells = ref_grid_of(mask, region, false, 0, 0, kmask_grid_width(mask),
                        kmask_grid_height(mask));
    CHECK(cells != NULL);
    ref_count = ref_cover_cells(cells, kmask_grid_width(mask),
                                kmask_grid_height(mask), kmask_cell(mask),
                                0, 0, ref_rects, REF_RECTS_MAX, &ref_needed);
    free(cells);
    CHECK(needed == ref_needed);
    CHECK(needed <= REF_RECTS_MAX);
    count = kmask_cover(mask, region, rects, REF_RECTS_MAX, &needed);
    CHECK(count == ref_count);
    CHECK(memcmp(rects, ref_rects, count * sizeof(kmask_rect)) == 0);

    /* The decomposition: same verdict, bounds, count and hole list. */
    ok = kmask_decompose(mask, region, &bounds, holes, REF_RECTS_MAX,
                         &needed);
    ref_ok = ref_decompose(mask, region, &ref_bounds, ref_holes,
                           REF_RECTS_MAX, &ref_needed);
    CHECK(ok == ref_ok);
    CHECK(needed == ref_needed);
    if (!ok) {
        return true;
    }
    CHECK(bounds.x == ref_bounds.x && bounds.y == ref_bounds.y);
    CHECK(bounds.w == ref_bounds.w && bounds.h == ref_bounds.h);
    CHECK(memcmp(holes, ref_holes, needed * sizeof(kmask_rect)) == 0);

    /* And applying what came back reproduces the region, at this size too,
     * not only on the small grids the fixpoint tests walk. */
    CHECK(kmask_create(&back, kmask_source_width(mask),
                       kmask_source_height(mask), kmask_cell(mask)));
    CHECK(kmask_apply(back, region, &bounds, holes, needed));
    ok = same_cells(mask, back, region);
    kmask_free(back);
    return ok;
}

/*
 * The reference is exercised on the shapes where a faster sweep earns its
 * keep: a per-pixel camera mask - one large zone, a ragged diagonal edge,
 * holes scattered all over it - alongside a coarse room and a batch of
 * random blobs.  Any rewrite of the cover has to leave every one of these
 * answers exactly where it found them.
 */
static bool
test_cover_matches_a_naive_reference(void)
{
    unsigned local_seed = 77u;
    kmask *mask = NULL;

    /* The camera case, scaled to what a test can afford. */
    CHECK(kmask_create(&mask, 320, 200, 1));
    kmask_fill_rect(mask, 8, 8, 312, 192, 1u);
    for (int i = 0; i < 60; i++) {
        kmask_fill_rect(mask, 8 + i * 5, 8 + i * 3, 8 + i * 5 + 4,
                        8 + i * 3 + 3, 0u);
    }
    for (int i = 0; i < 120; i++) {
        const int x = (int)(ref_random(&local_seed) % 300u);
        const int y = (int)(ref_random(&local_seed) % 180u);

        kmask_fill_rect(mask, x, y, x + 2 + (i % 5), y + 2 + (i % 3), 0u);
    }
    CHECK(matches_reference(mask, 1u));
    kmask_free(mask);

    /* A room at a coarse cell, where both orientations stay in play. */
    CHECK(kmask_create(&mask, 300, 180, 6));
    kmask_fill_rect(mask, 12, 12, 288, 168, 2u);
    kmask_fill_rect(mask, 60, 40, 120, 90, 0u);
    kmask_fill_rect(mask, 200, 100, 240, 150, 0u);
    for (int i = 0; i < 20; i++) {
        kmask_fill_rect(mask, 150 + i * 6, 60 + i * 5, 156 + i * 6,
                        66 + i * 5, 0u);
    }
    CHECK(matches_reference(mask, 2u));
    kmask_free(mask);

    /* Random blobs across cell sizes, both a painted and an absent region. */
    for (int trial = 0; trial < 12; trial++) {
        const int cell = 1 + (int)(ref_random(&local_seed) % 5u);

        CHECK(kmask_create(&mask, 160, 110, cell));
        for (int blob = 0; blob < 14; blob++) {
            const int x = (int)(ref_random(&local_seed) % 150u);
            const int y = (int)(ref_random(&local_seed) % 100u);
            const unsigned r = ref_random(&local_seed);

            kmask_fill_rect(mask, x, y, x + 3 + (int)(r % 40u),
                            y + 3 + (int)(r % 28u),
                            (r & 3u) != 0u ? 1u : 0u);
        }
        CHECK(matches_reference(mask, 1u));
        CHECK(matches_reference(mask, 9u));
        kmask_free(mask);
    }
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
        {"cover matches a naive reference",
         test_cover_matches_a_naive_reference},
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
