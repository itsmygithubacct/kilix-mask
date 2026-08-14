/*
 * Greedy exact rectangle cover.
 *
 * Ported from the walk-editor this module replaces, whose approach is
 * worth keeping for two reasons that only show up on real painted shapes.
 *
 * It sweeps both row-major and column-major and keeps the smaller result.
 * That is not a micro-optimisation: furniture outlines and tree lines are
 * run-heavy in one direction, and the wrong sweep can emit several times
 * as many rectangles for the same shape.
 *
 * And it grows the primary run first, then thickens only while the *whole*
 * run stays inside the shape.  Thickening cell by cell instead would leave
 * ragged single-cell rectangles behind along every diagonal edge.
 *
 * Work is in cells throughout; source coordinates are produced only at the
 * boundary.  Rectangles therefore always land on cell edges, which is what
 * makes the round trip exact - a rectangle spanning cells cx0..cx1
 * contains exactly those cells' centres and no others, whichever way a
 * consumer rasterises it.
 */

#include "kilix_mask_rects.h"

#include <stdlib.h>
#include <string.h>

typedef struct cover_state {
    uint8_t *remaining;   /* one byte per cell of the working box */
    int x0;
    int y0;
    int width;
    int height;
} cover_state;

static bool taken(const cover_state *state, int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= state->width || cy >= state->height) {
        return false;
    }
    return state->remaining[(size_t)cy * (size_t)state->width + (size_t)cx] !=
           0u;
}

static void clear_cell(cover_state *state, int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= state->width || cy >= state->height) {
        return;
    }
    state->remaining[(size_t)cy * (size_t)state->width + (size_t)cx] = 0u;
}

/*
 * One sweep.  Returns the rectangle count, writing into `out` when it is
 * non-NULL and there is room; the count is returned regardless so a
 * caller can compare the two orientations without allocating for both.
 */
static size_t sweep(const uint8_t *cells, int width, int height,
                    int cell, int origin_x, int origin_y,
                    bool column_major, kmask_rect *out, size_t capacity)
{
    cover_state state;
    size_t count = 0u;
    int seek_outer = 0;
    int seek_inner = 0;

    state.remaining = malloc((size_t)width * (size_t)height);
    if (state.remaining == NULL) {
        return 0u;
    }
    (void)memcpy(state.remaining, cells, (size_t)width * (size_t)height);
    state.x0 = origin_x;
    state.y0 = origin_y;
    state.width = width;
    state.height = height;

    for (;;) {
        int cx = -1;
        int cy = -1;
        int rect_w = 1;
        int rect_h = 1;

        /* Lowest cell in sweep order: row-major scans rows first,
         * column-major columns, and that choice is the whole difference
         * between the two results.
         *
         * The seek resumes where the last one ended rather than starting
         * over, which is safe because the first remaining cell can only
         * move forward: every cell a rectangle clears sits at or after
         * the cell it grew from, in this sweep's order.  Restarting from
         * the origin instead re-walks the cleared ground once per
         * rectangle, which on a per-pixel mask with hundreds of holes is
         * the difference between milliseconds and a second. */
        if (column_major) {
            for (int x = seek_outer; x < width && cx < 0; x++) {
                for (int y = x == seek_outer ? seek_inner : 0; y < height;
                     y++) {
                    if (taken(&state, x, y)) {
                        cx = x;
                        cy = y;
                        break;
                    }
                }
            }
        } else {
            for (int y = seek_outer; y < height && cx < 0; y++) {
                for (int x = y == seek_outer ? seek_inner : 0; x < width;
                     x++) {
                    if (taken(&state, x, y)) {
                        cx = x;
                        cy = y;
                        break;
                    }
                }
            }
        }
        if (cx < 0) {
            break;
        }
        seek_outer = column_major ? cx : cy;
        seek_inner = column_major ? cy : cx;

        if (column_major) {
            while (taken(&state, cx, cy + rect_h)) {
                rect_h++;
            }
            for (;;) {
                bool whole_run = true;

                for (int i = 0; i < rect_h; i++) {
                    if (!taken(&state, cx + rect_w, cy + i)) {
                        whole_run = false;
                        break;
                    }
                }
                if (!whole_run) {
                    break;
                }
                rect_w++;
            }
        } else {
            while (taken(&state, cx + rect_w, cy)) {
                rect_w++;
            }
            for (;;) {
                bool whole_run = true;

                for (int i = 0; i < rect_w; i++) {
                    if (!taken(&state, cx + i, cy + rect_h)) {
                        whole_run = false;
                        break;
                    }
                }
                if (!whole_run) {
                    break;
                }
                rect_h++;
            }
        }

        for (int dy = 0; dy < rect_h; dy++) {
            for (int dx = 0; dx < rect_w; dx++) {
                clear_cell(&state, cx + dx, cy + dy);
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
    free(state.remaining);
    return count;
}

/* Both orientations, keeping the smaller. */
static size_t cover_cells(const uint8_t *cells, int width, int height,
                          int cell, int origin_x, int origin_y,
                          kmask_rect *out, size_t capacity, size_t *needed)
{
    size_t row_count;
    size_t column_count;
    bool column_major;
    size_t total;

    if (width <= 0 || height <= 0) {
        if (needed != NULL) {
            *needed = 0u;
        }
        return 0u;
    }
    row_count = sweep(cells, width, height, cell, origin_x, origin_y, false,
                      NULL, 0u);
    column_count = sweep(cells, width, height, cell, origin_x, origin_y, true,
                         NULL, 0u);
    column_major = column_count < row_count;
    total = column_major ? column_count : row_count;
    if (needed != NULL) {
        *needed = total;
    }
    if (out == NULL || capacity == 0u) {
        return 0u;
    }
    (void)sweep(cells, width, height, cell, origin_x, origin_y, column_major,
                out, capacity);
    return total < capacity ? total : capacity;
}

size_t kmask_cover(
    const kmask *mask,
    uint8_t region,
    kmask_rect *rects,
    size_t capacity,
    size_t *needed)
{
    const int width = kmask_grid_width(mask);
    const int height = kmask_grid_height(mask);
    uint8_t *cells;
    size_t written;

    if (needed != NULL) {
        *needed = 0u;
    }
    if (mask == NULL || width <= 0 || height <= 0) {
        return 0u;
    }
    cells = calloc((size_t)width * (size_t)height, 1u);
    if (cells == NULL) {
        return 0u;
    }
    for (int cy = 0; cy < height; cy++) {
        const uint8_t *row = kmask_row(mask, cy);

        for (int cx = 0; cx < width; cx++) {
            cells[(size_t)cy * (size_t)width + (size_t)cx] =
                row[cx] == region ? 1u : 0u;
        }
    }
    written = cover_cells(cells, width, height, kmask_cell(mask), 0, 0, rects,
                          capacity, needed);
    free(cells);
    return written;
}

bool kmask_decompose(
    const kmask *mask,
    uint8_t region,
    kmask_rect *bounds,
    kmask_rect *holes,
    size_t capacity,
    size_t *needed)
{
    const int width = kmask_grid_width(mask);
    const int height = kmask_grid_height(mask);
    const int cell = kmask_cell(mask);
    int min_cx = width;
    int min_cy = height;
    int max_cx = -1;
    int max_cy = -1;
    int box_width;
    int box_height;
    uint8_t *inverse;
    size_t count = 0u;
    bool ok;

    if (needed != NULL) {
        *needed = 0u;
    }
    if (mask == NULL || bounds == NULL || width <= 0 || height <= 0) {
        return false;
    }
    for (int cy = 0; cy < height; cy++) {
        const uint8_t *row = kmask_row(mask, cy);

        for (int cx = 0; cx < width; cx++) {
            if (row[cx] != region) {
                continue;
            }
            if (cx < min_cx) { min_cx = cx; }
            if (cy < min_cy) { min_cy = cy; }
            if (cx > max_cx) { max_cx = cx; }
            if (cy > max_cy) { max_cy = cy; }
        }
    }
    if (max_cx < 0) {
        /* Unpainted: there is no bounding box of nothing, and returning an
         * empty one would read as "a region covering the origin". */
        return false;
    }
    bounds->x = min_cx * cell;
    bounds->y = min_cy * cell;
    bounds->w = (max_cx - min_cx + 1) * cell;
    bounds->h = (max_cy - min_cy + 1) * cell;

    box_width = max_cx - min_cx + 1;
    box_height = max_cy - min_cy + 1;
    inverse = calloc((size_t)box_width * (size_t)box_height, 1u);
    if (inverse == NULL) {
        return false;
    }
    for (int cy = 0; cy < box_height; cy++) {
        const uint8_t *row = kmask_row(mask, min_cy + cy) + min_cx;

        for (int cx = 0; cx < box_width; cx++) {
            inverse[(size_t)cy * (size_t)box_width + (size_t)cx] =
                row[cx] == region ? (uint8_t)0u : (uint8_t)1u;
        }
    }
    (void)cover_cells(inverse, box_width, box_height, cell, min_cx, min_cy,
                      holes, capacity, &count);
    free(inverse);
    if (needed != NULL) {
        *needed = count;
    }
    /* Reporting how many were needed rather than truncating: a caller with
     * a fixed budget has to know whether to grow it or simplify the shape,
     * and a silently short list would decompose into a different room. */
    ok = holes != NULL && count <= capacity;
    return ok;
}

/* A hole in grid coordinates, so the bounds walk below compares instead
 * of dividing.  The four divisions per hole are loop invariant across
 * the whole walk, and recomputing them per cell per hole is what made
 * applying a per-pixel decomposition cost the better part of a second. */
typedef struct cell_hole {
    int x0;
    int y0;
    int x1;
    int y1;
} cell_hole;

static bool blocked_by_source_holes(const kmask_rect *holes,
                                    size_t hole_count, int cell,
                                    int cx, int cy)
{
    for (size_t i = 0u; i < hole_count; i++) {
        const int hx0 = holes[i].x / cell;
        const int hy0 = holes[i].y / cell;
        const int hx1 = (holes[i].x + holes[i].w) / cell;
        const int hy1 = (holes[i].y + holes[i].h) / cell;

        if (cx >= hx0 && cx < hx1 && cy >= hy0 && cy < hy1) {
            return true;
        }
    }
    return false;
}

bool kmask_apply(
    kmask *mask,
    uint8_t region,
    const kmask_rect *bounds,
    const kmask_rect *holes,
    size_t hole_count)
{
    const int cell = mask != NULL ? kmask_cell(mask) : 0;
    int cx0;
    int cy0;
    int cx1;
    int cy1;
    cell_hole *cells = NULL;
    size_t *active = NULL;

    if (mask == NULL || bounds == NULL || cell <= 0) {
        return false;
    }
    if (bounds->w <= 0 || bounds->h <= 0) {
        return false;
    }
    if (holes == NULL && hole_count > 0u) {
        return false;
    }
    cx0 = bounds->x / cell;
    cy0 = bounds->y / cell;
    cx1 = (bounds->x + bounds->w) / cell;
    cy1 = (bounds->y + bounds->h) / cell;

    if (hole_count > 0u) {
        cells = malloc(hole_count * sizeof(*cells));
        active = malloc(hole_count * sizeof(*active));
    }
    if (cells == NULL || active == NULL) {
        /* Applying never failed for want of memory before and still does
         * not: without room for the converted holes, fall back to
         * dividing in place.  Slow, correct, and taken only under
         * pressure this function did not create. */
        free(cells);
        free(active);
        for (int cy = cy0; cy < cy1; cy++) {
            for (int cx = cx0; cx < cx1; cx++) {
                if (!blocked_by_source_holes(holes, hole_count, cell, cx,
                                             cy)) {
                    kmask_set(mask, cx, cy, region);
                }
            }
        }
        return true;
    }
    for (size_t i = 0u; i < hole_count; i++) {
        cells[i].x0 = holes[i].x / cell;
        cells[i].y0 = holes[i].y / cell;
        cells[i].x1 = (holes[i].x + holes[i].w) / cell;
        cells[i].y1 = (holes[i].y + holes[i].h) / cell;
    }
    for (int cy = cy0; cy < cy1; cy++) {
        size_t active_count = 0u;

        /* Most holes touch few rows, so collect the ones crossing this
         * row once and test cells against those alone. */
        for (size_t i = 0u; i < hole_count; i++) {
            if (cy >= cells[i].y0 && cy < cells[i].y1) {
                active[active_count++] = i;
            }
        }
        for (int cx = cx0; cx < cx1; cx++) {
            bool blocked = false;

            for (size_t i = 0u; i < active_count && !blocked; i++) {
                const cell_hole *hole = &cells[active[i]];

                blocked = cx >= hole->x0 && cx < hole->x1;
            }
            if (!blocked) {
                kmask_set(mask, cx, cy, region);
            }
        }
    }
    free(active);
    free(cells);
    return true;
}
