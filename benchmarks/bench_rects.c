/*
 * Decompose timings on the masks the README quotes, plus a floor that
 * fails loudly if the cost regresses by orders of magnitude.
 *
 * The numbers matter beyond curiosity: the editor recomputes the live
 * rectangle count only while a decomposition stays inside a small budget,
 * and the first overrun switches the automatic refresh off for the
 * session.  A regression in this path therefore does not fail anything -
 * the count just quietly goes stale - so the regression has to be caught
 * here, where it is measured.
 *
 * The floor is throughput (grid cells per millisecond), not wall-clock
 * time, and it is deliberately generous: a machine an order of magnitude
 * slower than the ones these numbers came from passes with room to
 * spare, while the failure mode being guarded against - the seek work
 * growing with the number of rectangles as well as the number of cells -
 * misses it a hundredfold.
 */

#include "kilix_mask_rects.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 10k cells/ms: roughly 40x below what the 1080p case measures on a
 * mid-2010s laptop core, and roughly 6x above what the origin-restart
 * seek this guards against manages on the same machine. */
#define FLOOR_CELLS_PER_MS 10000.0

#define RUNS 3

static double now_ms(void)
{
    struct timespec at;

    (void)clock_gettime(CLOCK_MONOTONIC, &at);
    return (double)at.tv_sec * 1000.0 + (double)at.tv_nsec / 1e6;
}

static unsigned bench_random(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 16;
}

/* The camera shape: one large ignore zone, a ragged diagonal edge, and
 * small holes scattered over all of it.  This is the shape that makes a
 * decomposition expensive - lots of holes, none of them tidy. */
static void paint_camera_shape(kmask *mask, int width, int height)
{
    unsigned seed = 9u;

    kmask_fill_rect(mask, 8, 8, width - 8, height - 8, 1u);
    for (int i = 0; i < height / 4; i++) {
        kmask_fill_rect(mask, 8 + i * 5, 8 + i * 3, 8 + i * 5 + 4,
                        8 + i * 3 + 3, 0u);
    }
    for (int i = 0; i < 300; i++) {
        const int x = (int)(bench_random(&seed) % (unsigned)(width - 20));
        const int y = (int)(bench_random(&seed) % (unsigned)(height - 20));

        kmask_fill_rect(mask, x, y, x + 2 + (i % 6), y + 2 + (i % 4), 0u);
    }
}

/* The walkable-room shape at a coarse cell: mostly floor, a handful of
 * furniture holes.  This is the case the rectangle budget exists for,
 * and it should be so fast the timer barely sees it. */
static void paint_room_shape(kmask *mask, int width, int height)
{
    kmask_fill_rect(mask, 12, 12, width - 12, height - 12, 1u);
    kmask_fill_rect(mask, width / 6, height / 5, width / 3, height / 2, 0u);
    kmask_fill_rect(mask, width / 2, height / 2, width / 2 + width / 6,
                    height - height / 6, 0u);
    for (int i = 0; i < 20; i++) {
        kmask_fill_rect(mask, width / 3 + i * 6, height / 3 + i * 5,
                        width / 3 + i * 6 + 6, height / 3 + i * 5 + 6, 0u);
    }
}

/* Count-only decompose, which is what the editor's automatic refresh
 * runs: no output buffer, just the two counting sweeps.  Best of a few
 * runs, so a scheduler hiccup does not condemn a healthy build. */
static double best_decompose_ms(const kmask *mask, size_t *holes)
{
    double best = -1.0;

    *holes = 0u;
    for (int run = 0; run < RUNS; run++) {
        kmask_rect bounds;
        size_t needed = 0u;
        const double started = now_ms();
        double elapsed;

        (void)kmask_decompose(mask, 1u, &bounds, NULL, 0u, &needed);
        elapsed = now_ms() - started;
        if (best < 0.0 || elapsed < best) {
            best = elapsed;
        }
        if (run == 0) {
            *holes = needed;
        } else if (needed != *holes) {
            (void)fprintf(stderr,
                          "decompose is not deterministic: %zu then %zu "
                          "holes\n", *holes, needed);
            exit(1);
        }
    }
    return best;
}

static int report(const char *name, int width, int height, int cell,
                  void (*paint)(kmask *, int, int), bool enforce_floor)
{
    kmask *mask = NULL;
    size_t holes = 0u;
    double elapsed;
    double cells;
    int failed = 0;

    if (!kmask_create(&mask, width, height, cell)) {
        (void)fprintf(stderr, "could not create the %s mask\n", name);
        return 1;
    }
    paint(mask, width, height);
    elapsed = best_decompose_ms(mask, &holes);
    cells = (double)kmask_grid_width(mask) * (double)kmask_grid_height(mask);
    (void)printf("%-28s %5dx%-5d cell %-3d grid %4dx%-4d  "
                 "%8.2f ms  %6zu holes  %10.0f cells/ms\n",
                 name, width, height, cell, kmask_grid_width(mask),
                 kmask_grid_height(mask), elapsed, holes,
                 elapsed > 0.0 ? cells / elapsed : cells);
    if (enforce_floor && elapsed > 0.0 &&
        cells / elapsed < FLOOR_CELLS_PER_MS) {
        (void)fprintf(stderr,
                      "decompose fell through the floor: %.0f cells/ms "
                      "against a floor of %.0f\n",
                      cells / elapsed, FLOOR_CELLS_PER_MS);
        failed = 1;
    }
    kmask_free(mask);
    return failed;
}

int main(void)
{
    int failed = 0;

    failed |= report("room walkable", 1280, 720, 6, paint_room_shape, false);
    failed |= report("walk-behind per-pixel", 1280, 720, 1,
                     paint_camera_shape, false);
    failed |= report("camera 1080p per-pixel", 1920, 1080, 1,
                     paint_camera_shape, true);
    if (failed == 0) {
        (void)printf("decompose stayed above %.0f cells/ms\n",
                     FLOOR_CELLS_PER_MS);
    }
    return failed;
}
