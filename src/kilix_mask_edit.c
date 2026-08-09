/*
 * The editor, without a terminal in it.
 *
 * Two things in here are load-bearing and worth reading before changing
 * anything.
 *
 * The first is the pair of lookup tables, col_src and row_src.  They hold
 * the source pixel each view column and row samples, and *both* the
 * compositor and kmaskedit_to_source() read them.  Computing the mapping
 * twice - once to draw and once to hit-test - is how a painting tool ends
 * up putting paint one cell away from the pointer at some zoom levels
 * only.  There is one mapping here, so the two cannot disagree.
 *
 * The second is that damage is rounded outwards, always.  A rectangle a
 * pixel too large costs a few bytes on the wire.  A rectangle a pixel too
 * small leaves a stale pixel that nothing will repaint, because the next
 * frame patches only what the next change reports.
 */

#include "kilix_mask_edit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_OPS_MAX 128u
#define UNDO_BYTES_MAX (32u * 1024u * 1024u)

/* Below this many pixels per cell the grid is more line than picture. */
#define GRID_MIN_CELL_PIXELS 6

#define SCALE_MIN 0.02f
#define SCALE_MAX 64.0f

#define SURROUND_RGB 0x00141418u
#define CHECKER_A 0x00303038u
#define CHECKER_B 0x00404048u
#define CHECKER_SIZE 8
#define GRID_RGB 0x00000000u
#define GRID_ALPHA 0.25f
#define CURSOR_RGB 0x00FFFFFFu
#define PREVIEW_RGB 0x00FFD24Du

typedef struct undo_cell {
    uint32_t index;
    uint8_t before;
    uint8_t after;
} undo_cell;

typedef struct undo_op {
    undo_cell *cells;
    size_t count;
    size_t capacity;
} undo_op;

struct kmaskedit {
    kmask *mask;
    const sr_canvas *background;
    int source_width;
    int source_height;
    int cell;
    int grid_width;
    int grid_height;

    int view_width;
    int view_height;
    bool view_set;
    float scale;
    float origin_x;
    float origin_y;
    int *col_src;
    int *row_src;

    kmaskedit_tool tool;
    uint8_t region;
    int brush;
    bool grid;
    float overlay_alpha;
    int wand_tolerance;

    bool hover_valid;
    int hover_cx;
    int hover_cy;

    bool stroking;
    uint8_t stroke_value;
    int stroke_cx;
    int stroke_cy;
    bool rect_active;
    int rect_from_x;
    int rect_from_y;
    int rect_to_x;
    int rect_to_y;

    undo_op pending;
    bool recording;
    /* One spare, so a new entry is always appended and then trimmed
     * rather than every caller having to make room first. */
    undo_op ops[UNDO_OPS_MAX + 1u];
    size_t op_count;
    size_t op_cursor;
    size_t undo_bytes;
    size_t saved_cursor;
    bool saved_known;

    uint64_t revision;

    kmaskedit_rect damage[KMASKEDIT_DAMAGE_MAX_RECTS];
    size_t damage_count;
    bool damage_full;
};

/* --------------------------------- util --------------------------------- */

static int clamp_int(int value, int low, int high)
{
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

static float clamp_float(float value, float low, float high)
{
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

/* --------------------------------- damage ------------------------------- */

void kmaskedit_damage_all(kmaskedit *editor)
{
    if (editor != NULL) {
        editor->damage_full = true;
        editor->damage_count = 0u;
    }
}

/*
 * View rectangle, clamped, x1/y1 exclusive.  Callers pass rectangles that
 * are already rounded outwards; this only drops what falls off the view.
 */
static void damage_add(kmaskedit *editor, int x0, int y0, int x1, int y1)
{
    if (editor->damage_full || !editor->view_set) {
        return;
    }
    x0 = clamp_int(x0, 0, editor->view_width);
    y0 = clamp_int(y0, 0, editor->view_height);
    x1 = clamp_int(x1, 0, editor->view_width);
    y1 = clamp_int(y1, 0, editor->view_height);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (editor->damage_count >= KMASKEDIT_DAMAGE_MAX_RECTS) {
        kmaskedit_damage_all(editor);
        return;
    }
    editor->damage[editor->damage_count].x0 = x0;
    editor->damage[editor->damage_count].y0 = y0;
    editor->damage[editor->damage_count].x1 = x1;
    editor->damage[editor->damage_count].y1 = y1;
    editor->damage_count++;
}

/*
 * The view rectangle a half-open source rectangle can have touched.
 *
 * Grown by a pixel on every side.  The exact bound is computable, but it
 * depends on float rounding agreeing with the rounding the table build
 * did, and the cost of being wrong is asymmetric: too large is bytes, too
 * small is a pixel left stale for the rest of the session.
 */
static void damage_source_rect(
    kmaskedit *editor, int x0, int y0, int x1, int y1)
{
    const float sx0 = editor->origin_x + (float)x0 * editor->scale;
    const float sy0 = editor->origin_y + (float)y0 * editor->scale;
    const float sx1 = editor->origin_x + (float)x1 * editor->scale;
    const float sy1 = editor->origin_y + (float)y1 * editor->scale;

    damage_add(editor, (int)floorf(sx0) - 1, (int)floorf(sy0) - 1,
               (int)ceilf(sx1) + 1, (int)ceilf(sy1) + 1);
}

static void damage_cell_rect(
    kmaskedit *editor, int cx0, int cy0, int cx1, int cy1)
{
    damage_source_rect(editor, cx0 * editor->cell, cy0 * editor->cell,
                       (cx1 + 1) * editor->cell, (cy1 + 1) * editor->cell);
}

size_t kmaskedit_take_damage(
    kmaskedit *editor, kmaskedit_rect *rects, size_t capacity)
{
    size_t count;

    if (editor == NULL || rects == NULL || capacity == 0u ||
        !editor->view_set) {
        return 0u;
    }
    if (editor->damage_full || editor->damage_count > capacity) {
        rects[0].x0 = 0;
        rects[0].y0 = 0;
        rects[0].x1 = editor->view_width;
        rects[0].y1 = editor->view_height;
        editor->damage_full = false;
        editor->damage_count = 0u;
        return 1u;
    }
    count = editor->damage_count;
    (void)memcpy(rects, editor->damage, count * sizeof(kmaskedit_rect));
    editor->damage_count = 0u;
    return count;
}

/* ---------------------------------- undo -------------------------------- */

static void undo_op_free(undo_op *op)
{
    free(op->cells);
    op->cells = NULL;
    op->count = 0u;
    op->capacity = 0u;
}

static size_t undo_op_bytes(const undo_op *op)
{
    return op->capacity * sizeof(undo_cell);
}

/* Drop the oldest entries until the budget is met.  The newest is never
 * dropped: a single fill can exceed the whole budget on its own, and
 * losing the operation the person just performed is worse than losing
 * every one before it. */
static void undo_trim(kmaskedit *editor)
{
    size_t drop = 0u;

    while (editor->op_count - drop > 1u &&
           (editor->op_count - drop > UNDO_OPS_MAX ||
            editor->undo_bytes > UNDO_BYTES_MAX)) {
        editor->undo_bytes -= undo_op_bytes(&editor->ops[drop]);
        undo_op_free(&editor->ops[drop]);
        drop++;
    }
    if (drop == 0u) {
        return;
    }
    (void)memmove(editor->ops, editor->ops + drop,
                  (editor->op_count - drop) * sizeof(undo_op));
    editor->op_count -= drop;
    editor->op_cursor = editor->op_cursor > drop ? editor->op_cursor - drop
                                                 : 0u;
    if (editor->saved_known && editor->saved_cursor >= drop) {
        editor->saved_cursor -= drop;
    } else if (editor->saved_known) {
        /* The saved point itself was discarded, so "unchanged since save"
         * can no longer be established; report modified from now on. */
        editor->saved_known = false;
    }
}

static void undo_discard_redo(kmaskedit *editor)
{
    while (editor->op_count > editor->op_cursor) {
        editor->op_count--;
        editor->undo_bytes -= undo_op_bytes(&editor->ops[editor->op_count]);
        undo_op_free(&editor->ops[editor->op_count]);
    }
    if (editor->saved_known && editor->saved_cursor > editor->op_cursor) {
        editor->saved_known = false;
    }
}

static void undo_begin(kmaskedit *editor)
{
    editor->pending.count = 0u;
    editor->recording = true;
}

static void undo_end(kmaskedit *editor)
{
    editor->recording = false;
    if (editor->pending.count == 0u) {
        return;
    }
    undo_discard_redo(editor);
    /* The pending buffer is handed over rather than copied, and a fresh
     * one is grown by the next stroke. */
    editor->ops[editor->op_count] = editor->pending;
    editor->undo_bytes += undo_op_bytes(&editor->pending);
    editor->pending.cells = NULL;
    editor->pending.count = 0u;
    editor->pending.capacity = 0u;
    editor->op_count++;
    editor->op_cursor = editor->op_count;
    undo_trim(editor);
}

static bool undo_record(kmaskedit *editor, uint32_t index, uint8_t before,
                        uint8_t after)
{
    undo_op *op = &editor->pending;

    if (op->count == op->capacity) {
        const size_t grown = op->capacity == 0u ? 256u : op->capacity * 2u;
        undo_cell *cells = realloc(op->cells, grown * sizeof(undo_cell));

        if (cells == NULL) {
            return false;
        }
        op->cells = cells;
        op->capacity = grown;
    }
    op->cells[op->count].index = index;
    op->cells[op->count].before = before;
    op->cells[op->count].after = after;
    op->count++;
    return true;
}

/* ------------------------------- cell writes ---------------------------- */

/* Every cell write goes through here, so nothing can change the map
 * without moving the revision on with it. */
static void apply_cell(kmaskedit *editor, int cx, int cy, uint8_t value)
{
    kmask_set(editor->mask, cx, cy, value);
    editor->revision++;
}

static bool set_cell(kmaskedit *editor, int cx, int cy, uint8_t value)
{
    uint8_t before;

    if (cx < 0 || cy < 0 || cx >= editor->grid_width ||
        cy >= editor->grid_height) {
        return false;
    }
    before = kmask_get(editor->mask, cx, cy);
    if (before == value) {
        return false;
    }
    apply_cell(editor, cx, cy, value);
    if (editor->recording) {
        (void)undo_record(editor,
                          (uint32_t)((size_t)cy * (size_t)editor->grid_width +
                                     (size_t)cx),
                          before, value);
    }
    return true;
}

/* One brush stamp centred on a cell, damaging its whole footprint. */
static void stamp(kmaskedit *editor, int cx, int cy, uint8_t value)
{
    const int half = editor->brush / 2;
    const int cx0 = cx - half;
    const int cy0 = cy - half;
    const int cx1 = cx + half;
    const int cy1 = cy + half;
    bool changed = false;

    for (int y = cy0; y <= cy1; y++) {
        for (int x = cx0; x <= cx1; x++) {
            changed |= set_cell(editor, x, y, value);
        }
    }
    if (changed) {
        damage_cell_rect(editor, cx0, cy0, cx1, cy1);
    }
}

/*
 * Stamp along the cell line between two points.
 *
 * A terminal reports pointer motion at whatever rate it manages, so a
 * quick drag arrives as two positions a long way apart.  Without this the
 * stroke is a row of dots.  Walking in cells rather than source pixels
 * keeps the work proportional to what is actually painted: at a cell size
 * of 6 that is a sixth of the steps for the same stroke.
 */
static void stamp_line(kmaskedit *editor, int cx0, int cy0, int cx1, int cy1,
                       uint8_t value)
{
    const int dx = abs(cx1 - cx0);
    const int dy = -abs(cy1 - cy0);
    const int step_x = cx0 < cx1 ? 1 : -1;
    const int step_y = cy0 < cy1 ? 1 : -1;
    int error = dx + dy;
    int x = cx0;
    int y = cy0;

    for (;;) {
        stamp(editor, x, y, value);
        if (x == cx1 && y == cy1) {
            break;
        }
        {
            const int doubled = 2 * error;

            if (doubled >= dy) {
                error += dy;
                x += step_x;
            }
            if (doubled <= dx) {
                error += dx;
                y += step_y;
            }
        }
    }
}

/* ------------------------------- lifetime ------------------------------- */

static void rebuild_tables(kmaskedit *editor)
{
    if (!editor->view_set) {
        return;
    }
    for (int vx = 0; vx < editor->view_width; vx++) {
        editor->col_src[vx] =
            (int)floorf(((float)vx - editor->origin_x) / editor->scale);
    }
    for (int vy = 0; vy < editor->view_height; vy++) {
        editor->row_src[vy] =
            (int)floorf(((float)vy - editor->origin_y) / editor->scale);
    }
    kmaskedit_damage_all(editor);
}

/* Keep the image where it can be seen: centred when it is smaller than the
 * view, held against the edges when it is larger. */
static void clamp_origin(kmaskedit *editor)
{
    const float shown_w = (float)editor->source_width * editor->scale;
    const float shown_h = (float)editor->source_height * editor->scale;

    if (shown_w <= (float)editor->view_width) {
        editor->origin_x = ((float)editor->view_width - shown_w) * 0.5f;
    } else {
        editor->origin_x = clamp_float(
            editor->origin_x, (float)editor->view_width - shown_w, 0.0f);
    }
    if (shown_h <= (float)editor->view_height) {
        editor->origin_y = ((float)editor->view_height - shown_h) * 0.5f;
    } else {
        editor->origin_y = clamp_float(
            editor->origin_y, (float)editor->view_height - shown_h, 0.0f);
    }
}

bool kmaskedit_create(
    kmaskedit **editor, kmask *mask, const sr_canvas *background)
{
    kmaskedit *result;

    if (editor == NULL) {
        return false;
    }
    *editor = NULL;
    if (mask == NULL) {
        return false;
    }
    if (background != NULL &&
        (background->px == NULL ||
         background->w != kmask_source_width(mask) ||
         background->h != kmask_source_height(mask))) {
        /* Scaling to fit would put every painted region over the wrong
         * pixels while looking entirely reasonable on screen. */
        return false;
    }
    result = calloc(1u, sizeof(*result));
    if (result == NULL) {
        return false;
    }
    result->mask = mask;
    result->background = background;
    result->source_width = kmask_source_width(mask);
    result->source_height = kmask_source_height(mask);
    result->cell = kmask_cell(mask);
    result->grid_width = kmask_grid_width(mask);
    result->grid_height = kmask_grid_height(mask);
    result->scale = 1.0f;
    result->tool = KMASKEDIT_TOOL_BRUSH;
    result->region = 1u;
    result->brush = 1;
    result->grid = true;
    result->overlay_alpha = 0.45f;
    result->wand_tolerance = 32;
    result->saved_known = true;
    result->saved_cursor = 0u;
    *editor = result;
    return true;
}

void kmaskedit_free(kmaskedit *editor)
{
    if (editor == NULL) {
        return;
    }
    for (size_t i = 0u; i < editor->op_count; i++) {
        undo_op_free(&editor->ops[i]);
    }
    undo_op_free(&editor->pending);
    free(editor->col_src);
    free(editor->row_src);
    free(editor);
}

kmask *kmaskedit_mask(const kmaskedit *editor)
{
    return editor != NULL ? editor->mask : NULL;
}

/* ------------------------------- viewport ------------------------------- */

bool kmaskedit_set_view(kmaskedit *editor, int width, int height)
{
    int *columns;
    int *rows;

    if (editor == NULL || width <= 0 || height <= 0) {
        return false;
    }
    columns = realloc(editor->col_src, (size_t)width * sizeof(int));
    if (columns == NULL) {
        return false;
    }
    editor->col_src = columns;
    rows = realloc(editor->row_src, (size_t)height * sizeof(int));
    if (rows == NULL) {
        return false;
    }
    editor->row_src = rows;
    editor->view_width = width;
    editor->view_height = height;
    if (!editor->view_set) {
        editor->view_set = true;
        kmaskedit_fit(editor);
        return true;
    }
    /* A resize keeps the operator's zoom; only the framing is redone. */
    clamp_origin(editor);
    rebuild_tables(editor);
    return true;
}

int kmaskedit_view_width(const kmaskedit *editor)
{
    return editor != NULL ? editor->view_width : 0;
}

int kmaskedit_view_height(const kmaskedit *editor)
{
    return editor != NULL ? editor->view_height : 0;
}

void kmaskedit_fit(kmaskedit *editor)
{
    float fit_x;
    float fit_y;

    if (editor == NULL || !editor->view_set) {
        return;
    }
    fit_x = (float)editor->view_width / (float)editor->source_width;
    fit_y = (float)editor->view_height / (float)editor->source_height;
    editor->scale = clamp_float(fit_x < fit_y ? fit_x : fit_y, SCALE_MIN,
                                SCALE_MAX);
    clamp_origin(editor);
    rebuild_tables(editor);
}

float kmaskedit_scale(const kmaskedit *editor)
{
    return editor != NULL ? editor->scale : 0.0f;
}

void kmaskedit_zoom(kmaskedit *editor, int steps, int anchor_x, int anchor_y)
{
    float scale;
    float source_x;
    float source_y;

    if (editor == NULL || !editor->view_set || steps == 0) {
        return;
    }
    scale = editor->scale * powf(2.0f, (float)steps);
    scale = clamp_float(scale, SCALE_MIN, SCALE_MAX);
    if (scale == editor->scale) {
        return;
    }
    /* Hold the source point under the anchor still, so zooming follows
     * the pointer rather than the middle of the view. */
    source_x = ((float)anchor_x - editor->origin_x) / editor->scale;
    source_y = ((float)anchor_y - editor->origin_y) / editor->scale;
    editor->scale = scale;
    editor->origin_x = (float)anchor_x - source_x * scale;
    editor->origin_y = (float)anchor_y - source_y * scale;
    clamp_origin(editor);
    rebuild_tables(editor);
}

void kmaskedit_pan(kmaskedit *editor, int dx, int dy)
{
    if (editor == NULL || !editor->view_set || (dx == 0 && dy == 0)) {
        return;
    }
    editor->origin_x += (float)dx;
    editor->origin_y += (float)dy;
    clamp_origin(editor);
    rebuild_tables(editor);
}

bool kmaskedit_to_source(
    const kmaskedit *editor, int view_x, int view_y, int *x, int *y)
{
    int source_x;
    int source_y;

    if (editor == NULL || !editor->view_set) {
        return false;
    }
    if (view_x < 0 || view_y < 0 || view_x >= editor->view_width ||
        view_y >= editor->view_height) {
        return false;
    }
    /* The same tables the compositor samples through, which is what makes
     * the cell under the pointer the cell that was drawn there. */
    source_x = editor->col_src[view_x];
    source_y = editor->row_src[view_y];
    if (source_x < 0 || source_y < 0 || source_x >= editor->source_width ||
        source_y >= editor->source_height) {
        return false;
    }
    if (x != NULL) {
        *x = source_x;
    }
    if (y != NULL) {
        *y = source_y;
    }
    return true;
}

void kmaskedit_to_view(
    const kmaskedit *editor, int x, int y, int *view_x, int *view_y)
{
    if (editor == NULL) {
        return;
    }
    if (view_x != NULL) {
        *view_x = (int)ceilf(editor->origin_x + (float)x * editor->scale);
    }
    if (view_y != NULL) {
        *view_y = (int)ceilf(editor->origin_y + (float)y * editor->scale);
    }
}

/* ------------------------------- settings ------------------------------- */

void kmaskedit_set_tool(kmaskedit *editor, kmaskedit_tool tool)
{
    if (editor == NULL || editor->tool == tool) {
        return;
    }
    kmaskedit_cancel(editor);
    editor->tool = tool;
}

kmaskedit_tool kmaskedit_get_tool(const kmaskedit *editor)
{
    return editor != NULL ? editor->tool : KMASKEDIT_TOOL_BRUSH;
}

void kmaskedit_set_region(kmaskedit *editor, uint8_t region)
{
    if (editor != NULL) {
        editor->region = region;
    }
}

uint8_t kmaskedit_get_region(const kmaskedit *editor)
{
    return editor != NULL ? editor->region : 0u;
}

void kmaskedit_set_brush(kmaskedit *editor, int cells)
{
    int size;

    if (editor == NULL) {
        return;
    }
    size = clamp_int(cells, 1, KMASKEDIT_BRUSH_MAX);
    size |= 1; /* even footprints have no centre cell to sit under the
                * pointer, so the brush would paint off to one side */
    if (size == editor->brush) {
        return;
    }
    if (editor->hover_valid) {
        /* The larger of the two footprints, because shrinking the brush
         * has to erase the outline the bigger one left behind. */
        const int half = (size > editor->brush ? size : editor->brush) / 2;

        damage_cell_rect(editor, editor->hover_cx - half - 1,
                         editor->hover_cy - half - 1,
                         editor->hover_cx + half + 1,
                         editor->hover_cy + half + 1);
    }
    editor->brush = size;
}

int kmaskedit_get_brush(const kmaskedit *editor)
{
    return editor != NULL ? editor->brush : 0;
}

void kmaskedit_set_grid(kmaskedit *editor, bool show)
{
    if (editor != NULL && editor->grid != show) {
        editor->grid = show;
        kmaskedit_damage_all(editor);
    }
}

bool kmaskedit_get_grid(const kmaskedit *editor)
{
    return editor != NULL && editor->grid;
}

void kmaskedit_set_overlay_alpha(kmaskedit *editor, float alpha)
{
    if (editor != NULL) {
        editor->overlay_alpha = clamp_float(alpha, 0.0f, 1.0f);
        kmaskedit_damage_all(editor);
    }
}

void kmaskedit_set_wand_tolerance(kmaskedit *editor, int tolerance)
{
    if (editor != NULL) {
        editor->wand_tolerance = clamp_int(tolerance, 0, 441);
    }
}

int kmaskedit_get_wand_tolerance(const kmaskedit *editor)
{
    return editor != NULL ? editor->wand_tolerance : 0;
}

/* -------------------------------- pointer ------------------------------- */

/* The cursor is drawn one cell larger than the brush so its outline sits
 * outside the painted footprint; damage has to cover that. */
static void damage_cursor(kmaskedit *editor, int cx, int cy)
{
    const int half = editor->brush / 2;

    damage_cell_rect(editor, cx - half - 1, cy - half - 1, cx + half + 1,
                     cy + half + 1);
}

void kmaskedit_hover(kmaskedit *editor, int view_x, int view_y)
{
    int source_x;
    int source_y;
    int cx;
    int cy;

    if (editor == NULL || !editor->view_set) {
        return;
    }
    if (!kmaskedit_to_source(editor, view_x, view_y, &source_x, &source_y)) {
        if (editor->hover_valid) {
            damage_cursor(editor, editor->hover_cx, editor->hover_cy);
            editor->hover_valid = false;
        }
        return;
    }
    cx = source_x / editor->cell;
    cy = source_y / editor->cell;
    if (editor->hover_valid && cx == editor->hover_cx &&
        cy == editor->hover_cy) {
        return;
    }
    if (editor->hover_valid) {
        damage_cursor(editor, editor->hover_cx, editor->hover_cy);
    }
    editor->hover_valid = true;
    editor->hover_cx = cx;
    editor->hover_cy = cy;
    damage_cursor(editor, cx, cy);
}

bool kmaskedit_hover_cell(const kmaskedit *editor, int *cx, int *cy)
{
    if (editor == NULL || !editor->hover_valid) {
        return false;
    }
    if (cx != NULL) {
        *cx = editor->hover_cx;
    }
    if (cy != NULL) {
        *cy = editor->hover_cy;
    }
    return true;
}

static void rect_preview_damage(kmaskedit *editor)
{
    const int x0 = editor->rect_from_x < editor->rect_to_x ? editor->rect_from_x
                                                           : editor->rect_to_x;
    const int y0 = editor->rect_from_y < editor->rect_to_y ? editor->rect_from_y
                                                           : editor->rect_to_y;
    const int x1 = editor->rect_from_x > editor->rect_to_x ? editor->rect_from_x
                                                           : editor->rect_to_x;
    const int y1 = editor->rect_from_y > editor->rect_to_y ? editor->rect_from_y
                                                           : editor->rect_to_y;

    /* The outline is drawn on the rectangle's edge, so the damage has to
     * take in a pixel beyond it on each side. */
    damage_source_rect(editor, x0 - 2, y0 - 2, x1 + 3, y1 + 3);
}

/*
 * Flood fill over the background image, painting the mask cells the
 * matched pixels fall in.
 *
 * Breadth-first with a visited byte per pixel, rather than recursion: a
 * fill over a 1080p sky is two million pixels deep in the worst case and
 * would take the stack with it.  Both buffers are transient - a fill is a
 * click, not a frame - and a fill that cannot allocate does nothing rather
 * than filling part of the region.
 */
static void wand_fill(kmaskedit *editor, int seed_x, int seed_y, uint8_t value)
{
    const sr_canvas *image = editor->background;
    const size_t pixels = (size_t)editor->source_width *
                          (size_t)editor->source_height;
    const int tolerance = editor->wand_tolerance;
    const long limit = (long)tolerance * (long)tolerance;
    uint8_t *visited;
    uint32_t *queue;
    size_t head = 0u;
    size_t tail = 0u;
    uint32_t seed_colour;
    int seed_r;
    int seed_g;
    int seed_b;
    int min_cx = editor->grid_width;
    int min_cy = editor->grid_height;
    int max_cx = -1;
    int max_cy = -1;
    bool changed = false;

    if (image == NULL) {
        return;
    }
    visited = calloc(pixels, 1u);
    if (visited == NULL) {
        return;
    }
    queue = malloc(pixels * sizeof(uint32_t));
    if (queue == NULL) {
        free(visited);
        return;
    }
    seed_colour = image->px[(size_t)seed_y * (size_t)image->w +
                            (size_t)seed_x];
    seed_r = (int)((seed_colour >> 16) & 0xFFu);
    seed_g = (int)((seed_colour >> 8) & 0xFFu);
    seed_b = (int)(seed_colour & 0xFFu);

    visited[(size_t)seed_y * (size_t)editor->source_width + (size_t)seed_x] =
        1u;
    queue[tail++] = (uint32_t)((size_t)seed_y * (size_t)editor->source_width +
                               (size_t)seed_x);

    undo_begin(editor);
    while (head < tail) {
        const uint32_t index = queue[head++];
        const int x = (int)(index % (uint32_t)editor->source_width);
        const int y = (int)(index / (uint32_t)editor->source_width);
        const int cx = x / editor->cell;
        const int cy = y / editor->cell;
        static const int step_x[4] = {1, -1, 0, 0};
        static const int step_y[4] = {0, 0, 1, -1};

        if (set_cell(editor, cx, cy, value)) {
            changed = true;
        }
        if (cx < min_cx) { min_cx = cx; }
        if (cy < min_cy) { min_cy = cy; }
        if (cx > max_cx) { max_cx = cx; }
        if (cy > max_cy) { max_cy = cy; }

        for (size_t i = 0u; i < 4u; i++) {
            const int nx = x + step_x[i];
            const int ny = y + step_y[i];
            size_t neighbour;
            uint32_t colour;
            long dr;
            long dg;
            long db;

            if (nx < 0 || ny < 0 || nx >= editor->source_width ||
                ny >= editor->source_height) {
                continue;
            }
            neighbour = (size_t)ny * (size_t)editor->source_width + (size_t)nx;
            if (visited[neighbour] != 0u) {
                continue;
            }
            colour = image->px[(size_t)ny * (size_t)image->w + (size_t)nx];
            dr = (long)((colour >> 16) & 0xFFu) - seed_r;
            dg = (long)((colour >> 8) & 0xFFu) - seed_g;
            db = (long)(colour & 0xFFu) - seed_b;
            if (dr * dr + dg * dg + db * db > limit) {
                continue;
            }
            visited[neighbour] = 1u;
            queue[tail++] = (uint32_t)neighbour;
        }
    }
    undo_end(editor);
    free(queue);
    free(visited);
    if (changed && max_cx >= 0) {
        damage_cell_rect(editor, min_cx, min_cy, max_cx, max_cy);
    }
}

void kmaskedit_press(
    kmaskedit *editor, int view_x, int view_y, kmaskedit_button button)
{
    int source_x;
    int source_y;
    int cx;
    int cy;

    if (editor == NULL || editor->stroking) {
        return;
    }
    if (!kmaskedit_to_source(editor, view_x, view_y, &source_x, &source_y)) {
        return;
    }
    kmaskedit_hover(editor, view_x, view_y);
    cx = source_x / editor->cell;
    cy = source_y / editor->cell;

    if (editor->tool == KMASKEDIT_TOOL_PICK) {
        editor->region = kmask_get(editor->mask, cx, cy);
        return;
    }
    /*
     * The stroke's value is decided here and holds for the whole drag.
     * Toggling per cell instead would make a drag across a boundary turn
     * the region on and off under the pointer.
     */
    if (button == KMASKEDIT_BUTTON_ERASE) {
        editor->stroke_value = 0u;
    } else {
        editor->stroke_value =
            kmask_get(editor->mask, cx, cy) == editor->region ? 0u
                                                              : editor->region;
    }
    editor->stroking = true;

    if (editor->tool == KMASKEDIT_TOOL_RECT) {
        editor->rect_active = true;
        editor->rect_from_x = source_x;
        editor->rect_from_y = source_y;
        editor->rect_to_x = source_x;
        editor->rect_to_y = source_y;
        rect_preview_damage(editor);
        return;
    }
    if (editor->tool == KMASKEDIT_TOOL_WAND) {
        wand_fill(editor, source_x, source_y, editor->stroke_value);
        editor->stroking = false;
        return;
    }
    editor->stroke_cx = cx;
    editor->stroke_cy = cy;
    undo_begin(editor);
    stamp(editor, cx, cy, editor->stroke_value);
}

void kmaskedit_drag(kmaskedit *editor, int view_x, int view_y)
{
    int source_x;
    int source_y;

    if (editor == NULL || !editor->stroking) {
        return;
    }
    kmaskedit_hover(editor, view_x, view_y);
    if (!kmaskedit_to_source(editor, view_x, view_y, &source_x, &source_y)) {
        return;
    }
    if (editor->tool == KMASKEDIT_TOOL_RECT) {
        rect_preview_damage(editor);
        editor->rect_to_x = source_x;
        editor->rect_to_y = source_y;
        rect_preview_damage(editor);
        return;
    }
    {
        const int cx = source_x / editor->cell;
        const int cy = source_y / editor->cell;

        stamp_line(editor, editor->stroke_cx, editor->stroke_cy, cx, cy,
                   editor->stroke_value);
        editor->stroke_cx = cx;
        editor->stroke_cy = cy;
    }
}

void kmaskedit_release(kmaskedit *editor, int view_x, int view_y)
{
    if (editor == NULL || !editor->stroking) {
        return;
    }
    kmaskedit_drag(editor, view_x, view_y);
    if (editor->tool == KMASKEDIT_TOOL_RECT) {
        const int x0 = editor->rect_from_x < editor->rect_to_x
                           ? editor->rect_from_x : editor->rect_to_x;
        const int y0 = editor->rect_from_y < editor->rect_to_y
                           ? editor->rect_from_y : editor->rect_to_y;
        const int x1 = editor->rect_from_x > editor->rect_to_x
                           ? editor->rect_from_x : editor->rect_to_x;
        const int y1 = editor->rect_from_y > editor->rect_to_y
                           ? editor->rect_from_y : editor->rect_to_y;
        const int cx0 = x0 / editor->cell;
        const int cy0 = y0 / editor->cell;
        const int cx1 = x1 / editor->cell;
        const int cy1 = y1 / editor->cell;
        bool changed = false;

        rect_preview_damage(editor);
        editor->rect_active = false;
        undo_begin(editor);
        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                changed |= set_cell(editor, cx, cy, editor->stroke_value);
            }
        }
        undo_end(editor);
        if (changed) {
            damage_cell_rect(editor, cx0, cy0, cx1, cy1);
        }
    } else {
        undo_end(editor);
    }
    editor->stroking = false;
}

bool kmaskedit_stroking(const kmaskedit *editor)
{
    return editor != NULL && editor->stroking;
}

/* Roll the pending edit back rather than leaving half a stroke behind;
 * the person who pressed escape wants the state from before the press. */
static void undo_pending(kmaskedit *editor)
{
    undo_op *op = &editor->pending;

    editor->recording = false;
    while (op->count > 0u) {
        const undo_cell *entry;

        op->count--;
        entry = &op->cells[op->count];
        apply_cell(editor,
                   (int)(entry->index % (uint32_t)editor->grid_width),
                   (int)(entry->index / (uint32_t)editor->grid_width),
                   entry->before);
    }
}

void kmaskedit_cancel(kmaskedit *editor)
{
    if (editor == NULL || !editor->stroking) {
        return;
    }
    if (editor->rect_active) {
        rect_preview_damage(editor);
        editor->rect_active = false;
    }
    undo_pending(editor);
    editor->stroking = false;
    kmaskedit_damage_all(editor);
}

/* --------------------------------- edits -------------------------------- */

static void fill_every_cell(kmaskedit *editor, uint8_t value, bool only_region,
                            uint8_t match)
{
    bool changed = false;

    if (editor == NULL) {
        return;
    }
    kmaskedit_cancel(editor);
    undo_begin(editor);
    for (int cy = 0; cy < editor->grid_height; cy++) {
        for (int cx = 0; cx < editor->grid_width; cx++) {
            if (only_region && kmask_get(editor->mask, cx, cy) != match) {
                continue;
            }
            changed |= set_cell(editor, cx, cy, value);
        }
    }
    undo_end(editor);
    if (changed) {
        kmaskedit_damage_all(editor);
    }
}

void kmaskedit_clear_region(kmaskedit *editor, uint8_t region)
{
    fill_every_cell(editor, 0u, true, region);
}

void kmaskedit_fill_all(kmaskedit *editor, uint8_t region)
{
    fill_every_cell(editor, region, false, 0u);
}

bool kmaskedit_can_undo(const kmaskedit *editor)
{
    return editor != NULL && editor->op_cursor > 0u;
}

bool kmaskedit_can_redo(const kmaskedit *editor)
{
    return editor != NULL && editor->op_cursor < editor->op_count;
}

bool kmaskedit_undo(kmaskedit *editor)
{
    const undo_op *op;

    if (!kmaskedit_can_undo(editor)) {
        return false;
    }
    kmaskedit_cancel(editor);
    editor->op_cursor--;
    op = &editor->ops[editor->op_cursor];
    for (size_t i = op->count; i > 0u; i--) {
        const undo_cell *entry = &op->cells[i - 1u];

        apply_cell(editor,
                   (int)(entry->index % (uint32_t)editor->grid_width),
                   (int)(entry->index / (uint32_t)editor->grid_width),
                   entry->before);
    }
    kmaskedit_damage_all(editor);
    return true;
}

bool kmaskedit_redo(kmaskedit *editor)
{
    const undo_op *op;

    if (!kmaskedit_can_redo(editor)) {
        return false;
    }
    kmaskedit_cancel(editor);
    op = &editor->ops[editor->op_cursor];
    for (size_t i = 0u; i < op->count; i++) {
        const undo_cell *entry = &op->cells[i];

        apply_cell(editor,
                   (int)(entry->index % (uint32_t)editor->grid_width),
                   (int)(entry->index / (uint32_t)editor->grid_width),
                   entry->after);
    }
    editor->op_cursor++;
    kmaskedit_damage_all(editor);
    return true;
}

bool kmaskedit_modified(const kmaskedit *editor)
{
    if (editor == NULL) {
        return false;
    }
    if (!editor->saved_known) {
        return true;
    }
    return editor->op_cursor != editor->saved_cursor;
}

void kmaskedit_mark_saved(kmaskedit *editor)
{
    if (editor != NULL) {
        editor->saved_cursor = editor->op_cursor;
        editor->saved_known = true;
    }
}

uint64_t kmaskedit_revision(const kmaskedit *editor)
{
    return editor != NULL ? editor->revision : 0u;
}

/* ------------------------------ composition ----------------------------- */

static uint32_t blend_over(uint32_t base, uint32_t tint, float alpha)
{
    const unsigned weight = (unsigned)(alpha * 256.0f + 0.5f);
    const unsigned inverse = 256u - (weight > 256u ? 256u : weight);
    const unsigned w = weight > 256u ? 256u : weight;
    const unsigned r = (((base >> 16) & 0xFFu) * inverse +
                        ((tint >> 16) & 0xFFu) * w) >> 8;
    const unsigned g = (((base >> 8) & 0xFFu) * inverse +
                        ((tint >> 8) & 0xFFu) * w) >> 8;
    const unsigned b = ((base & 0xFFu) * inverse + (tint & 0xFFu) * w) >> 8;

    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void kmaskedit_compose(
    kmaskedit *editor, sr_canvas *out, int origin_x, int origin_y)
{
    int dst_x0;
    int dst_y0;
    int dst_x1;
    int dst_y1;
    int save_clip[4];
    uint32_t palette[KMASK_REGION_MAX + 1];

    if (editor == NULL || out == NULL || out->px == NULL ||
        !editor->view_set) {
        return;
    }
    /* The intersection of the view, the canvas and whatever the caller
     * had already clipped to. */
    dst_x0 = origin_x > out->clip_x0 ? origin_x : out->clip_x0;
    dst_y0 = origin_y > out->clip_y0 ? origin_y : out->clip_y0;
    dst_x1 = origin_x + editor->view_width;
    dst_y1 = origin_y + editor->view_height;
    dst_x1 = dst_x1 < out->clip_x1 ? dst_x1 : out->clip_x1;
    dst_y1 = dst_y1 < out->clip_y1 ? dst_y1 : out->clip_y1;
    if (dst_x1 <= dst_x0 || dst_y1 <= dst_y0) {
        return;
    }
    for (size_t region = 0u; region <= (size_t)KMASK_REGION_MAX; region++) {
        palette[region] = kmask_region_color(editor->mask, (uint8_t)region);
    }

    /*
     * One pass for the image and its tints.  Written out rather than
     * assembled from soft-raster calls because it has to sample through
     * col_src/row_src - the same tables the hit test reads - and a
     * general-purpose scaled blit would use its own arithmetic.
     */
    for (int dy = dst_y0; dy < dst_y1; dy++) {
        const int source_y = editor->row_src[dy - origin_y];
        uint32_t *row = out->px + (size_t)dy * (size_t)out->w;
        const bool row_inside =
            source_y >= 0 && source_y < editor->source_height;
        const int cy = row_inside ? source_y / editor->cell : 0;

        for (int dx = dst_x0; dx < dst_x1; dx++) {
            const int source_x = editor->col_src[dx - origin_x];
            uint32_t colour;
            uint8_t region;

            if (!row_inside || source_x < 0 ||
                source_x >= editor->source_width) {
                row[dx] = 0xFF000000u | SURROUND_RGB;
                continue;
            }
            if (editor->background != NULL) {
                colour = editor->background
                             ->px[(size_t)source_y *
                                      (size_t)editor->background->w +
                                  (size_t)source_x];
            } else {
                colour = ((source_x / CHECKER_SIZE + source_y / CHECKER_SIZE) &
                          1) != 0
                             ? CHECKER_B
                             : CHECKER_A;
            }
            region = kmask_get(editor->mask, source_x / editor->cell, cy);
            row[dx] = region == 0u
                          ? (0xFF000000u | (colour & 0x00FFFFFFu))
                          : blend_over(colour, palette[region],
                                       editor->overlay_alpha);
        }
    }

    save_clip[0] = out->clip_x0;
    save_clip[1] = out->clip_y0;
    save_clip[2] = out->clip_x1;
    save_clip[3] = out->clip_y1;
    sr_canvas_set_clip(out, dst_x0, dst_y0, dst_x1 - dst_x0, dst_y1 - dst_y0);

    if (editor->grid) {
        const float step = (float)editor->cell * editor->scale;

        if (step >= (float)GRID_MIN_CELL_PIXELS) {
            for (int cx = 0; cx <= editor->grid_width; cx++) {
                const float x = (float)origin_x + editor->origin_x +
                                (float)cx * step;

                sr_fill_rect(out, x, (float)dst_y0, 1.0f,
                             (float)(dst_y1 - dst_y0), GRID_RGB, GRID_ALPHA);
            }
            for (int cy = 0; cy <= editor->grid_height; cy++) {
                const float y = (float)origin_y + editor->origin_y +
                                (float)cy * step;

                sr_fill_rect(out, (float)dst_x0, y, (float)(dst_x1 - dst_x0),
                             1.0f, GRID_RGB, GRID_ALPHA);
            }
        }
    }

    if (editor->rect_active) {
        const int x0 = editor->rect_from_x < editor->rect_to_x
                           ? editor->rect_from_x : editor->rect_to_x;
        const int y0 = editor->rect_from_y < editor->rect_to_y
                           ? editor->rect_from_y : editor->rect_to_y;
        const int x1 = editor->rect_from_x > editor->rect_to_x
                           ? editor->rect_from_x : editor->rect_to_x;
        const int y1 = editor->rect_from_y > editor->rect_to_y
                           ? editor->rect_from_y : editor->rect_to_y;
        const float px0 = (float)origin_x + editor->origin_x +
                          (float)x0 * editor->scale;
        const float py0 = (float)origin_y + editor->origin_y +
                          (float)y0 * editor->scale;
        const float px1 = (float)origin_x + editor->origin_x +
                          (float)(x1 + 1) * editor->scale;
        const float py1 = (float)origin_y + editor->origin_y +
                          (float)(y1 + 1) * editor->scale;

        sr_stroke_rect(out, px0, py0, px1 - px0, py1 - py0, 1.0f, PREVIEW_RGB,
                       0.9f);
    }

    if (editor->hover_valid) {
        const int half = editor->brush / 2;
        const float step = (float)editor->cell * editor->scale;
        const float px0 = (float)origin_x + editor->origin_x +
                          (float)(editor->hover_cx - half) * step;
        const float py0 = (float)origin_y + editor->origin_y +
                          (float)(editor->hover_cy - half) * step;
        const float size = (float)editor->brush * step;

        /* A drawn cursor, because the terminal hides the system pointer
         * over a graphics placement.  Black under white so it stays
         * visible over both a bright sky and a dark doorway. */
        sr_stroke_rect(out, px0 - 1.0f, py0 - 1.0f, size + 2.0f, size + 2.0f,
                       1.0f, 0x00000000u, 0.7f);
        sr_stroke_rect(out, px0, py0, size, size, 1.0f, CURSOR_RGB, 0.9f);
    }

    out->clip_x0 = save_clip[0];
    out->clip_y0 = save_clip[1];
    out->clip_x1 = save_clip[2];
    out->clip_y1 = save_clip[3];
}
