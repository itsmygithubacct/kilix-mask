#include "kilix_mask_edit.h"

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

#define SOURCE_W 61
#define SOURCE_H 43

/* Kept in step with UNDO_OPS_MAX by hand: the depth is an implementation
 * choice rather than part of the contract, but a test that read it from
 * the implementation could not notice it silently changing. */
#define UNDO_OPS_MAX_EXPECTED 128u
#define VIEW_W 200
#define VIEW_H 140

static unsigned seed = 12345u;

static unsigned next_random(void)
{
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
}

/* A background where no two pixels share a colour, so a composed pixel
 * names the source pixel it came from. */
static bool make_identity_background(sr_canvas *canvas, int w, int h)
{
    if (!sr_canvas_init(canvas, w, h)) {
        return false;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            canvas->px[(size_t)y * (size_t)w + (size_t)x] =
                0xFF000000u | (uint32_t)(y * w + x + 1);
        }
    }
    return true;
}

/*
 * The property the whole viewport exists to get right: the cell the
 * pointer reports is the cell that was drawn under it.  Checked by
 * composing an image whose pixels are all distinguishable and reading the
 * result back at every view pixel.
 *
 * A tool that fails this paints one cell away from the cursor at some zoom
 * levels and not others, which is close to impossible to diagnose from a
 * screenshot.
 */
static bool
test_pointer_matches_picture(void)
{
    static const float scales[] = {0.0f, 1.0f, 2.0f, 4.0f, 0.5f};
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    sr_canvas background;
    sr_canvas frame;

    CHECK(make_identity_background(&background, SOURCE_W, SOURCE_H));
    CHECK(kmask_create(&mask, SOURCE_W, SOURCE_H, 3));
    CHECK(kmaskedit_create(&editor, mask, &background));
    CHECK(kmaskedit_set_view(editor, VIEW_W, VIEW_H));
    kmaskedit_set_grid(editor, false);
    CHECK(sr_canvas_init(&frame, VIEW_W, VIEW_H));

    for (size_t i = 0u; i < sizeof(scales) / sizeof(scales[0]); i++) {
        if (scales[i] == 0.0f) {
            kmaskedit_fit(editor);
        } else {
            kmaskedit_zoom(editor, 0, 0, 0);
            kmaskedit_fit(editor);
            while (kmaskedit_scale(editor) < scales[i]) {
                kmaskedit_zoom(editor, 1, VIEW_W / 3, VIEW_H / 3);
            }
        }
        /* Off-centre, so a mapping that only works at the origin fails. */
        kmaskedit_pan(editor, -17, 9);
        kmaskedit_compose(editor, &frame, 0, 0);

        for (int vy = 0; vy < VIEW_H; vy++) {
            for (int vx = 0; vx < VIEW_W; vx++) {
                const uint32_t drawn =
                    frame.px[(size_t)vy * VIEW_W + (size_t)vx] & 0x00FFFFFFu;
                int sx = 0;
                int sy = 0;

                if (!kmaskedit_to_source(editor, vx, vy, &sx, &sy)) {
                    /* Outside the image: whatever the surround is, it must
                     * not be a pixel of the picture. */
                    CHECK(drawn == 0u || drawn > (uint32_t)(SOURCE_W * SOURCE_H));
                    continue;
                }
                if (drawn != (uint32_t)(sy * SOURCE_W + sx + 1)) {
                    (void)fprintf(stderr,
                                  "  scale %.3f view %d,%d: drew %u, "
                                  "pointer says source %d,%d (%u)\n",
                                  (double)kmaskedit_scale(editor), vx, vy,
                                  drawn, sx, sy,
                                  (unsigned)(sy * SOURCE_W + sx + 1));
                    return false;
                }
            }
        }
    }
    sr_canvas_free(&frame);
    kmaskedit_free(editor);
    kmask_free(mask);
    sr_canvas_free(&background);
    return true;
}

/*
 * The same agreement, for the paint: at full overlay strength a painted
 * cell composes to exactly its region's colour, so every view pixel can be
 * predicted from kmask_get_at() at the source pixel the pointer maps it
 * to.  This nails the compositor's region lookup to the public one - any
 * shortcut it takes through the grid has to land on the same cell.
 */
static bool
test_paint_matches_the_map(void)
{
    static const float scales[] = {0.0f, 1.0f, 2.0f, 0.5f};
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    sr_canvas background;
    sr_canvas frame;

    CHECK(make_identity_background(&background, SOURCE_W, SOURCE_H));
    CHECK(kmask_create(&mask, SOURCE_W, SOURCE_H, 3));
    kmask_fill_rect(mask, 4, 4, 30, 22, 1u);
    kmask_fill_rect(mask, 20, 15, 55, 40, 2u);
    kmask_fill_rect(mask, 40, 2, 58, 12, 7u);
    /* Diagonal single cells, so region edges land mid-cell-row often. */
    for (int i = 0; i < 12; i++) {
        kmask_set(mask, i, i, 3u);
    }
    kmask_region_set_color(mask, 1u, 0x102030u);
    kmask_region_set_color(mask, 2u, 0x405060u);
    kmask_region_set_color(mask, 3u, 0x708090u);
    kmask_region_set_color(mask, 7u, 0xA0B0C0u);

    CHECK(kmaskedit_create(&editor, mask, &background));
    CHECK(kmaskedit_set_view(editor, VIEW_W, VIEW_H));
    kmaskedit_set_grid(editor, false);
    /* Full strength: a painted pixel is the palette colour, nothing mixed
     * in, so the expectation needs no blending arithmetic. */
    kmaskedit_set_overlay_alpha(editor, 1.0f);
    CHECK(sr_canvas_init(&frame, VIEW_W, VIEW_H));

    for (size_t i = 0u; i < sizeof(scales) / sizeof(scales[0]); i++) {
        if (scales[i] == 0.0f) {
            kmaskedit_fit(editor);
        } else {
            kmaskedit_fit(editor);
            while (kmaskedit_scale(editor) < scales[i]) {
                kmaskedit_zoom(editor, 1, VIEW_W / 3, VIEW_H / 3);
            }
        }
        kmaskedit_pan(editor, -13, 7);
        kmaskedit_compose(editor, &frame, 0, 0);

        for (int vy = 0; vy < VIEW_H; vy++) {
            for (int vx = 0; vx < VIEW_W; vx++) {
                const uint32_t drawn =
                    frame.px[(size_t)vy * VIEW_W + (size_t)vx];
                int sx = 0;
                int sy = 0;
                uint8_t region;
                uint32_t expected;

                if (!kmaskedit_to_source(editor, vx, vy, &sx, &sy)) {
                    continue;
                }
                region = kmask_get_at(mask, sx, sy);
                expected = 0xFF000000u |
                           (region == 0u
                                ? (background.px[(size_t)sy * SOURCE_W +
                                                 (size_t)sx] & 0x00FFFFFFu)
                                : kmask_region_color(mask, region));
                if (drawn != expected) {
                    (void)fprintf(stderr,
                                  "  scale %.3f view %d,%d: drew %08x, "
                                  "region %u at source %d,%d says %08x\n",
                                  (double)kmaskedit_scale(editor), vx, vy,
                                  drawn, region, sx, sy, expected);
                    return false;
                }
            }
        }
    }
    sr_canvas_free(&frame);
    kmaskedit_free(editor);
    kmask_free(mask);
    sr_canvas_free(&background);
    return true;
}

/*
 * Composing under a clip must paint exactly the pixels a full compose
 * would have put there, and no others.  That is what lets a caller
 * repaint only what changed and trust the rest of the frame to still be
 * right - a clipped compose that disagreed with the full one would smear
 * seams along every damage boundary.
 */
static bool
test_a_clipped_compose_matches_the_full_one(void)
{
    static const kmaskedit_rect clips[] = {
        {30, 20, 78, 68},     /* an interior square, cursor sized */
        {0, 0, 25, 17},       /* against the corner */
        {150, 100, 200, 140}, /* against the far edge */
        {0, 60, 200, 75},     /* a full-width band */
        {90, 0, 105, 140}     /* a full-height band */
    };
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    sr_canvas background;
    sr_canvas full;
    sr_canvas clipped;

    CHECK(make_identity_background(&background, SOURCE_W, SOURCE_H));
    CHECK(kmask_create(&mask, SOURCE_W, SOURCE_H, 3));
    kmask_fill_rect(mask, 6, 6, 40, 30, 1u);
    kmask_fill_rect(mask, 25, 20, 58, 41, 5u);
    CHECK(kmaskedit_create(&editor, mask, &background));
    CHECK(kmaskedit_set_view(editor, VIEW_W, VIEW_H));
    CHECK(sr_canvas_init(&full, VIEW_W, VIEW_H));
    CHECK(sr_canvas_init(&clipped, VIEW_W, VIEW_H));

    /* Everything the compositor can draw is on: grid, blended paint, the
     * cursor, and a rectangle preview mid-drag. */
    kmaskedit_zoom(editor, 1, VIEW_W / 2, VIEW_H / 2);
    kmaskedit_set_tool(editor, KMASKEDIT_TOOL_RECT);
    kmaskedit_press(editor, 40, 30, KMASKEDIT_BUTTON_PAINT);
    kmaskedit_drag(editor, 120, 90);
    kmaskedit_hover(editor, 120, 90);

    kmaskedit_compose(editor, &full, 0, 0);

    for (size_t i = 0u; i < sizeof(clips) / sizeof(clips[0]); i++) {
        const kmaskedit_rect clip = clips[i];

        for (int p = 0; p < VIEW_W * VIEW_H; p++) {
            clipped.px[p] = 0xFFABCDEFu;
        }
        sr_canvas_set_clip(&clipped, clip.x0, clip.y0, clip.x1 - clip.x0,
                           clip.y1 - clip.y0);
        kmaskedit_compose(editor, &clipped, 0, 0);
        sr_canvas_reset_clip(&clipped);

        for (int vy = 0; vy < VIEW_H; vy++) {
            for (int vx = 0; vx < VIEW_W; vx++) {
                const size_t at = (size_t)vy * VIEW_W + (size_t)vx;
                const bool inside = vx >= clip.x0 && vx < clip.x1 &&
                                    vy >= clip.y0 && vy < clip.y1;
                const uint32_t want =
                    inside ? full.px[at] : 0xFFABCDEFu;

                if (clipped.px[at] != want) {
                    (void)fprintf(stderr,
                                  "  clip %zu pixel %d,%d (%s): %08x, "
                                  "the full compose has %08x\n",
                                  i, vx, vy, inside ? "inside" : "outside",
                                  clipped.px[at], want);
                    return false;
                }
            }
        }
    }
    kmaskedit_cancel(editor);
    sr_canvas_free(&clipped);
    sr_canvas_free(&full);
    kmaskedit_free(editor);
    kmask_free(mask);
    sr_canvas_free(&background);
    return true;
}

static bool covered(const kmaskedit_rect *rects, size_t count, int x, int y)
{
    for (size_t i = 0u; i < count; i++) {
        if (x >= rects[i].x0 && x < rects[i].x1 && y >= rects[i].y0 &&
            y < rects[i].y1) {
            return true;
        }
    }
    return false;
}

/* Compose, act, compose again: every pixel that moved must lie inside a
 * reported rectangle.  Damage that misses a pixel leaves it stale on
 * screen until something unrelated happens to repaint it. */
static bool check_damage(kmaskedit *editor, sr_canvas *before,
                         sr_canvas *after, int origin_x, int origin_y,
                         const char *what)
{
    kmaskedit_rect rects[KMASKEDIT_DAMAGE_MAX_RECTS];
    const size_t count =
        kmaskedit_take_damage(editor, rects, KMASKEDIT_DAMAGE_MAX_RECTS);

    for (int y = 0; y < before->h; y++) {
        for (int x = 0; x < before->w; x++) {
            const size_t index = (size_t)y * (size_t)before->w + (size_t)x;

            if (before->px[index] == after->px[index]) {
                continue;
            }
            if (!covered(rects, count, x - origin_x, y - origin_y)) {
                (void)fprintf(stderr,
                              "  %s: pixel %d,%d changed but is outside "
                              "all %zu damage rects\n",
                              what, x, y, count);
                return false;
            }
        }
    }
    return true;
}

/*
 * The same check over a long random session, because the damage bugs that
 * survive are the ones in combinations nobody thought to try - a zoom
 * during a stroke, an undo of a fill that ran off the edge.
 */
static bool
test_damage_never_misses_a_pixel(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    sr_canvas background;
    sr_canvas before;
    sr_canvas after;
    const int origin_x = 5;
    const int origin_y = 3;
    bool ok = true;

    CHECK(make_identity_background(&background, SOURCE_W, SOURCE_H));
    CHECK(kmask_create(&mask, SOURCE_W, SOURCE_H, 3));
    CHECK(kmaskedit_create(&editor, mask, &background));
    CHECK(kmaskedit_set_view(editor, VIEW_W, VIEW_H));
    CHECK(sr_canvas_init(&before, VIEW_W + origin_x + 7,
                         VIEW_H + origin_y + 4));
    CHECK(sr_canvas_init(&after, before.w, before.h));

    for (int step = 0; step < 400 && ok; step++) {
        const unsigned action = next_random() % 11u;
        const int vx = (int)(next_random() % (unsigned)VIEW_W);
        const int vy = (int)(next_random() % (unsigned)VIEW_H);
        char label[64];

        kmaskedit_compose(editor, &before, origin_x, origin_y);
        (void)kmaskedit_take_damage(editor, NULL, 0u);
        {
            kmaskedit_rect drain[KMASKEDIT_DAMAGE_MAX_RECTS];

            (void)kmaskedit_take_damage(editor, drain,
                                        KMASKEDIT_DAMAGE_MAX_RECTS);
        }
        (void)snprintf(label, sizeof(label), "step %d action %u", step,
                       action);

        switch (action) {
        case 0:
        case 1:
            kmaskedit_hover(editor, vx, vy);
            break;
        case 2:
            kmaskedit_press(editor, vx, vy, KMASKEDIT_BUTTON_PAINT);
            break;
        case 3:
            kmaskedit_drag(editor, vx, vy);
            break;
        case 4:
            kmaskedit_release(editor, vx, vy);
            break;
        case 5:
            kmaskedit_press(editor, vx, vy, KMASKEDIT_BUTTON_ERASE);
            break;
        case 6:
            kmaskedit_zoom(editor, (next_random() & 1u) != 0u ? 1 : -1, vx,
                           vy);
            break;
        case 7:
            kmaskedit_pan(editor, (int)(next_random() % 40u) - 20,
                          (int)(next_random() % 40u) - 20);
            break;
        case 8:
            kmaskedit_set_brush(editor, 1 + (int)(next_random() % 7u));
            break;
        case 9:
            kmaskedit_set_tool(editor,
                               (kmaskedit_tool)(next_random() % 4u));
            break;
        default:
            if ((next_random() & 1u) != 0u) {
                (void)kmaskedit_undo(editor);
            } else {
                (void)kmaskedit_redo(editor);
            }
            break;
        }
        kmaskedit_set_region(editor, (uint8_t)(1u + next_random() % 3u));

        kmaskedit_compose(editor, &after, origin_x, origin_y);
        ok = check_damage(editor, &before, &after, origin_x, origin_y, label);
    }
    sr_canvas_free(&after);
    sr_canvas_free(&before);
    kmaskedit_free(editor);
    kmask_free(mask);
    sr_canvas_free(&background);
    return ok;
}

/* Nothing outside the view is written, so a caller can keep a status bar
 * in the same canvas and compose the editor at an offset. */
static bool
test_compose_stays_inside_its_view(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    sr_canvas frame;
    const int origin_x = 6;
    const int origin_y = 4;
    const int view_w = 40;
    const int view_h = 30;

    CHECK(kmask_create(&mask, SOURCE_W, SOURCE_H, 3));
    CHECK(kmaskedit_create(&editor, mask, NULL));
    CHECK(kmaskedit_set_view(editor, view_w, view_h));
    CHECK(sr_canvas_init(&frame, 60, 50));
    for (int i = 0; i < frame.w * frame.h; i++) {
        frame.px[i] = 0xFFABCDEFu;
    }
    kmaskedit_hover(editor, 0, 0);
    kmaskedit_compose(editor, &frame, origin_x, origin_y);

    for (int y = 0; y < frame.h; y++) {
        for (int x = 0; x < frame.w; x++) {
            const bool inside = x >= origin_x && x < origin_x + view_w &&
                                y >= origin_y && y < origin_y + view_h;

            if (!inside &&
                frame.px[(size_t)y * (size_t)frame.w + (size_t)x] !=
                    0xFFABCDEFu) {
                (void)fprintf(stderr, "  wrote outside the view at %d,%d\n", x,
                              y);
                return false;
            }
        }
    }

    /* And the caller's clip is honoured, then handed back untouched. */
    for (int i = 0; i < frame.w * frame.h; i++) {
        frame.px[i] = 0xFFABCDEFu;
    }
    sr_canvas_set_clip(&frame, 0, 0, 10, 10);
    kmaskedit_compose(editor, &frame, origin_x, origin_y);
    /* Inside both the clip and the view, so drawn... */
    CHECK(frame.px[(size_t)5 * (size_t)frame.w + 7u] != 0xFFABCDEFu);
    /* ...and inside the view but outside the clip, so not. */
    CHECK(frame.px[(size_t)20 * (size_t)frame.w + 20u] == 0xFFABCDEFu);
    CHECK(frame.clip_x0 == 0 && frame.clip_y0 == 0);
    CHECK(frame.clip_x1 == 10 && frame.clip_y1 == 10);

    sr_canvas_free(&frame);
    kmaskedit_free(editor);
    kmask_free(mask);
    return true;
}

/* Undo has to restore the map exactly, not approximately: a tool whose
 * undo is lossy corrupts a mask a little on every mistake. */
static bool
test_undo_restores_exactly(void)
{
    kmask *mask = NULL;
    kmask *reference = NULL;
    kmaskedit *editor = NULL;

    CHECK(kmask_create(&mask, SOURCE_W, SOURCE_H, 3));
    CHECK(kmask_create(&reference, SOURCE_W, SOURCE_H, 3));
    CHECK(kmaskedit_create(&editor, mask, NULL));
    CHECK(kmaskedit_set_view(editor, VIEW_W, VIEW_H));

    /* A starting map that is not empty, so "undo cleared everything" is
     * not mistaken for "undo restored the map". */
    kmask_fill_rect(mask, 10, 10, 40, 30, 2u);
    kmask_fill_rect(reference, 10, 10, 40, 30, 2u);
    CHECK(!kmaskedit_can_undo(editor));

    for (int stroke = 0; stroke < 24; stroke++) {
        const int x0 = (int)(next_random() % (unsigned)VIEW_W);
        const int y0 = (int)(next_random() % (unsigned)VIEW_H);

        kmaskedit_set_region(editor, (uint8_t)(1u + next_random() % 4u));
        kmaskedit_set_brush(editor, 1 + (int)(next_random() % 6u));
        switch (next_random() % 3u) {
        case 0:
            kmaskedit_set_tool(editor, KMASKEDIT_TOOL_RECT);
            break;
        case 1:
            kmaskedit_set_tool(editor, KMASKEDIT_TOOL_BRUSH);
            break;
        default:
            kmaskedit_clear_region(editor, (uint8_t)(1u + next_random() % 4u));
            continue;
        }
        kmaskedit_press(editor, x0, y0, KMASKEDIT_BUTTON_PAINT);
        for (int move = 0; move < 3; move++) {
            kmaskedit_drag(editor, (int)(next_random() % (unsigned)VIEW_W),
                           (int)(next_random() % (unsigned)VIEW_H));
        }
        kmaskedit_release(editor, (int)(next_random() % (unsigned)VIEW_W),
                          (int)(next_random() % (unsigned)VIEW_H));
    }

    CHECK(kmaskedit_can_undo(editor));
    while (kmaskedit_undo(editor)) {
        /* all the way back */
    }
    for (int cy = 0; cy < kmask_grid_height(mask); cy++) {
        for (int cx = 0; cx < kmask_grid_width(mask); cx++) {
            if (kmask_get(mask, cx, cy) != kmask_get(reference, cx, cy)) {
                (void)fprintf(stderr, "  cell %d,%d: %u after undo, %u before\n",
                              cx, cy, kmask_get(mask, cx, cy),
                              kmask_get(reference, cx, cy));
                return false;
            }
        }
    }

    /* And forward again reaches the same place it left. */
    {
        size_t counts_after_redo[256];
        size_t counts_before[256];

        kmask_counts(mask, counts_before);
        while (kmaskedit_redo(editor)) {
            /* all the way forward */
        }
        kmask_counts(mask, counts_after_redo);
        CHECK(memcmp(counts_before, counts_after_redo,
                     sizeof(counts_before)) != 0);
        while (kmaskedit_undo(editor)) {
            /* and back once more */
        }
        kmask_counts(mask, counts_after_redo);
        CHECK(memcmp(counts_before, counts_after_redo,
                     sizeof(counts_before)) == 0);
    }

    kmaskedit_free(editor);
    kmask_free(reference);
    kmask_free(mask);
    return true;
}

/*
 * History is bounded, so a long session cannot grow without limit.  What
 * matters is which end is discarded: the oldest, so the edits a person is
 * most likely to want back are the ones still there.
 */
static bool
test_history_drops_the_oldest(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    const int strokes = 200;
    int undone = 0;

    CHECK(kmask_create(&mask, strokes, 4, 1));
    CHECK(kmaskedit_create(&editor, mask, NULL));
    CHECK(kmaskedit_set_view(editor, strokes, 4));
    CHECK(kmaskedit_scale(editor) == 1.0f);
    kmaskedit_mark_saved(editor);

    for (int i = 0; i < strokes; i++) {
        kmaskedit_press(editor, i, 0, KMASKEDIT_BUTTON_PAINT);
        kmaskedit_release(editor, i, 0);
    }
    for (int i = 0; i < strokes; i++) {
        CHECK(kmask_get(mask, i, 0) == 1u);
    }
    while (kmaskedit_undo(editor)) {
        undone++;
    }
    CHECK(undone == (int)UNDO_OPS_MAX_EXPECTED);

    /* The recent strokes came back... */
    for (int i = strokes - undone; i < strokes; i++) {
        CHECK(kmask_get(mask, i, 0) == 0u);
    }
    /* ...and the ones whose history was discarded stayed painted. */
    for (int i = 0; i < strokes - undone; i++) {
        CHECK(kmask_get(mask, i, 0) == 1u);
    }
    /* The saved point was discarded with them, so the map can no longer
     * be claimed to match what was written to disk. */
    CHECK(kmaskedit_modified(editor));

    kmaskedit_free(editor);
    kmask_free(mask);
    return true;
}

/*
 * The revision exists for callers that cache something expensive derived
 * from the map.  What it must do that "modified" cannot is notice a
 * change that ends where it started: edit, then undo, and the map took
 * two different shapes on the way while modified is false at both ends.
 */
static bool
test_revision_tracks_the_cells(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    uint64_t start;
    uint64_t after_paint;
    uint64_t after_undo;

    CHECK(kmask_create(&mask, 40, 40, 1));
    CHECK(kmaskedit_create(&editor, mask, NULL));
    CHECK(kmaskedit_set_view(editor, 40, 40));
    start = kmaskedit_revision(editor);

    kmaskedit_press(editor, 10, 10, KMASKEDIT_BUTTON_PAINT);
    kmaskedit_release(editor, 10, 10);
    after_paint = kmaskedit_revision(editor);
    CHECK(after_paint != start);

    /* Painting the value a cell already holds changes nothing, so it must
     * not invalidate anybody's cache. */
    kmaskedit_set_region(editor, kmask_get(mask, 10, 10));
    kmaskedit_press(editor, 10, 10, KMASKEDIT_BUTTON_ERASE);
    kmaskedit_release(editor, 10, 10);
    kmaskedit_press(editor, 10, 10, KMASKEDIT_BUTTON_ERASE);
    kmaskedit_release(editor, 10, 10);
    {
        const uint64_t settled = kmaskedit_revision(editor);

        kmaskedit_press(editor, 10, 10, KMASKEDIT_BUTTON_ERASE);
        kmaskedit_release(editor, 10, 10);
        CHECK(kmaskedit_revision(editor) == settled);
    }

    /* And back to the beginning: unmodified, but the shape moved twice,
     * so anything cached against it is stale. */
    while (kmaskedit_undo(editor)) {
        /* all the way back */
    }
    after_undo = kmaskedit_revision(editor);
    CHECK(!kmaskedit_modified(editor));
    CHECK(after_undo != after_paint);
    CHECK(after_undo != start);

    CHECK(kmaskedit_revision(NULL) == 0u);
    kmaskedit_free(editor);
    kmask_free(mask);
    return true;
}

/*
 * A terminal reports pointer motion at whatever rate it manages, so a
 * quick drag arrives as two positions far apart.  Without interpolation
 * the stroke is a row of disconnected dots.
 */
static bool
test_a_fast_drag_paints_a_line(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    int painted = 0;

    CHECK(kmask_create(&mask, 120, 120, 1));
    CHECK(kmaskedit_create(&editor, mask, NULL));
    CHECK(kmaskedit_set_view(editor, 120, 120));
    CHECK(kmaskedit_scale(editor) == 1.0f);

    kmaskedit_press(editor, 10, 10, KMASKEDIT_BUTTON_PAINT);
    kmaskedit_drag(editor, 100, 100);   /* one jump, no intermediate events */
    kmaskedit_release(editor, 100, 100);

    /* Every cell on the diagonal between the two points, not just the
     * two ends. */
    for (int i = 0; i <= 90; i++) {
        if (kmask_get(mask, 10 + i, 10 + i) == 1u) {
            painted++;
        }
    }
    CHECK(painted == 91);

    kmaskedit_free(editor);
    kmask_free(mask);
    return true;
}

/*
 * The stroke decides paint-or-erase once, at press.  Deciding per cell
 * would make a drag across a region boundary turn cells on and off under
 * the pointer as it went.
 */
static bool
test_a_stroke_decides_once(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;

    CHECK(kmask_create(&mask, 60, 20, 1));
    CHECK(kmaskedit_create(&editor, mask, NULL));
    CHECK(kmaskedit_set_view(editor, 60, 20));
    kmaskedit_set_region(editor, 1u);

    /* Left half already painted; the stroke starts inside it. */
    kmask_fill_rect(mask, 0, 0, 30, 20, 1u);
    kmaskedit_press(editor, 5, 10, KMASKEDIT_BUTTON_PAINT);
    kmaskedit_drag(editor, 55, 10);
    kmaskedit_release(editor, 55, 10);

    /* Started on the region, so the whole stroke erased - including the
     * part that crossed into unpainted ground, where it changed nothing. */
    CHECK(kmask_get(mask, 5, 10) == 0u);
    CHECK(kmask_get(mask, 29, 10) == 0u);
    CHECK(kmask_get(mask, 40, 10) == 0u);
    /* And it did not paint the far half on the way. */
    CHECK(kmask_get(mask, 55, 10) == 0u);
    /* Rows it never touched are untouched. */
    CHECK(kmask_get(mask, 5, 3) == 1u);

    /* Starting off the region paints for the whole stroke instead. */
    kmaskedit_press(editor, 55, 5, KMASKEDIT_BUTTON_PAINT);
    kmaskedit_drag(editor, 5, 5);
    kmaskedit_release(editor, 5, 5);
    CHECK(kmask_get(mask, 55, 5) == 1u);
    CHECK(kmask_get(mask, 5, 5) == 1u);

    /* The erase button erases whatever it starts on. */
    kmaskedit_press(editor, 55, 5, KMASKEDIT_BUTTON_ERASE);
    kmaskedit_release(editor, 55, 5);
    CHECK(kmask_get(mask, 55, 5) == 0u);

    kmaskedit_free(editor);
    kmask_free(mask);
    return true;
}

/* Flood fill follows the picture, and stops where the picture does. */
static bool
test_wand_follows_the_image(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    sr_canvas background;

    CHECK(sr_canvas_init(&background, 80, 60));
    for (int y = 0; y < 60; y++) {
        for (int x = 0; x < 80; x++) {
            /* A red block on blue, with a slight gradient so an exact
             * match would fail and a tolerant one succeeds. */
            const bool red = x >= 20 && x < 60 && y >= 15 && y < 45;
            const uint32_t noise = (uint32_t)((x + y) % 5);

            background.px[(size_t)y * 80u + (size_t)x] =
                red ? (0xFFC00000u + noise) : (0xFF0000C0u + noise);
        }
    }
    CHECK(kmask_create(&mask, 80, 60, 1));
    CHECK(kmaskedit_create(&editor, mask, &background));
    CHECK(kmaskedit_set_view(editor, 80, 60));
    kmaskedit_set_tool(editor, KMASKEDIT_TOOL_WAND);
    kmaskedit_set_region(editor, 3u);
    kmaskedit_set_wand_tolerance(editor, 10);

    kmaskedit_press(editor, 40, 30, KMASKEDIT_BUTTON_PAINT);

    CHECK(kmask_get(mask, 40, 30) == 3u);
    CHECK(kmask_get(mask, 20, 15) == 3u);
    CHECK(kmask_get(mask, 59, 44) == 3u);
    CHECK(kmask_get(mask, 19, 15) == 0u);
    CHECK(kmask_get(mask, 60, 44) == 0u);
    CHECK(kmask_get(mask, 5, 5) == 0u);

    /* One undo entry, not one per pixel. */
    CHECK(kmaskedit_can_undo(editor));
    CHECK(kmaskedit_undo(editor));
    CHECK(kmask_get(mask, 40, 30) == 0u);
    CHECK(!kmaskedit_can_undo(editor));

    /* A tolerance that spans the whole image floods it all. */
    kmaskedit_set_wand_tolerance(editor, 441);
    kmaskedit_press(editor, 40, 30, KMASKEDIT_BUTTON_PAINT);
    CHECK(kmask_get(mask, 5, 5) == 3u);

    kmaskedit_free(editor);
    kmask_free(mask);
    sr_canvas_free(&background);
    return true;
}

static bool
test_settings_and_rejections(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    sr_canvas wrong_size;

    CHECK(kmask_create(&mask, SOURCE_W, SOURCE_H, 4));

    /* A background of the wrong size is refused rather than scaled: every
     * region would land over the wrong pixels while looking plausible. */
    CHECK(sr_canvas_init(&wrong_size, SOURCE_W + 1, SOURCE_H));
    CHECK(!kmaskedit_create(&editor, mask, &wrong_size));
    CHECK(editor == NULL);
    sr_canvas_free(&wrong_size);

    CHECK(!kmaskedit_create(&editor, NULL, NULL));
    CHECK(!kmaskedit_create(NULL, mask, NULL));

    CHECK(kmaskedit_create(&editor, mask, NULL));
    CHECK(kmaskedit_mask(editor) == mask);

    /* Composing before the view is set does nothing rather than reading
     * tables that do not exist yet. */
    kmaskedit_compose(editor, NULL, 0, 0);
    CHECK(!kmaskedit_to_source(editor, 0, 0, NULL, NULL));
    CHECK(!kmaskedit_set_view(editor, 0, 10));
    CHECK(kmaskedit_set_view(editor, VIEW_W, VIEW_H));

    /* Even brush sizes have no centre cell to sit under the pointer. */
    kmaskedit_set_brush(editor, 4);
    CHECK(kmaskedit_get_brush(editor) == 5);
    kmaskedit_set_brush(editor, 0);
    CHECK(kmaskedit_get_brush(editor) == 1);
    kmaskedit_set_brush(editor, 9999);
    CHECK(kmaskedit_get_brush(editor) == KMASKEDIT_BRUSH_MAX);

    kmaskedit_set_wand_tolerance(editor, -5);
    CHECK(kmaskedit_get_wand_tolerance(editor) == 0);
    kmaskedit_set_wand_tolerance(editor, 100000);
    CHECK(kmaskedit_get_wand_tolerance(editor) == 441);

    /* The pointer leaving is reported as a hover outside the view. */
    kmaskedit_hover(editor, 10, 10);
    CHECK(kmaskedit_hover_cell(editor, NULL, NULL));
    kmaskedit_hover(editor, -1, -1);
    CHECK(!kmaskedit_hover_cell(editor, NULL, NULL));

    /* The image is kept where it can be seen. */
    kmaskedit_fit(editor);
    kmaskedit_pan(editor, 100000, 100000);
    {
        int sx = 0;
        int sy = 0;
        bool any = false;

        for (int vy = 0; vy < VIEW_H && !any; vy++) {
            for (int vx = 0; vx < VIEW_W; vx++) {
                if (kmaskedit_to_source(editor, vx, vy, &sx, &sy)) {
                    any = true;
                    break;
                }
            }
        }
        CHECK(any);
    }

    /* Picking adopts the region under the pointer. */
    kmask_fill_rect(mask, 0, 0, SOURCE_W, SOURCE_H, 7u);
    kmaskedit_set_tool(editor, KMASKEDIT_TOOL_PICK);
    kmaskedit_set_region(editor, 1u);
    kmaskedit_press(editor, VIEW_W / 2, VIEW_H / 2, KMASKEDIT_BUTTON_PAINT);
    CHECK(kmaskedit_get_region(editor) == 7u);
    CHECK(!kmaskedit_stroking(editor));

    /* Cancelling a stroke leaves the map as it was before the press. */
    kmaskedit_set_tool(editor, KMASKEDIT_TOOL_BRUSH);
    kmaskedit_set_region(editor, 2u);
    kmaskedit_press(editor, 20, 20, KMASKEDIT_BUTTON_ERASE);
    kmaskedit_drag(editor, 60, 60);
    CHECK(kmaskedit_stroking(editor));
    kmaskedit_cancel(editor);
    CHECK(!kmaskedit_stroking(editor));
    for (int cy = 0; cy < kmask_grid_height(mask); cy++) {
        for (int cx = 0; cx < kmask_grid_width(mask); cx++) {
            CHECK(kmask_get(mask, cx, cy) == 7u);
        }
    }
    CHECK(!kmaskedit_can_undo(editor));

    /* Saved-state tracking is about edits, not about files. */
    CHECK(!kmaskedit_modified(editor));
    kmaskedit_fill_all(editor, 4u);
    CHECK(kmaskedit_modified(editor));
    kmaskedit_mark_saved(editor);
    CHECK(!kmaskedit_modified(editor));
    CHECK(kmaskedit_undo(editor));
    CHECK(kmaskedit_modified(editor));
    CHECK(kmaskedit_redo(editor));
    CHECK(!kmaskedit_modified(editor));

    /* Every accessor tolerates NULL, so a caller cleaning up after a
     * failed create needs no special case. */
    CHECK(kmaskedit_view_width(NULL) == 0);
    CHECK(kmaskedit_scale(NULL) == 0.0f);
    CHECK(!kmaskedit_undo(NULL));
    CHECK(!kmaskedit_modified(NULL));
    CHECK(kmaskedit_take_damage(NULL, NULL, 0u) == 0u);
    kmaskedit_free(NULL);

    kmaskedit_free(editor);
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
        {"the pointer and the picture agree", test_pointer_matches_picture},
        {"the paint matches the map", test_paint_matches_the_map},
        {"a clipped compose matches the full one",
         test_a_clipped_compose_matches_the_full_one},
        {"damage never misses a changed pixel",
         test_damage_never_misses_a_pixel},
        {"compose stays inside its view", test_compose_stays_inside_its_view},
        {"undo restores exactly", test_undo_restores_exactly},
        {"history drops the oldest", test_history_drops_the_oldest},
        {"revision tracks the cells", test_revision_tracks_the_cells},
        {"a fast drag paints a line", test_a_fast_drag_paints_a_line},
        {"a stroke decides paint or erase once", test_a_stroke_decides_once},
        {"the wand follows the image", test_wand_follows_the_image},
        {"settings and rejections", test_settings_and_rejections}
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
