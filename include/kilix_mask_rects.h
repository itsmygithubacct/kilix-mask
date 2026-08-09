#ifndef KILIX_MASK_RECTS_H
#define KILIX_MASK_RECTS_H

/*
 * Turning a painted map back into rectangles.
 *
 * A map is convenient to paint and inconvenient to ship: a game that
 * stores its rooms as JSON wants a handful of rectangles, not a bitmap,
 * and a collision test wants something it can check without a lookup per
 * pixel.  This converts between the two.
 *
 * The property that makes it safe is that the conversion is a **fixpoint**:
 * decomposing a map and painting the result back reproduces the map cell
 * for cell.  Without that, every edit-save-reload cycle would erode the
 * shape slightly, and the drift would only show up after enough of them
 * that nobody could say which edit caused it.
 *
 * Separate header because most consumers never need it.  A motion mask is
 * consumed as a bitmap and never becomes rectangles at all.
 */

#include "kilix_mask.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Source-pixel coordinates, always aligned to cell boundaries so the
 * round trip is exact. */
typedef struct kmask_rect {
    int x;
    int y;
    int w;
    int h;
} kmask_rect;

/*
 * Cover every cell holding `region` with axis-aligned rectangles.
 *
 * Writes up to `capacity` rects and returns how many were written;
 * `needed` receives the number required, so a caller can size a buffer
 * from a first call that passes capacity 0.  A region that is not painted
 * anywhere needs none, which is not an error.
 *
 * The cover is exact rather than minimal - finding the minimum rectangle
 * cover of an arbitrary shape is expensive, and this runs while somebody
 * is waiting for a save.  Both sweep orientations are tried and the
 * smaller result kept, which matters more than it sounds: painted shapes
 * are usually run-heavy in one direction, and the wrong sweep can produce
 * several times as many rectangles for the same shape.
 */
size_t kmask_cover(
    const kmask *mask,
    uint8_t region,
    kmask_rect *rects,
    size_t capacity,
    size_t *needed);

/*
 * The other shape of the same answer, and the one a room model usually
 * wants: one bounding rectangle for a region, plus rectangles covering
 * the cells inside it that are *not* that region.
 *
 * "Walkable is this box, minus these obstacles" is a far smaller
 * description than a cover of the walkable cells themselves, because a
 * room is mostly floor with furniture in it rather than the reverse.
 *
 * Returns false when the region is unpainted - there is no bounding box
 * of nothing - or when the holes exceed `capacity`, with `needed` set so
 * the caller can decide whether to grow or to simplify.
 */
bool kmask_decompose(
    const kmask *mask,
    uint8_t region,
    kmask_rect *bounds,
    kmask_rect *holes,
    size_t capacity,
    size_t *needed);

/*
 * Paint a decomposition back: set `region` on every cell inside `bounds`
 * that is not inside any hole.  Cells outside `bounds` are untouched, so
 * this composes with other regions already painted.
 *
 * The exact inverse of kmask_decompose(), and the two together are the
 * fixpoint the tests pin.
 */
bool kmask_apply(
    kmask *mask,
    uint8_t region,
    const kmask_rect *bounds,
    const kmask_rect *holes,
    size_t hole_count);

#ifdef __cplusplus
}
#endif

#endif /* KILIX_MASK_RECTS_H */
