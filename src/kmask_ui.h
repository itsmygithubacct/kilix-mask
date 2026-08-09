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
