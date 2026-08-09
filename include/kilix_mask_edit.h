#ifndef KILIX_MASK_EDIT_H
#define KILIX_MASK_EDIT_H

/*
 * Painting a region map over the picture it describes.
 *
 * This is the editor without a terminal in it: a viewport, a set of tools,
 * an undo stack, and a compositor that draws the whole scene into a
 * soft-raster canvas.  Everything a person does to a mask happens through
 * this header, and none of it needs a screen.
 *
 * That split is not tidiness.  A painting tool is close to untestable once
 * its logic lives inside an event loop talking to a terminal - the bugs
 * that matter are "the cell I painted is not the cell under the pointer"
 * and "the screen kept a stale pixel", and neither is observable from
 * outside a running session.  Here both are ordinary assertions, and the
 * tests make them.
 *
 * Two properties are worth knowing about before using any of this.
 *
 * **The pointer and the picture agree by construction.**  Composition
 * samples the source image through the same lookup tables that
 * kmaskedit_to_source() reads, so the cell drawn at a view pixel is
 * always the cell that painting at that view pixel will change.  They
 * cannot drift apart, because there is only one mapping.
 *
 * **Damage is never under-reported.**  Every state change records the view
 * rectangle it could have altered, rounded outwards, and anything that
 * cannot be described in the rectangles on hand becomes the whole view.
 * Over-reporting costs a few bytes on the wire; under-reporting leaves a
 * stale pixel on screen that nothing will ever repaint.
 *
 * Dependencies: kilix-mask and soft-raster.  A consumer that only reads
 * masks links neither this nor soft-raster.
 */

#include "kilix_mask.h"
#include "soft_raster.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Odd sizes only; even ones have no centre cell to sit under the pointer. */
#define KMASKEDIT_BRUSH_MAX 15

/* Beyond this, a frame is cheaper than the patches: see
 * kmaskedit_take_damage(). */
#define KMASKEDIT_DAMAGE_MAX_RECTS 16

typedef struct kmaskedit kmaskedit;

/*
 * View-space rectangle, x1/y1 exclusive.  The same four fields in the
 * same order as kittyfb_rect, so handing damage to the framebuffer's
 * patch path is a copy and never a conversion - but a copy, not a cast:
 * a reordering in either header should be a compile error rather than
 * silently transposed rectangles.
 */
typedef struct kmaskedit_rect {
    int x0;
    int y0;
    int x1;
    int y1;
} kmaskedit_rect;

typedef enum kmaskedit_tool {
    KMASKEDIT_TOOL_BRUSH = 0, /* freehand, footprint of the brush size */
    KMASKEDIT_TOOL_RECT,      /* drag a rectangle, committed on release */
    KMASKEDIT_TOOL_WAND,      /* flood fill by colour over the background */
    KMASKEDIT_TOOL_PICK       /* adopt the region under the pointer */
} kmaskedit_tool;

typedef enum kmaskedit_button {
    KMASKEDIT_BUTTON_PAINT = 0,
    KMASKEDIT_BUTTON_ERASE = 1
} kmaskedit_button;

/* ------------------------------ lifetime -------------------------------- */

/*
 * Both the mask and the background are borrowed and must outlive the
 * editor; the caller loads and saves them.  The background may be NULL,
 * in which case the scene is drawn over a checkerboard and the wand has
 * nothing to sample and does nothing.
 *
 * A background whose size differs from the mask's source size is
 * rejected rather than scaled to fit.  Scaling would put every painted
 * region over the wrong pixels while looking entirely plausible.
 *
 * The view starts unset; call kmaskedit_set_view() before composing.
 */
bool kmaskedit_create(
    kmaskedit **editor, kmask *mask, const sr_canvas *background);
void kmaskedit_free(kmaskedit *editor);

kmask *kmaskedit_mask(const kmaskedit *editor);

/* ------------------------------ viewport -------------------------------- */

/*
 * Set the size of the composed view in pixels.  The first call fits the
 * image to it; later calls keep the current scale and re-clamp, so a
 * terminal resize does not throw away the operator's zoom.
 */
bool kmaskedit_set_view(kmaskedit *editor, int width, int height);
int kmaskedit_view_width(const kmaskedit *editor);
int kmaskedit_view_height(const kmaskedit *editor);

/* Scale so the whole image is visible, and centre it. */
void kmaskedit_fit(kmaskedit *editor);

/*
 * Zoom by whole doubling steps - positive in, negative out - keeping the
 * source pixel under (anchor_x, anchor_y) where it is.  Powers of two keep
 * source pixels square and evenly sized on screen at every level above
 * 1.0, which matters when the thing being painted is single cells.
 *
 * The image is kept from wandering out of the view: smaller than the view
 * it is centred, larger it is held so no edge comes inside.
 */
void kmaskedit_zoom(kmaskedit *editor, int steps, int anchor_x, int anchor_y);
void kmaskedit_pan(kmaskedit *editor, int dx, int dy);
float kmaskedit_scale(const kmaskedit *editor);

/*
 * View pixel to source pixel, and back.
 *
 * to_source() returns false when the view pixel is outside the view or
 * falls beyond the image, so a caller never has to bounds-check first.
 * to_view() gives the view pixel where a source pixel begins, and may
 * report a coordinate outside the view for a source pixel scrolled off it.
 */
bool kmaskedit_to_source(
    const kmaskedit *editor, int view_x, int view_y, int *x, int *y);
void kmaskedit_to_view(
    const kmaskedit *editor, int x, int y, int *view_x, int *view_y);

/* ------------------------------ settings -------------------------------- */

void kmaskedit_set_tool(kmaskedit *editor, kmaskedit_tool tool);
kmaskedit_tool kmaskedit_get_tool(const kmaskedit *editor);

/* The region painted by the paint button.  0 is allowed and erases. */
void kmaskedit_set_region(kmaskedit *editor, uint8_t region);
uint8_t kmaskedit_get_region(const kmaskedit *editor);

/* Brush footprint in cells, forced odd and clamped to 1..KMASKEDIT_BRUSH_MAX. */
void kmaskedit_set_brush(kmaskedit *editor, int cells);
int kmaskedit_get_brush(const kmaskedit *editor);

/* Cell borders, drawn only when a cell is at least a few pixels across;
 * below that the grid is solid lines and hides the picture. */
void kmaskedit_set_grid(kmaskedit *editor, bool show);
bool kmaskedit_get_grid(const kmaskedit *editor);

/* How strongly painted regions tint the image, 0..1.  Default 0.45: enough
 * to read the shape, transparent enough to see what is under it. */
void kmaskedit_set_overlay_alpha(kmaskedit *editor, float alpha);

/* Euclidean RGB distance the wand accepts, 0..441.  Default 32. */
void kmaskedit_set_wand_tolerance(kmaskedit *editor, int tolerance);
int kmaskedit_get_wand_tolerance(const kmaskedit *editor);

/* ------------------------------- pointer -------------------------------- */

/*
 * Where the pointer is, in view pixels.  Coordinates outside the view
 * clear the cursor, which is how a caller reports the pointer leaving or
 * the window losing focus - no separate call for it.
 *
 * The editor draws its own cursor because a terminal hides the system
 * pointer over a drawn image, so hover has to be tracked even when
 * nothing is being painted.
 */
void kmaskedit_hover(kmaskedit *editor, int view_x, int view_y);
bool kmaskedit_hover_cell(const kmaskedit *editor, int *cx, int *cy);

/*
 * A stroke: press, any number of drags, release.
 *
 * The paint button decides its value once, at press: if the cell under the
 * pointer already holds the active region the whole stroke erases,
 * otherwise the whole stroke paints.  Deciding per cell instead would make
 * a drag across a boundary alternate on and off under the pointer.  The
 * erase button always erases.
 *
 * Drags interpolate from the previous point, so a fast movement paints a
 * line rather than the two sampled dots the terminal happened to report.
 *
 * A stroke is one undo entry however many cells it touched.
 */
void kmaskedit_press(
    kmaskedit *editor, int view_x, int view_y, kmaskedit_button button);
void kmaskedit_drag(kmaskedit *editor, int view_x, int view_y);
void kmaskedit_release(kmaskedit *editor, int view_x, int view_y);
bool kmaskedit_stroking(const kmaskedit *editor);

/* Abandon a stroke in progress, undoing what it has painted so far. */
void kmaskedit_cancel(kmaskedit *editor);

/* --------------------------------- edits -------------------------------- */

/* Fill or clear a whole region across the image, as one undo entry. */
void kmaskedit_clear_region(kmaskedit *editor, uint8_t region);
void kmaskedit_fill_all(kmaskedit *editor, uint8_t region);

bool kmaskedit_undo(kmaskedit *editor);
bool kmaskedit_redo(kmaskedit *editor);
bool kmaskedit_can_undo(const kmaskedit *editor);
bool kmaskedit_can_redo(const kmaskedit *editor);

/*
 * Whether the mask differs from the last point it was marked saved.  The
 * editor never touches a file; the caller saves and then says so.
 */
bool kmaskedit_modified(const kmaskedit *editor);
void kmaskedit_mark_saved(kmaskedit *editor);

/*
 * A counter that changes whenever any cell does.
 *
 * For a caller that derives something expensive from the map - a
 * rectangle decomposition, a collision structure - and needs to know
 * whether what it derived is still current.  Comparing two readings is
 * the whole interface: the value itself means nothing, only its
 * inequality does.
 *
 * "Modified" cannot serve here.  It tracks distance from the last save,
 * so editing and then undoing back leaves it false while the cells took
 * two different shapes on the way.
 */
uint64_t kmaskedit_revision(const kmaskedit *editor);

/* ----------------------------- presentation ----------------------------- */

/*
 * Draw the scene into `out` with the view's top-left corner at
 * (origin_x, origin_y): the image, the region tints over it, the grid and
 * the cursor.  Drawing is clipped to the intersection of the view and the
 * canvas, and the canvas' own clip is left alone, so a caller can reserve
 * a status bar simply by composing at an offset.
 *
 * The origin is not remembered.  Damage rectangles are in view
 * coordinates, and a caller composing at an offset adds the same offset to
 * them.
 */
void kmaskedit_compose(
    kmaskedit *editor, sr_canvas *out, int origin_x, int origin_y);

/*
 * Drain the view rectangles that changed since the last drain, for
 * handing to a damage-aware presenter.
 *
 * Returns the number written.  A caller with no interest in patching can
 * ignore this entirely and present whole frames; the accumulated damage is
 * simply reset by the next drain.
 *
 * The result collapses to a single view-sized rectangle when the change
 * cannot be described in `capacity` rectangles - that is the only reason
 * it collapses.  Whether a given damaged area is still worth patching is
 * the presenter's judgement and not repeated here; kittyfb_present_damage()
 * already measures it and falls back on its own.  Deciding it twice, with
 * two thresholds that could disagree, would make the cheaper path depend
 * on which of them was stricter.
 */
size_t kmaskedit_take_damage(
    kmaskedit *editor, kmaskedit_rect *rects, size_t capacity);

/* Force the next drain to report the whole view.  Needed after anything
 * the editor cannot see, such as the caller redrawing its own chrome. */
void kmaskedit_damage_all(kmaskedit *editor);

#ifdef __cplusplus
}
#endif

#endif /* KILIX_MASK_EDIT_H */
