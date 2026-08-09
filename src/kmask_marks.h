#ifndef KMASK_MARKS_H
#define KMASK_MARKS_H

/*
 * Things to show alongside a mask that are not part of it.
 *
 * A mask is painted in relation to what is already in the picture, and
 * some of that is not in the picture: where a doorway is, where a
 * character spawns, where a detection zone sits.  Painting walkable space
 * without seeing where the door is means painting blind and finding out
 * from a validator afterwards.
 *
 * These are *annotations*, never part of the map.  Nothing here is saved,
 * nothing here is editable, and nothing here changes a cell.  The caller
 * owns their meaning entirely - this only knows how to draw a rectangle,
 * a point and a label in the mask's own coordinate space.
 *
 * They are drawn by the command rather than by kmaskedit_compose(), for
 * the same reason region attributes are: the editor should not learn one
 * consumer's vocabulary, and every other consumer includes that header.
 */

#include "kilix_mask_edit.h"

#define KMASK_MARK_MAX 512
#define KMASK_MARK_LABEL_MAX 48
#define KMASK_MARK_ERROR_MAX 160

typedef enum kmask_mark_kind {
    KMASK_MARK_RECT = 0,
    KMASK_MARK_POINT
} kmask_mark_kind;

typedef struct kmask_mark {
    kmask_mark_kind kind;
    int x;
    int y;
    int w;
    int h;
    uint32_t rgb;
    char label[KMASK_MARK_LABEL_MAX];
} kmask_mark;

typedef struct kmask_marks {
    kmask_mark items[KMASK_MARK_MAX];
    size_t count;
    /* Past capacity, counted rather than silently forgotten: a caller
     * that sees "12 of 520 shown" can act on it, one that sees a short
     * list cannot tell it is short. */
    size_t dropped;
} kmask_marks;

/*
 * Read a marks file.  Lines, in the mask's source coordinates:
 *
 *   rect  X Y W H RRGGBB [label...]
 *   point X Y RRGGBB [label...]
 *
 * Blank lines and lines starting with '#' are ignored.  A line that does
 * not parse is an error naming its number, rather than a silently missing
 * marker - a door that fails to draw is worse than no marks at all,
 * because the map gets painted around a door that was never there.
 */
bool kmask_marks_load(
    kmask_marks *marks, const char *path, char *error, size_t error_size);

/* Draw them over the view, clipped to it.  Labels are drawn only when
 * `labels` is set, since a busy room is more legible without them. */
void kmask_marks_draw(
    sr_canvas *out,
    const kmaskedit *editor,
    const kmask_marks *marks,
    int width,
    int view_height,
    bool labels);

#endif /* KMASK_MARKS_H */
