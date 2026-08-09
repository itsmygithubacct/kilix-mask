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
 * Draw the status strip across the bottom `KMASK_UI_STATUS_HEIGHT` pixels
 * of a canvas `width` wide, starting at `y`.
 *
 * `message` is the transient line - what was just saved, what went wrong -
 * and may be NULL, in which case the key hint is shown instead.
 */
void kmask_ui_status(
    sr_canvas *out,
    int y,
    int width,
    const kmaskedit *editor,
    const char *path,
    const char *message);

/* Centred key reference over the view. */
void kmask_ui_help(sr_canvas *out, int width, int height);

#endif /* KMASK_UI_H */
