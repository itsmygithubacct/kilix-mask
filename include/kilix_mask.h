#ifndef KILIX_MASK_H
#define KILIX_MASK_H

/*
 * Region maps: which parts of an image mean something, and what.
 *
 * The same shape of problem turns up in places that look unrelated.  A
 * camera needs to know which parts of its view to ignore - the tree that
 * moves in every wind, the road, the burnt-in timestamp.  A top-down game
 * needs to know which parts of a room can be walked on, and which stand
 * in front of the character.  Both are a person painting regions over a
 * picture and something later reading them back.
 *
 * A region is an id from 1 to 255; 0 means unset.  Masking uses ids 0 and
 * 1 and never notices the rest.  A game gets walkable, water and blocked
 * as separate ids.  Each id can carry a name and arbitrary key/value
 * attributes, which is how a walk-behind region carries its baseline and
 * a terrain type carries its movement cost without this header knowing
 * what either of those means.
 *
 * This is the model and its file format.  Painting interactively is a
 * separate concern and lives in the editor built on top; a consumer that
 * only reads masks never links it.
 *
 * Dependencies: C11 and zlib.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KILIX_MASK_VERSION_MAJOR 0
#define KILIX_MASK_VERSION_MINOR 1
#define KILIX_MASK_VERSION_PATCH 0

/* Region 0 is "unset" and cannot be named or given attributes; it is the
 * absence of a region rather than one of them. */
#define KMASK_REGION_MAX 255
#define KMASK_NAME_MAX 48
#define KMASK_KEY_MAX 24
#define KMASK_VALUE_MAX 64
#define KMASK_ATTRS_MAX 8

typedef struct kmask kmask;

/*
 * Create an empty map over an image of `source_width` x `source_height`.
 *
 * `cell` is how many source pixels one map cell covers.  1 stores a value
 * per pixel, which is what a motion mask or a walk-behind mask wants; a
 * larger cell stores a grid, which is what a tile game wants and which
 * keeps the file proportionally smaller.  The grid is sized by rounding
 * up, so a cell that does not divide the image evenly still covers all of
 * it and the last row and column simply extend past the edge.
 *
 * The stored resolution is the grid, not the source.  Consumers that want
 * one byte per source pixel call kmask_expand().
 */
bool kmask_create(kmask **mask, int source_width, int source_height, int cell);
void kmask_free(kmask *mask);

int kmask_source_width(const kmask *mask);
int kmask_source_height(const kmask *mask);
int kmask_cell(const kmask *mask);
int kmask_grid_width(const kmask *mask);
int kmask_grid_height(const kmask *mask);

/* ------------------------------ contents -------------------------------- */

/* Grid coordinates.  Out of range reads return 0 and writes are ignored,
 * so a brush sweeping off the edge needs no clamping by the caller. */
uint8_t kmask_get(const kmask *mask, int cx, int cy);
void kmask_set(kmask *mask, int cx, int cy, uint8_t region);

/*
 * One grid row, kmask_grid_width() cells of it, or NULL when the mask is
 * NULL or the row is out of range.
 *
 * For read paths that visit many cells in order - a compositor sampling
 * the region under every view pixel, a cover copying the whole grid -
 * where a bounds-checked call per cell is the measurable cost.  The
 * caller checks the row once and indexes; the pointer stays valid for
 * the mask's lifetime.  It is read-only: writes still go through
 * kmask_set(), which is what keeps every write bounds-checked.
 */
const uint8_t *kmask_row(const kmask *mask, int cy);

/* Source coordinates, mapped through the cell size.  A caller working in
 * image space never has to know the grid resolution. */
uint8_t kmask_get_at(const kmask *mask, int x, int y);
void kmask_set_at(kmask *mask, int x, int y, uint8_t region);

/* Source-coordinate rectangle; x1/y1 exclusive.  Clamped, and empty or
 * inverted rectangles do nothing. */
void kmask_fill_rect(
    kmask *mask, int x0, int y0, int x1, int y1, uint8_t region);

/* Set every cell of one region back to 0. */
void kmask_clear_region(kmask *mask, uint8_t region);
void kmask_clear(kmask *mask);

/* How many cells carry each region; `counts` receives 256 entries. */
void kmask_counts(const kmask *mask, size_t *counts);

/* ------------------------------- regions -------------------------------- */

/*
 * A region's name and attributes travel with the map, in the file.
 *
 * Attributes are free-form key/value strings rather than fixed fields
 * because the consumers disagree about what a region needs to carry: a
 * walk-behind region has a baseline, a terrain type has a movement cost,
 * a camera zone has a label.  Fixing those in this header would mean
 * changing it for every new consumer.
 *
 * Keys and values are ASCII without newlines or '='; both are structural
 * in the on-disk form, and silently mangling a value is worse than
 * refusing it.
 */
bool kmask_region_set_name(kmask *mask, uint8_t region, const char *name);
const char *kmask_region_name(const kmask *mask, uint8_t region);

bool kmask_region_set_attr(
    kmask *mask, uint8_t region, const char *key, const char *value);
const char *kmask_region_attr(
    const kmask *mask, uint8_t region, const char *key);
/* Enumerate: index 0..KMASK_ATTRS_MAX-1, returns false past the end. */
bool kmask_region_attr_at(
    const kmask *mask, uint8_t region, size_t index,
    const char **key, const char **value);

/*
 * Display colour, used when a map is drawn over its image and written
 * into the file's palette so it is visible in an ordinary image viewer.
 * Unset regions get a generated hue, so a file is legible without anyone
 * having chosen colours.
 */
void kmask_region_set_color(kmask *mask, uint8_t region, uint32_t rgb);
uint32_t kmask_region_color(const kmask *mask, uint8_t region);

/* ------------------------------ expansion ------------------------------- */

/*
 * One byte per source pixel, holding the region id.
 * `size` must be source_width * source_height.
 */
bool kmask_expand(const kmask *mask, uint8_t *out, size_t size);

/*
 * The other direction: take one byte per source pixel and reduce it to
 * the grid, replacing everything the map held.
 *
 * For bringing in a map something else produced - an asset in a game's
 * own on-disk format, a mask exported by another tool - without pushing
 * it through kmask_set() a pixel at a time, which for a per-pixel map
 * over a 1280x720 plate is nearly a million calls.
 *
 * Where several source pixels fall in one cell they have to be reconciled,
 * and the rule is **the most common non-zero value, ties to the lowest
 * id**; a cell is 0 only when no pixel in it is set.  That matches how
 * painting already behaves - kmask_fill_rect() covers every cell a
 * rectangle touches rather than only those wholly inside it - so
 * shrinking a map does not quietly drop the thin parts of a region.  At
 * a cell of 1 the question does not arise and the copy is exact.
 *
 * `size` must be source_width * source_height.
 */
bool kmask_import(kmask *mask, const uint8_t *values, size_t size);

/*
 * One byte per source pixel: 0 where `region` is painted, 255 elsewhere.
 *
 * This polarity is deliberate and worth stating, because it is the one
 * easy thing to get backwards.  A motion mask names what to *ignore* -
 * the operator paints the tree - while the detector consuming it treats 0
 * as "ignore this pixel".  Producing that directly means neither side has
 * to invert, and nobody discovers the mistake as a camera that detects
 * only the tree.
 */
bool kmask_expand_exclude(
    const kmask *mask, uint8_t region, uint8_t *out, size_t size);

/* ---------------------------------- io ---------------------------------- */

/*
 * Save as a PNG whose pixels are region ids.
 *
 * A real PNG, not a private format with a familiar extension: it opens in
 * any image viewer, which is most of why the format was chosen.  Regions
 * are written as a palette so they appear as distinct colours rather than
 * as near-black indices, region 0 is transparent, and the names and
 * attributes ride in a tEXt chunk so the map and its metadata cannot
 * become separated.
 *
 * The image is the *grid*, not the source resolution; the source size and
 * cell live in the metadata.  A 6-pixel grid over a 1280x720 plate is a
 * 214x120 PNG rather than a 1280x720 one.
 */
bool kmask_save(const kmask *mask, const char *path);

/*
 * Load one back.  Fails on a file that is not a PNG, not written by this
 * module, or internally inconsistent.  Chunks written by other tools are
 * skipped rather than rejected - an image editor that has re-saved the
 * file will have added some.
 *
 * The out-pointer is cleared before anything else, so a caller reusing
 * one across loads must free what it already holds first.
 */
bool kmask_load(kmask **mask, const char *path);

/* The same two, against memory, for callers that do their own io and for
 * tests that should not touch a filesystem.  On save, *out is allocated
 * with malloc() and belongs to the caller. */
bool kmask_encode(const kmask *mask, uint8_t **out, size_t *size);
bool kmask_decode(kmask **mask, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* KILIX_MASK_H */
