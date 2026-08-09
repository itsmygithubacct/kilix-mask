#ifndef KILIX_MASK_IMAGE_H
#define KILIX_MASK_IMAGE_H

/*
 * Reading a picture to paint over.
 *
 * Not a mask - a mask is a PNG this module wrote and kmask_load() reads.
 * This is the *photograph*: a camera snapshot, a room plate, whatever the
 * regions are being drawn on top of.  Different problem, different
 * entry point, and the two must not be confused: kmask_load() refuses an
 * ordinary PNG on purpose, because the geometry it needs lives in
 * metadata a general encoder would not have written.
 *
 * It lives here rather than in soft-raster because soft-raster is pure
 * ISO C11 with libm and nothing else, and says so.  A PNG reader needs
 * zlib, and a game that only wants to draw triangles should not acquire
 * a compression library to do it.  This module already links zlib for
 * the mask format, so the cost lands where it was already paid.
 *
 * Supported: colour types 0, 2, 3, 4 and 6, bit depths 1 through 16, all
 * five scanline filters, tRNS transparency.  Not supported: interlaced
 * files, which are refused rather than half-read.
 */

#include "soft_raster.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode a PNG into a newly allocated canvas that the caller owns and
 * frees with sr_canvas_free().  `*canvas` is reset first, so a caller
 * reusing one across loads must free what it already holds.
 *
 * Pixels come out as 0xAARRGGBB with alpha filled in: opaque where the
 * file has no transparency, so a caller that does not care about alpha
 * can ignore it entirely.  16-bit samples are reduced to 8 by keeping
 * the high byte; sub-byte greyscale is scaled to the full range so a
 * 1-bit image is black and white rather than black and almost-black.
 */
bool kmask_image_decode(sr_canvas *canvas, const uint8_t *data, size_t size);

/*
 * The same from a file, choosing the reader by content rather than by
 * extension: a PNG signature picks the PNG path, "P6" picks soft-raster's
 * PPM loader.  Callers therefore need one entry point for both, and a
 * mislabelled file still opens.
 */
bool kmask_image_load(sr_canvas *canvas, const char *path);

/*
 * Why the last decode failed, as a short phrase for an error message
 * ("interlaced", "unsupported colour type"), or NULL after a success.
 * Static storage, overwritten by the next call on this thread.
 */
const char *kmask_image_error(void);

#ifdef __cplusplus
}
#endif

#endif /* KILIX_MASK_IMAGE_H */
