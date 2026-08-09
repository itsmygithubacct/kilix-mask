/*
 * A PNG reader for pictures.
 *
 * The mask codec next door reads only what it writes - one filter, one
 * colour type - because a mask file whose geometry it cannot trust is
 * worse than no file.  A photograph is the opposite case: it comes from
 * whatever wrote it, and refusing a perfectly ordinary PNG because it
 * used the Paeth filter would make the tool useless on real input.
 *
 * So this handles the format as encoders actually emit it, and refuses
 * only what it genuinely cannot do - interlacing - by name, so the
 * message says what to convert.
 */

#include "kilix_mask_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#define PNG_MAX_DIMENSION 32768

typedef struct png_header {
    int width;
    int height;
    int depth;
    int colour;
    int channels;
} png_header;

static const char *g_error;

const char *kmask_image_error(void)
{
    return g_error;
}

static bool fail(const char *reason)
{
    g_error = reason;
    return false;
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static int channels_for(int colour)
{
    switch (colour) {
    case 0: return 1;   /* grey */
    case 2: return 3;   /* rgb */
    case 3: return 1;   /* palette index */
    case 4: return 2;   /* grey + alpha */
    case 6: return 4;   /* rgba */
    default: return 0;
    }
}

/* Legal depth depends on the colour type; the spec's table, not a guess. */
static bool depth_allowed(int colour, int depth)
{
    switch (colour) {
    case 0:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 ||
               depth == 16;
    case 3:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6:
        return depth == 8 || depth == 16;
    default:
        return false;
    }
}

static int paeth(int a, int b, int c)
{
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

/*
 * Undo the per-scanline filters, in place, top to bottom.
 *
 * Each row is one filter byte then `stride` data bytes, and every filter
 * refers to the row above, so this cannot be done out of order or in
 * parallel without keeping the previous row around anyway.
 */
static bool unfilter(uint8_t *raw, size_t stride, int height, size_t bpp)
{
    uint8_t *previous = NULL;

    for (int y = 0; y < height; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1u);
        const uint8_t type = row[0];
        uint8_t *line = row + 1;

        switch (type) {
        case 0:
            break;
        case 1:
            for (size_t i = bpp; i < stride; i++) {
                line[i] = (uint8_t)(line[i] + line[i - bpp]);
            }
            break;
        case 2:
            if (previous != NULL) {
                for (size_t i = 0u; i < stride; i++) {
                    line[i] = (uint8_t)(line[i] + previous[i]);
                }
            }
            break;
        case 3:
            for (size_t i = 0u; i < stride; i++) {
                const int left = i >= bpp ? line[i - bpp] : 0;
                const int above = previous != NULL ? previous[i] : 0;

                line[i] = (uint8_t)(line[i] + (left + above) / 2);
            }
            break;
        case 4:
            for (size_t i = 0u; i < stride; i++) {
                const int left = i >= bpp ? line[i - bpp] : 0;
                const int above = previous != NULL ? previous[i] : 0;
                const int corner =
                    (previous != NULL && i >= bpp) ? previous[i - bpp] : 0;

                line[i] = (uint8_t)(line[i] + paeth(left, above, corner));
            }
            break;
        default:
            return fail("unknown scanline filter");
        }
        previous = line;
    }
    return true;
}

/* One sample from a scanline, as 0..255 whatever the stored depth. */
static unsigned sample(const uint8_t *line, size_t index, int depth,
                       bool scale_to_full)
{
    if (depth == 8) {
        return line[index];
    }
    if (depth == 16) {
        /* The low byte is precision this canvas cannot hold. */
        return line[index * 2u];
    }
    {
        const unsigned per_byte = (unsigned)(8 / depth);
        const uint8_t packed = line[index / per_byte];
        const unsigned shift =
            (unsigned)(8 - depth) - (unsigned)(index % per_byte) *
                                        (unsigned)depth;
        const unsigned mask = (1u << depth) - 1u;
        const unsigned value = (packed >> shift) & mask;

        /* A grey sample has to be stretched, or a 1-bit image comes out
         * black and almost-black.  A palette index must not be. */
        return scale_to_full ? value * 255u / mask : value;
    }
}

static bool read_header(const uint8_t *payload, uint32_t length,
                        png_header *header)
{
    if (length != 13u) {
        return fail("malformed header");
    }
    header->width = (int)read_u32(payload);
    header->height = (int)read_u32(payload + 4u);
    header->depth = payload[8];
    header->colour = payload[9];
    if (header->width <= 0 || header->height <= 0 ||
        header->width > PNG_MAX_DIMENSION ||
        header->height > PNG_MAX_DIMENSION) {
        return fail("implausible dimensions");
    }
    if (payload[10] != 0u || payload[11] != 0u) {
        return fail("unknown compression or filter method");
    }
    if (payload[12] != 0u) {
        /* Adam7 is seven interleaved passes with their own geometry.
         * Refusing by name beats reading one pass and calling it the
         * picture. */
        return fail("interlaced");
    }
    header->channels = channels_for(header->colour);
    if (header->channels == 0) {
        return fail("unsupported colour type");
    }
    if (!depth_allowed(header->colour, header->depth)) {
        return fail("bit depth not legal for this colour type");
    }
    return true;
}

bool kmask_image_decode(sr_canvas *canvas, const uint8_t *data, size_t size)
{
    static const uint8_t signature[8] = {
        137u, 'P', 'N', 'G', '\r', '\n', 26u, '\n'
    };
    png_header header = {0, 0, 0, 0, 0};
    uint8_t palette[256][3];
    uint8_t palette_alpha[256];
    size_t palette_count = 0u;
    uint8_t *idat = NULL;
    size_t idat_size = 0u;
    size_t idat_used = 0u;
    uint8_t *raw = NULL;
    uLongf raw_length;
    size_t stride;
    size_t bits_per_pixel;
    size_t bpp;
    size_t at = 8u;
    bool have_header = false;
    bool ok = false;

    g_error = NULL;
    if (canvas == NULL || data == NULL) {
        return fail("nothing to decode");
    }
    (void)memset(canvas, 0, sizeof(*canvas));
    (void)memset(palette_alpha, 0xFF, sizeof(palette_alpha));
    if (size < 8u + 12u || memcmp(data, signature, 8u) != 0) {
        return fail("not a PNG");
    }

    /*
     * Two passes over the chunks.  The first only adds up the IDAT
     * lengths so the compressed image can be assembled into one exact
     * allocation - a PNG may split it across any number of chunks, and
     * zlib needs the stream whole.
     */
    while (at + 12u <= size) {
        const uint32_t length = read_u32(data + at);

        if (length > size - at - 12u) {
            return fail("truncated");
        }
        if (memcmp(data + at + 4u, "IDAT", 4u) == 0) {
            idat_size += length;
        }
        at += 12u + length;
    }
    if (idat_size == 0u) {
        return fail("no image data");
    }
    idat = malloc(idat_size);
    if (idat == NULL) {
        return fail("out of memory");
    }

    at = 8u;
    while (at + 12u <= size) {
        const uint32_t length = read_u32(data + at);
        const char *type = (const char *)(data + at + 4u);
        const uint8_t *payload = data + at + 8u;

        if (memcmp(type, "IHDR", 4u) == 0) {
            if (!read_header(payload, length, &header)) {
                goto done;
            }
            have_header = true;
        } else if (memcmp(type, "PLTE", 4u) == 0) {
            if (length % 3u != 0u || length > 256u * 3u) {
                (void)fail("malformed palette");
                goto done;
            }
            palette_count = length / 3u;
            (void)memcpy(palette, payload, length);
        } else if (memcmp(type, "tRNS", 4u) == 0) {
            /* Only the palette form is honoured.  Colour-key
             * transparency on a photograph is vanishingly rare and
             * getting it wrong would punch holes in the picture. */
            if (have_header && header.colour == 3 && length <= 256u) {
                (void)memcpy(palette_alpha, payload, length);
            }
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            (void)memcpy(idat + idat_used, payload, length);
            idat_used += length;
        }
        at += 12u + length;
    }
    if (!have_header) {
        (void)fail("no header");
        goto done;
    }
    if (header.colour == 3 && palette_count == 0u) {
        (void)fail("palette image with no palette");
        goto done;
    }

    bits_per_pixel = (size_t)header.channels * (size_t)header.depth;
    stride = ((size_t)header.width * bits_per_pixel + 7u) / 8u;
    bpp = bits_per_pixel >= 8u ? bits_per_pixel / 8u : 1u;

    raw_length = (uLongf)((stride + 1u) * (size_t)header.height);
    raw = malloc((size_t)raw_length);
    if (raw == NULL) {
        (void)fail("out of memory");
        goto done;
    }
    if (uncompress(raw, &raw_length, idat, (uLong)idat_used) != Z_OK ||
        raw_length != (uLongf)((stride + 1u) * (size_t)header.height)) {
        (void)fail("compressed data does not match the header");
        goto done;
    }
    if (!unfilter(raw, stride, header.height, bpp)) {
        goto done;
    }
    if (!sr_canvas_init(canvas, header.width, header.height)) {
        (void)fail("out of memory");
        goto done;
    }

    for (int y = 0; y < header.height; y++) {
        const uint8_t *line = raw + (size_t)y * (stride + 1u) + 1u;
        uint32_t *out = canvas->px + (size_t)y * (size_t)header.width;

        for (int x = 0; x < header.width; x++) {
            const size_t base = (size_t)x * (size_t)header.channels;
            unsigned r;
            unsigned g;
            unsigned b;
            unsigned a = 255u;

            switch (header.colour) {
            case 0:
                r = sample(line, base, header.depth, true);
                g = r;
                b = r;
                break;
            case 4:
                r = sample(line, base, header.depth, true);
                g = r;
                b = r;
                a = sample(line, base + 1u, header.depth, true);
                break;
            case 3: {
                const unsigned index = sample(line, base, header.depth, false);

                if (index >= palette_count) {
                    (void)fail("palette index out of range");
                    sr_canvas_free(canvas);
                    goto done;
                }
                r = palette[index][0];
                g = palette[index][1];
                b = palette[index][2];
                a = palette_alpha[index];
                break;
            }
            case 6:
                a = sample(line, base + 3u, header.depth, true);
                /* fall through */
            default:
                r = sample(line, base, header.depth, true);
                g = sample(line, base + 1u, header.depth, true);
                b = sample(line, base + 2u, header.depth, true);
                break;
            }
            out[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    ok = true;

done:
    free(raw);
    free(idat);
    return ok;
}

bool kmask_image_load(sr_canvas *canvas, const char *path)
{
    static const uint8_t signature[8] = {
        137u, 'P', 'N', 'G', '\r', '\n', 26u, '\n'
    };
    uint8_t head[8];
    FILE *file;
    uint8_t *data = NULL;
    long size;
    size_t read_count;
    bool is_png;

    g_error = NULL;
    if (canvas == NULL || path == NULL) {
        return fail("nothing to load");
    }
    (void)memset(canvas, 0, sizeof(*canvas));
    file = fopen(path, "rb");
    if (file == NULL) {
        return fail("cannot be opened");
    }
    /* Chosen by content, not by extension: a plate saved as .png that is
     * really a PPM still opens, and so does the reverse. */
    is_png = fread(head, 1u, sizeof(head), file) == sizeof(head) &&
             memcmp(head, signature, sizeof(head)) == 0;
    if (!is_png) {
        (void)fclose(file);
        if (sr_load_ppm(canvas, path)) {
            return true;
        }
        return fail("not a PNG or a binary PPM");
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return fail("cannot be read");
    }
    data = malloc((size_t)size);
    if (data == NULL) {
        (void)fclose(file);
        return fail("out of memory");
    }
    read_count = fread(data, 1u, (size_t)size, file);
    (void)fclose(file);
    if (read_count != (size_t)size) {
        free(data);
        return fail("cannot be read");
    }
    {
        const bool ok = kmask_image_decode(canvas, data, (size_t)size);

        free(data);
        return ok;
    }
}
