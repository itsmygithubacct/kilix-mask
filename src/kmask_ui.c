#include "kmask_ui.h"

#include <stdio.h>
#include <string.h>

#define BAR_RGB 0x00181820u
#define BAR_EDGE_RGB 0x00303040u
#define TEXT_RGB 0x00C8C8D4u
#define DIM_RGB 0x00808894u
#define MARK_RGB 0x00FFB020u
#define PANEL_RGB 0x00101018u
#define PANEL_EDGE_RGB 0x00505068u

static const char *tool_name(kmaskedit_tool tool)
{
    switch (tool) {
    case KMASKEDIT_TOOL_BRUSH: return "brush";
    case KMASKEDIT_TOOL_RECT:  return "rect";
    case KMASKEDIT_TOOL_WAND:  return "wand";
    case KMASKEDIT_TOOL_PICK:  return "pick";
    default:                   return "?";
    }
}

/* The file name alone.  A full path is usually longer than the strip and
 * its interesting end is the one that would be cut off. */
static const char *base_name(const char *path)
{
    const char *slash;

    if (path == NULL) {
        return "(unsaved)";
    }
    slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

void kmask_ui_status(
    sr_canvas *out,
    int y,
    int width,
    const kmaskedit *editor,
    const char *path,
    const char *message)
{
    const kmask *mask = kmaskedit_mask(editor);
    const uint8_t region = kmaskedit_get_region(editor);
    const char *name = mask != NULL ? kmask_region_name(mask, region) : NULL;
    char line[192];
    int cursor_x = 8;
    int cx = 0;
    int cy = 0;

    if (out == NULL || editor == NULL) {
        return;
    }
    sr_fill_rect(out, 0.0f, (float)y, (float)width,
                 (float)KMASK_UI_STATUS_HEIGHT, BAR_RGB, 1.0f);
    sr_fill_rect(out, 0.0f, (float)y, (float)width, 1.0f, BAR_EDGE_RGB, 1.0f);

    /* A swatch, because a region number is not a colour and the colour is
     * what is actually on screen. */
    sr_fill_rect(out, (float)cursor_x, (float)(y + 9), 12.0f, 12.0f,
                 mask != NULL ? kmask_region_color(mask, region) : 0x00808080u,
                 1.0f);
    cursor_x += 18;

    (void)snprintf(line, sizeof(line), "%s  region %u%s%s  size %d  %.2fx",
                   tool_name(kmaskedit_get_tool(editor)), (unsigned)region,
                   (name != NULL && name[0] != '\0') ? " " : "",
                   (name != NULL && name[0] != '\0') ? name : "",
                   kmaskedit_get_brush(editor),
                   (double)kmaskedit_scale(editor));
    sr_text(out, (float)cursor_x, (float)(y + 6), line, TEXT_RGB, 1.0f, 1);
    cursor_x += sr_text_width(line, 1) + 16;

    if (kmaskedit_get_tool(editor) == KMASKEDIT_TOOL_WAND) {
        (void)snprintf(line, sizeof(line), "tol %d",
                       kmaskedit_get_wand_tolerance(editor));
        sr_text(out, (float)cursor_x, (float)(y + 6), line, DIM_RGB, 1.0f, 1);
        cursor_x += sr_text_width(line, 1) + 16;
    }
    if (kmaskedit_hover_cell(editor, &cx, &cy)) {
        (void)snprintf(line, sizeof(line), "%d,%d", cx, cy);
        sr_text(out, (float)cursor_x, (float)(y + 6), line, DIM_RGB, 1.0f, 1);
    }

    (void)snprintf(line, sizeof(line), "%s%s", base_name(path),
                   kmaskedit_modified(editor) ? " *" : "");
    sr_text(out, (float)(width - sr_text_width(line, 1) - 8), (float)(y + 6),
            line, kmaskedit_modified(editor) ? MARK_RGB : DIM_RGB, 1.0f, 1);

    sr_text(out, 8.0f, (float)(y + 22),
            message != NULL && message[0] != '\0'
                ? message
                : "? keys   s save   u undo   1-9 region   b r w p tools"
                  "   q quit",
            message != NULL && message[0] != '\0' ? TEXT_RGB : DIM_RGB, 1.0f,
            1);
}

void kmask_ui_help(sr_canvas *out, int width, int height)
{
    static const char *const lines[] = {
        "drag            paint; right drag erases",
        "wheel           zoom at the pointer",
        "1-9             active region",
        "b r w p         brush, rectangle, wand, pick",
        "[ ]             brush size          , .  wand tolerance",
        "arrows / hjkl   pan                 + -  zoom",
        "f               fit to view         g    grid",
        "u / R           undo / redo         c    clear region",
        "s               save                esc  cancel a stroke",
        "q               quit (twice if unsaved)"
    };
    const size_t count = sizeof(lines) / sizeof(lines[0]);
    const int line_height = 18;
    const int panel_h = (int)count * line_height + 28;
    int panel_w = 0;
    int x;
    int y;

    if (out == NULL) {
        return;
    }
    for (size_t i = 0u; i < count; i++) {
        const int line_w = sr_text_width(lines[i], 1);

        if (line_w > panel_w) {
            panel_w = line_w;
        }
    }
    panel_w += 32;
    x = (width - panel_w) / 2;
    y = (height - panel_h) / 2;
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }

    sr_fill_rect(out, (float)x, (float)y, (float)panel_w, (float)panel_h,
                 PANEL_RGB, 0.92f);
    sr_stroke_rect(out, (float)x, (float)y, (float)panel_w, (float)panel_h,
                   1.0f, PANEL_EDGE_RGB, 1.0f);
    for (size_t i = 0u; i < count; i++) {
        sr_text(out, (float)(x + 16),
                (float)(y + 14 + (int)i * line_height), lines[i], TEXT_RGB,
                1.0f, 1);
    }
}
