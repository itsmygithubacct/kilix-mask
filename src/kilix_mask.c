/*
 * The region model and its file format.
 *
 * The file is a genuine PNG rather than a private format wearing the
 * extension, because being openable in an ordinary image viewer was most
 * of the reason for choosing it.  That means emitting real chunk framing,
 * real CRC32s and a real zlib stream - not much code, and it buys a file
 * anyone can inspect without writing a viewer first.
 *
 * Pixels are region ids, written through a palette so a map is legible on
 * screen instead of appearing as a near-black field of 0..3.  Region 0 is
 * transparent, so a map laid over its source image shows through where
 * nothing is painted.  Names and attributes live in a tEXt chunk, which
 * is what keeps them from drifting away from the bitmap they describe -
 * the failure the sidecar approach has to reconcile every time it loads.
 */

#include "kilix_mask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define TEXT_KEYWORD "kilix-mask"
#define FORMAT_VERSION 1

typedef struct kmask_attr {
    char key[KMASK_KEY_MAX];
    char value[KMASK_VALUE_MAX];
    bool used;
} kmask_attr;

typedef struct kmask_region {
    char name[KMASK_NAME_MAX];
    kmask_attr attrs[KMASK_ATTRS_MAX];
    uint32_t color;
    bool color_set;
} kmask_region;

struct kmask {
    int source_width;
    int source_height;
    int cell;
    int grid_width;
    int grid_height;
    uint8_t *cells;
    kmask_region regions[KMASK_REGION_MAX + 1];
};

/* ------------------------------- lifecycle ------------------------------ */

static int divide_up(int value, int by)
{
    return (value + by - 1) / by;
}

bool kmask_create(kmask **out, int source_width, int source_height, int cell)
{
    kmask *mask;
    size_t cells;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (source_width <= 0 || source_height <= 0 || cell <= 0) {
        return false;
    }
    if (cell > source_width || cell > source_height) {
        return false;
    }
    mask = calloc(1u, sizeof(*mask));
    if (mask == NULL) {
        return false;
    }
    mask->source_width = source_width;
    mask->source_height = source_height;
    mask->cell = cell;
    mask->grid_width = divide_up(source_width, cell);
    mask->grid_height = divide_up(source_height, cell);
    cells = (size_t)mask->grid_width * (size_t)mask->grid_height;
    mask->cells = calloc(cells, 1u);
    if (mask->cells == NULL) {
        free(mask);
        return false;
    }
    *out = mask;
    return true;
}

void kmask_free(kmask *mask)
{
    if (mask == NULL) {
        return;
    }
    free(mask->cells);
    free(mask);
}

int kmask_source_width(const kmask *mask)
{
    return mask != NULL ? mask->source_width : 0;
}

int kmask_source_height(const kmask *mask)
{
    return mask != NULL ? mask->source_height : 0;
}

int kmask_cell(const kmask *mask)
{
    return mask != NULL ? mask->cell : 0;
}

int kmask_grid_width(const kmask *mask)
{
    return mask != NULL ? mask->grid_width : 0;
}

int kmask_grid_height(const kmask *mask)
{
    return mask != NULL ? mask->grid_height : 0;
}

/* ------------------------------- contents ------------------------------- */

uint8_t kmask_get(const kmask *mask, int cx, int cy)
{
    if (mask == NULL || cx < 0 || cy < 0 ||
        cx >= mask->grid_width || cy >= mask->grid_height) {
        return 0u;
    }
    return mask->cells[(size_t)cy * (size_t)mask->grid_width + (size_t)cx];
}

void kmask_set(kmask *mask, int cx, int cy, uint8_t region)
{
    if (mask == NULL || cx < 0 || cy < 0 ||
        cx >= mask->grid_width || cy >= mask->grid_height) {
        return;
    }
    mask->cells[(size_t)cy * (size_t)mask->grid_width + (size_t)cx] = region;
}

uint8_t kmask_get_at(const kmask *mask, int x, int y)
{
    if (mask == NULL) {
        return 0u;
    }
    return kmask_get(mask, x / mask->cell, y / mask->cell);
}

void kmask_set_at(kmask *mask, int x, int y, uint8_t region)
{
    if (mask == NULL || x < 0 || y < 0) {
        return;
    }
    kmask_set(mask, x / mask->cell, y / mask->cell, region);
}

void kmask_fill_rect(kmask *mask, int x0, int y0, int x1, int y1,
                     uint8_t region)
{
    int cx0;
    int cy0;
    int cx1;
    int cy1;

    if (mask == NULL) {
        return;
    }
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > mask->source_width) { x1 = mask->source_width; }
    if (y1 > mask->source_height) { y1 = mask->source_height; }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    /* Any cell the rectangle touches is painted: a caller filling a
     * source rect expects the whole rect covered, not the cells wholly
     * inside it. */
    cx0 = x0 / mask->cell;
    cy0 = y0 / mask->cell;
    cx1 = divide_up(x1, mask->cell);
    cy1 = divide_up(y1, mask->cell);
    for (int cy = cy0; cy < cy1; cy++) {
        for (int cx = cx0; cx < cx1; cx++) {
            kmask_set(mask, cx, cy, region);
        }
    }
}

void kmask_clear_region(kmask *mask, uint8_t region)
{
    size_t cells;

    if (mask == NULL) {
        return;
    }
    cells = (size_t)mask->grid_width * (size_t)mask->grid_height;
    for (size_t i = 0u; i < cells; i++) {
        if (mask->cells[i] == region) {
            mask->cells[i] = 0u;
        }
    }
}

void kmask_clear(kmask *mask)
{
    if (mask == NULL) {
        return;
    }
    (void)memset(mask->cells, 0,
                 (size_t)mask->grid_width * (size_t)mask->grid_height);
}

void kmask_counts(const kmask *mask, size_t *counts)
{
    size_t cells;

    if (mask == NULL || counts == NULL) {
        return;
    }
    (void)memset(counts, 0, 256u * sizeof(*counts));
    cells = (size_t)mask->grid_width * (size_t)mask->grid_height;
    for (size_t i = 0u; i < cells; i++) {
        counts[mask->cells[i]]++;
    }
}

/* -------------------------------- regions ------------------------------- */

/* Structural in the on-disk form, so refusing beats silently mangling. */
static bool text_is_safe(const char *text, size_t limit)
{
    size_t length;

    if (text == NULL) {
        return false;
    }
    length = strlen(text);
    if (length == 0u || length >= limit) {
        return false;
    }
    for (size_t i = 0u; i < length; i++) {
        const unsigned char c = (unsigned char)text[i];

        if (c < 0x20u || c > 0x7eu || c == '=' ) {
            return false;
        }
    }
    return true;
}

bool kmask_region_set_name(kmask *mask, uint8_t region, const char *name)
{
    if (mask == NULL || region == 0u) {
        return false;
    }
    if (name == NULL) {
        mask->regions[region].name[0] = '\0';
        return true;
    }
    if (!text_is_safe(name, KMASK_NAME_MAX)) {
        return false;
    }
    (void)snprintf(mask->regions[region].name, KMASK_NAME_MAX, "%s", name);
    return true;
}

const char *kmask_region_name(const kmask *mask, uint8_t region)
{
    if (mask == NULL || region == 0u ||
        mask->regions[region].name[0] == '\0') {
        return NULL;
    }
    return mask->regions[region].name;
}

bool kmask_region_set_attr(
    kmask *mask, uint8_t region, const char *key, const char *value)
{
    kmask_attr *slot = NULL;

    if (mask == NULL || region == 0u) {
        return false;
    }
    if (!text_is_safe(key, KMASK_KEY_MAX)) {
        return false;
    }
    for (size_t i = 0u; i < KMASK_ATTRS_MAX; i++) {
        kmask_attr *attr = &mask->regions[region].attrs[i];

        if (attr->used && strcmp(attr->key, key) == 0) {
            slot = attr;
            break;
        }
        if (!attr->used && slot == NULL) {
            slot = attr;
        }
    }
    if (slot == NULL) {
        return false;   /* full; a caller with more than eight wants a file */
    }
    if (value == NULL) {
        slot->used = false;
        slot->key[0] = '\0';
        slot->value[0] = '\0';
        return true;
    }
    if (!text_is_safe(value, KMASK_VALUE_MAX)) {
        return false;
    }
    (void)snprintf(slot->key, KMASK_KEY_MAX, "%s", key);
    (void)snprintf(slot->value, KMASK_VALUE_MAX, "%s", value);
    slot->used = true;
    return true;
}

const char *kmask_region_attr(
    const kmask *mask, uint8_t region, const char *key)
{
    if (mask == NULL || region == 0u || key == NULL) {
        return NULL;
    }
    for (size_t i = 0u; i < KMASK_ATTRS_MAX; i++) {
        const kmask_attr *attr = &mask->regions[region].attrs[i];

        if (attr->used && strcmp(attr->key, key) == 0) {
            return attr->value;
        }
    }
    return NULL;
}

bool kmask_region_attr_at(
    const kmask *mask, uint8_t region, size_t index,
    const char **key, const char **value)
{
    size_t seen = 0u;

    if (mask == NULL || region == 0u) {
        return false;
    }
    for (size_t i = 0u; i < KMASK_ATTRS_MAX; i++) {
        const kmask_attr *attr = &mask->regions[region].attrs[i];

        if (!attr->used) {
            continue;
        }
        if (seen == index) {
            if (key != NULL) { *key = attr->key; }
            if (value != NULL) { *value = attr->value; }
            return true;
        }
        seen++;
    }
    return false;
}

/*
 * A generated hue per id, so a file written without anyone choosing
 * colours is still legible.  Spread around the wheel by a large stride
 * rather than sequentially, so neighbouring ids - which are the ones
 * likely to be adjacent on screen - do not come out nearly identical.
 */
static uint32_t default_color(uint8_t region)
{
    const float hue = (float)((region * 47u) % 360u) / 60.0f;
    const int sector = (int)hue;
    const float f = hue - (float)sector;
    const float v = 1.0f;
    const float s = 0.72f;
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));

    switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return ((uint32_t)(r * 255.0f) << 16) | ((uint32_t)(g * 255.0f) << 8) |
           (uint32_t)(b * 255.0f);
}

void kmask_region_set_color(kmask *mask, uint8_t region, uint32_t rgb)
{
    if (mask == NULL || region == 0u) {
        return;
    }
    mask->regions[region].color = rgb & 0xffffffu;
    mask->regions[region].color_set = true;
}

uint32_t kmask_region_color(const kmask *mask, uint8_t region)
{
    if (mask == NULL || region == 0u) {
        return 0u;
    }
    if (mask->regions[region].color_set) {
        return mask->regions[region].color;
    }
    return default_color(region);
}

/* ------------------------------- expansion ------------------------------ */

bool kmask_expand(const kmask *mask, uint8_t *out, size_t size)
{
    if (mask == NULL || out == NULL) {
        return false;
    }
    if (size != (size_t)mask->source_width * (size_t)mask->source_height) {
        return false;
    }
    for (int y = 0; y < mask->source_height; y++) {
        const uint8_t *row =
            mask->cells + (size_t)(y / mask->cell) * (size_t)mask->grid_width;
        uint8_t *dest = out + (size_t)y * (size_t)mask->source_width;

        for (int x = 0; x < mask->source_width; x++) {
            dest[x] = row[x / mask->cell];
        }
    }
    return true;
}

/*
 * One pass over the source, tallying per cell, then one pass to pick a
 * winner.  Counting into a 256-wide tally per cell rather than sorting
 * keeps this linear in pixels, which matters: the case it exists for is
 * a million of them.
 */
bool kmask_import(kmask *mask, const uint8_t *values, size_t size)
{
    uint16_t (*tally)[KMASK_REGION_MAX + 1];
    size_t cells;

    if (mask == NULL || values == NULL) {
        return false;
    }
    if (size != (size_t)mask->source_width * (size_t)mask->source_height) {
        return false;
    }
    cells = (size_t)mask->grid_width * (size_t)mask->grid_height;
    if (mask->cell == 1) {
        /* Every pixel is its own cell, so there is nothing to reconcile
         * and nothing to count. */
        for (int y = 0; y < mask->source_height; y++) {
            (void)memcpy(mask->cells + (size_t)y * (size_t)mask->grid_width,
                         values + (size_t)y * (size_t)mask->source_width,
                         (size_t)mask->source_width);
        }
        return true;
    }
    tally = calloc(cells, sizeof(*tally));
    if (tally == NULL) {
        return false;
    }
    for (int y = 0; y < mask->source_height; y++) {
        const size_t row = (size_t)(y / mask->cell) * (size_t)mask->grid_width;
        const uint8_t *source = values + (size_t)y * (size_t)mask->source_width;

        for (int x = 0; x < mask->source_width; x++) {
            const uint8_t value = source[x];

            if (value != 0u) {
                /* Saturating, so a cell larger than 65535 pixels cannot
                 * wrap a count back down past a rival. */
                uint16_t *slot = &tally[row + (size_t)(x / mask->cell)][value];

                if (*slot < UINT16_MAX) {
                    (*slot)++;
                }
            }
        }
    }
    for (size_t cell = 0u; cell < cells; cell++) {
        unsigned best = 0u;
        uint16_t best_count = 0u;

        for (unsigned region = 1u; region <= KMASK_REGION_MAX; region++) {
            if (tally[cell][region] > best_count) {
                best_count = tally[cell][region];
                best = region;   /* strictly greater, so ties keep the
                                  * lower id and the result is stable */
            }
        }
        mask->cells[cell] = (uint8_t)best;
    }
    free(tally);
    return true;
}

bool kmask_expand_exclude(
    const kmask *mask, uint8_t region, uint8_t *out, size_t size)
{
    if (mask == NULL || out == NULL) {
        return false;
    }
    if (size != (size_t)mask->source_width * (size_t)mask->source_height) {
        return false;
    }
    for (int y = 0; y < mask->source_height; y++) {
        const uint8_t *row =
            mask->cells + (size_t)(y / mask->cell) * (size_t)mask->grid_width;
        uint8_t *dest = out + (size_t)y * (size_t)mask->source_width;

        for (int x = 0; x < mask->source_width; x++) {
            dest[x] = row[x / mask->cell] == region ? 0u : 255u;
        }
    }
    return true;
}

/* ---------------------------------- png --------------------------------- */

typedef struct buffer {
    uint8_t *data;
    size_t used;
    size_t capacity;
    bool failed;
} buffer;

static bool buffer_reserve(buffer *b, size_t extra)
{
    size_t needed = b->used + extra;
    size_t capacity;
    uint8_t *grown;

    if (b->failed) {
        return false;
    }
    if (needed <= b->capacity) {
        return true;
    }
    capacity = b->capacity == 0u ? 4096u : b->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            b->failed = true;
            return false;
        }
        capacity *= 2u;
    }
    grown = realloc(b->data, capacity);
    if (grown == NULL) {
        b->failed = true;
        return false;
    }
    b->data = grown;
    b->capacity = capacity;
    return true;
}

static void buffer_put(buffer *b, const void *data, size_t size)
{
    if (!buffer_reserve(b, size)) {
        return;
    }
    (void)memcpy(b->data + b->used, data, size);
    b->used += size;
}

static void buffer_u32(buffer *b, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };

    buffer_put(b, bytes, 4u);
}

/* type + payload, length-prefixed, CRC32 over type and payload. */
static void png_chunk(buffer *b, const char *type, const uint8_t *payload,
                      size_t length)
{
    uLong crc;

    if (length > 0x7fffffffu) {
        b->failed = true;
        return;
    }
    buffer_u32(b, (uint32_t)length);
    buffer_put(b, type, 4u);
    if (length > 0u) {
        buffer_put(b, payload, length);
    }
    crc = crc32(0uL, (const Bytef *)type, 4u);
    if (length > 0u) {
        crc = crc32(crc, (const Bytef *)payload, (uInt)length);
    }
    buffer_u32(b, (uint32_t)crc);
}

/* The metadata block.  Line-based and plain ASCII so it stays readable in
 * whatever tool a person uses to look at the file. */
static bool build_text(const kmask *mask, buffer *text)
{
    char line[256];
    int printed;

    buffer_put(text, TEXT_KEYWORD, sizeof(TEXT_KEYWORD));   /* includes NUL */
    printed = snprintf(line, sizeof(line),
                       "version %d\nsource %d %d\ncell %d\n",
                       FORMAT_VERSION, mask->source_width,
                       mask->source_height, mask->cell);
    if (printed < 0) {
        return false;
    }
    buffer_put(text, line, (size_t)printed);

    for (int region = 1; region <= KMASK_REGION_MAX; region++) {
        const kmask_region *entry = &mask->regions[region];
        bool has_attrs = false;

        for (size_t i = 0u; i < KMASK_ATTRS_MAX; i++) {
            has_attrs = has_attrs || entry->attrs[i].used;
        }
        if (entry->name[0] == '\0' && !entry->color_set && !has_attrs) {
            continue;
        }
        printed = snprintf(line, sizeof(line), "region %d", region);
        if (printed < 0) {
            return false;
        }
        buffer_put(text, line, (size_t)printed);
        if (entry->name[0] != '\0') {
            printed = snprintf(line, sizeof(line), " name=%s", entry->name);
            if (printed < 0) { return false; }
            buffer_put(text, line, (size_t)printed);
        }
        if (entry->color_set) {
            printed = snprintf(line, sizeof(line), " color=%06x",
                               entry->color & 0xffffffu);
            if (printed < 0) { return false; }
            buffer_put(text, line, (size_t)printed);
        }
        for (size_t i = 0u; i < KMASK_ATTRS_MAX; i++) {
            if (!entry->attrs[i].used) {
                continue;
            }
            printed = snprintf(line, sizeof(line), " %s=%s",
                               entry->attrs[i].key, entry->attrs[i].value);
            if (printed < 0) { return false; }
            buffer_put(text, line, (size_t)printed);
        }
        buffer_put(text, "\n", 1u);
    }
    return !text->failed;
}

bool kmask_encode(const kmask *mask, uint8_t **out, size_t *size)
{
    static const uint8_t signature[8] = {
        137u, 'P', 'N', 'G', '\r', '\n', 26u, '\n'
    };
    buffer file = {NULL, 0u, 0u, false};
    buffer text = {NULL, 0u, 0u, false};
    buffer raw = {NULL, 0u, 0u, false};
    uint8_t header[13];
    uint8_t palette[256 * 3];
    uint8_t transparency[1];
    uint8_t *compressed = NULL;
    uLongf compressed_length;
    bool ok = false;

    if (mask == NULL || out == NULL || size == NULL) {
        return false;
    }
    *out = NULL;
    *size = 0u;

    buffer_put(&file, signature, sizeof(signature));

    /* IHDR: 8-bit, colour type 3 (palette), no interlace. */
    header[0] = (uint8_t)((uint32_t)mask->grid_width >> 24);
    header[1] = (uint8_t)((uint32_t)mask->grid_width >> 16);
    header[2] = (uint8_t)((uint32_t)mask->grid_width >> 8);
    header[3] = (uint8_t)mask->grid_width;
    header[4] = (uint8_t)((uint32_t)mask->grid_height >> 24);
    header[5] = (uint8_t)((uint32_t)mask->grid_height >> 16);
    header[6] = (uint8_t)((uint32_t)mask->grid_height >> 8);
    header[7] = (uint8_t)mask->grid_height;
    header[8] = 8u;
    header[9] = 3u;
    header[10] = 0u;
    header[11] = 0u;
    header[12] = 0u;
    png_chunk(&file, "IHDR", header, sizeof(header));

    /* PLTE: every id gets a colour so the file is legible on screen. */
    palette[0] = 0u;
    palette[1] = 0u;
    palette[2] = 0u;
    for (int region = 1; region <= KMASK_REGION_MAX; region++) {
        const uint32_t rgb = kmask_region_color(mask, (uint8_t)region);

        palette[region * 3] = (uint8_t)(rgb >> 16);
        palette[region * 3 + 1] = (uint8_t)(rgb >> 8);
        palette[region * 3 + 2] = (uint8_t)rgb;
    }
    png_chunk(&file, "PLTE", palette, sizeof(palette));

    /* Unset is transparent, so a map laid over its source shows through. */
    transparency[0] = 0u;
    png_chunk(&file, "tRNS", transparency, sizeof(transparency));

    if (!build_text(mask, &text)) {
        goto done;
    }
    png_chunk(&file, "tEXt", text.data, text.used);

    /* IDAT: filter byte 0 per scanline, then one zlib stream. */
    for (int y = 0; y < mask->grid_height; y++) {
        const uint8_t filter = 0u;

        buffer_put(&raw, &filter, 1u);
        buffer_put(&raw, mask->cells + (size_t)y * (size_t)mask->grid_width,
                   (size_t)mask->grid_width);
    }
    if (raw.failed) {
        goto done;
    }
    compressed_length = compressBound((uLong)raw.used);
    compressed = malloc((size_t)compressed_length);
    if (compressed == NULL) {
        goto done;
    }
    if (compress2(compressed, &compressed_length, raw.data, (uLong)raw.used,
                  9) != Z_OK) {
        goto done;
    }
    png_chunk(&file, "IDAT", compressed, (size_t)compressed_length);
    png_chunk(&file, "IEND", NULL, 0u);

    if (file.failed) {
        goto done;
    }
    *out = file.data;
    *size = file.used;
    file.data = NULL;
    ok = true;

done:
    free(compressed);
    free(text.data);
    free(raw.data);
    free(file.data);
    return ok;
}

static uint32_t read_u32(const uint8_t *at)
{
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) |
           ((uint32_t)at[2] << 8) | (uint32_t)at[3];
}

/* Parse the metadata block back into geometry and regions. */
static bool parse_text(kmask **mask, const char *body, size_t length,
                       int grid_width, int grid_height)
{
    char *copy = malloc(length + 1u);
    char *line;
    char *save = NULL;
    int source_width = 0;
    int source_height = 0;
    int cell = 0;
    int version = 0;
    bool ok = false;

    if (copy == NULL) {
        return false;
    }
    (void)memcpy(copy, body, length);
    copy[length] = '\0';

    /* Geometry first: the regions are set on the mask it describes. */
    for (line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        if (sscanf(line, "version %d", &version) == 1) {
            continue;
        }
        if (sscanf(line, "source %d %d", &source_width, &source_height) == 2) {
            continue;
        }
        (void)sscanf(line, "cell %d", &cell);
    }
    if (version != FORMAT_VERSION || source_width <= 0 || source_height <= 0 ||
        cell <= 0) {
        goto done;
    }
    /* The grid in the header and the geometry in the metadata have to
     * agree, or one of them has been edited and neither can be trusted. */
    if (divide_up(source_width, cell) != grid_width ||
        divide_up(source_height, cell) != grid_height) {
        goto done;
    }
    if (!kmask_create(mask, source_width, source_height, cell)) {
        goto done;
    }

    (void)memcpy(copy, body, length);
    copy[length] = '\0';
    save = NULL;
    for (line = strtok_r(copy, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        int region = 0;
        int consumed = 0;
        char *cursor;

        if (sscanf(line, "region %d%n", &region, &consumed) != 1) {
            continue;
        }
        if (region < 1 || region > KMASK_REGION_MAX) {
            continue;
        }
        cursor = line + consumed;
        while (*cursor != '\0') {
            char key[KMASK_KEY_MAX];
            char value[KMASK_VALUE_MAX];
            size_t key_length = 0u;
            size_t value_length = 0u;

            while (*cursor == ' ') {
                cursor++;
            }
            while (*cursor != '\0' && *cursor != '=' && *cursor != ' ' &&
                   key_length + 1u < sizeof(key)) {
                key[key_length++] = *cursor++;
            }
            key[key_length] = '\0';
            if (*cursor != '=') {
                break;
            }
            cursor++;
            while (*cursor != '\0' && *cursor != ' ' &&
                   value_length + 1u < sizeof(value)) {
                value[value_length++] = *cursor++;
            }
            value[value_length] = '\0';
            if (key_length == 0u || value_length == 0u) {
                continue;
            }
            if (strcmp(key, "name") == 0) {
                (void)kmask_region_set_name(*mask, (uint8_t)region, value);
            } else if (strcmp(key, "color") == 0) {
                unsigned long rgb = strtoul(value, NULL, 16);

                kmask_region_set_color(*mask, (uint8_t)region,
                                       (uint32_t)rgb);
            } else {
                (void)kmask_region_set_attr(*mask, (uint8_t)region, key,
                                            value);
            }
        }
    }
    ok = true;

done:
    free(copy);
    return ok;
}

bool kmask_decode(kmask **out, const uint8_t *data, size_t size)
{
    static const uint8_t signature[8] = {
        137u, 'P', 'N', 'G', '\r', '\n', 26u, '\n'
    };
    size_t at = 8u;
    int grid_width = 0;
    int grid_height = 0;
    kmask *mask = NULL;
    buffer idat = {NULL, 0u, 0u, false};
    uint8_t *raw = NULL;
    uLongf raw_length;
    bool ok = false;

    if (out == NULL || data == NULL) {
        return false;
    }
    *out = NULL;
    if (size < 8u + 12u || memcmp(data, signature, 8u) != 0) {
        return false;
    }

    while (at + 12u <= size) {
        const uint32_t length = read_u32(data + at);
        const char *type = (const char *)(data + at + 4u);
        const uint8_t *payload = data + at + 8u;

        if (length > size - at - 12u) {
            goto done;
        }
        if (memcmp(type, "IHDR", 4u) == 0 && length == 13u) {
            grid_width = (int)read_u32(payload);
            grid_height = (int)read_u32(payload + 4u);
            /* Only what this module writes; anything else is somebody
             * else's PNG that happens to be pointed at us. */
            if (payload[8] != 8u || payload[9] != 3u || payload[12] != 0u) {
                goto done;
            }
            if (grid_width <= 0 || grid_height <= 0) {
                goto done;
            }
        } else if (memcmp(type, "tEXt", 4u) == 0) {
            const size_t keyword = sizeof(TEXT_KEYWORD) - 1u;

            if (length > keyword + 1u &&
                memcmp(payload, TEXT_KEYWORD, keyword) == 0 &&
                payload[keyword] == '\0') {
                if (mask != NULL) {
                    goto done;   /* two metadata blocks: ambiguous */
                }
                if (!parse_text(&mask, (const char *)payload + keyword + 1u,
                                length - keyword - 1u, grid_width,
                                grid_height)) {
                    goto done;
                }
            }
            /* Other tEXt chunks belong to whatever else touched the file. */
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            buffer_put(&idat, payload, length);
        }
        at += 12u + length;
    }
    if (mask == NULL || idat.failed || idat.used == 0u) {
        goto done;
    }

    /* One filter byte plus one index per pixel, per scanline. */
    raw_length = (uLongf)((size_t)(grid_width + 1) * (size_t)grid_height);
    raw = malloc((size_t)raw_length);
    if (raw == NULL) {
        goto done;
    }
    if (uncompress(raw, &raw_length, idat.data, (uLong)idat.used) != Z_OK) {
        goto done;
    }
    if (raw_length != (uLongf)((size_t)(grid_width + 1) * (size_t)grid_height)) {
        goto done;
    }
    for (int y = 0; y < grid_height; y++) {
        const uint8_t *row = raw + (size_t)y * (size_t)(grid_width + 1);

        /* Only filter 0 is ever written here; anything else means the
         * file came from a general encoder and this reader would produce
         * plausible nonsense rather than fail. */
        if (row[0] != 0u) {
            goto done;
        }
        (void)memcpy(mask->cells + (size_t)y * (size_t)grid_width, row + 1,
                     (size_t)grid_width);
    }
    *out = mask;
    mask = NULL;
    ok = true;

done:
    kmask_free(mask);
    free(idat.data);
    free(raw);
    return ok;
}

bool kmask_save(const kmask *mask, const char *path)
{
    uint8_t *encoded = NULL;
    size_t size = 0u;
    FILE *handle;
    bool ok;

    if (path == NULL || !kmask_encode(mask, &encoded, &size)) {
        return false;
    }
    handle = fopen(path, "wb");
    if (handle == NULL) {
        free(encoded);
        return false;
    }
    ok = fwrite(encoded, 1u, size, handle) == size;
    ok = fclose(handle) == 0 && ok;
    free(encoded);
    return ok;
}

bool kmask_load(kmask **mask, const char *path)
{
    FILE *handle;
    uint8_t *data;
    long length;
    size_t got;
    bool ok = false;

    if (mask == NULL) {
        return false;
    }
    *mask = NULL;
    if (path == NULL) {
        return false;
    }
    handle = fopen(path, "rb");
    if (handle == NULL) {
        return false;
    }
    if (fseek(handle, 0, SEEK_END) != 0) {
        (void)fclose(handle);
        return false;
    }
    length = ftell(handle);
    if (length <= 0 || fseek(handle, 0, SEEK_SET) != 0) {
        (void)fclose(handle);
        return false;
    }
    data = malloc((size_t)length);
    if (data == NULL) {
        (void)fclose(handle);
        return false;
    }
    got = fread(data, 1u, (size_t)length, handle);
    (void)fclose(handle);
    if (got == (size_t)length) {
        ok = kmask_decode(mask, data, got);
    }
    free(data);
    return ok;
}
