#ifndef KMASK_UI_H
#define KMASK_UI_H

/*
 * The chrome around the editing view: a status strip and a help panel.
 *
 * Separate from the interactive loop because --render draws exactly the
 * same thing without a terminal, and a picture of the tool that does not
 * match the tool is worse than no picture.
 */

#include "kilix_mask_edit.h"

/* Two lines of the 8x16 face, with breathing room. */
#define KMASK_UI_STATUS_HEIGHT 40

/*
 * What the strip reports that the editor does not already know.
 *
 * A struct rather than more parameters: the strip is the one place every
 * piece of session state surfaces, and a growing positional list is how
 * two of them end up swapped.
 */
typedef struct kmask_ui_state {
    const char *path;
    /* The transient line - what was just saved, what went wrong.  NULL or
     * empty shows the key hint instead. */
    const char *message;

    /* Rectangles the active region's decomposition would produce.  A
     * consumer with a fixed budget - land-desktop caps a room at 64
     * obstacles and refuses to save past it - needs this before saving,
     * not as a rejection afterwards. */
    size_t rect_count;
    bool rect_known;
    /* The map moved since that count was taken.  Shown rather than
     * hidden, because a number quietly describing an older shape is
     * worse than one openly marked out of date. */
    bool rect_stale;
    /* 0 when no budget was configured, in which case the count is
     * reported without a verdict on it. */
    int rect_cap;
} kmask_ui_state;

/*
 * Draw the status strip across the bottom `KMASK_UI_STATUS_HEIGHT` pixels
 * of a canvas `width` wide, starting at `y`.
 */
void kmask_ui_status(
    sr_canvas *out,
    int y,
    int width,
    const kmaskedit *editor,
    const kmask_ui_state *state);

/* Centred key reference over the view. */
void kmask_ui_help(sr_canvas *out, int width, int height);

/*
 * Draw a marker line for every region carrying a `baseline` attribute,
 * the active region's brightly and labelled, the rest dim.
 *
 * Here rather than in kmaskedit_compose() on purpose.  Region attributes
 * are free-form strings precisely so the editor need not know what any of
 * them mean; teaching it that "baseline" is a horizontal line in a
 * walk-behind mask would put one consumer's vocabulary into the header
 * every other consumer includes.  A mask with no baselines draws nothing,
 * so this costs those consumers a loop over 255 empty regions and no
 * more.
 */
void kmask_ui_baselines(
    sr_canvas *out, const kmaskedit *editor, int width, int view_height);

#endif /* KMASK_UI_H */
