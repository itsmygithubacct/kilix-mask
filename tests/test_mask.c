#include "kilix_mask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

static bool
test_geometry_and_rejections(void)
{
    kmask *mask = NULL;

    CHECK(!kmask_create(NULL, 100, 100, 1));
    CHECK(!kmask_create(&mask, 0, 100, 1));
    CHECK(!kmask_create(&mask, 100, -1, 1));
    CHECK(!kmask_create(&mask, 100, 100, 0));
    /* A cell larger than the image is a configuration error, not a
     * one-cell mask. */
    CHECK(!kmask_create(&mask, 100, 100, 200));
    CHECK(mask == NULL);

    /* Per-pixel: the grid is the source. */
    CHECK(kmask_create(&mask, 640, 360, 1));
    CHECK(kmask_grid_width(mask) == 640 && kmask_grid_height(mask) == 360);
    CHECK(kmask_cell(mask) == 1);
    kmask_free(mask);

    /* A cell that divides evenly. */
    CHECK(kmask_create(&mask, 1280, 720, 6));
    CHECK(kmask_grid_width(mask) == 214);   /* 1280/6 rounded up */
    CHECK(kmask_grid_height(mask) == 120);
    kmask_free(mask);

    /* One that does not: rounding up so the grid still covers the image,
     * rather than leaving the last strip unrepresentable. */
    CHECK(kmask_create(&mask, 100, 100, 7));
    CHECK(kmask_grid_width(mask) == 15 && kmask_grid_height(mask) == 15);
    CHECK(kmask_source_width(mask) == 100);
    kmask_free(mask);
    return true;
}

static bool
test_painting_in_both_coordinate_spaces(void)
{
    kmask *mask = NULL;
    size_t counts[256];

    CHECK(kmask_create(&mask, 100, 100, 10));

    /* Grid coordinates. */
    kmask_set(mask, 3, 4, 7u);
    CHECK(kmask_get(mask, 3, 4) == 7u);
    /* The same cell, addressed in source pixels. */
    CHECK(kmask_get_at(mask, 35, 45) == 7u);
    CHECK(kmask_get_at(mask, 30, 40) == 7u);
    CHECK(kmask_get_at(mask, 39, 49) == 7u);
    CHECK(kmask_get_at(mask, 40, 40) == 0u);

    /* Source-coordinate writes land in the covering cell. */
    kmask_set_at(mask, 55, 65, 2u);
    CHECK(kmask_get(mask, 5, 6) == 2u);

    /* Out of range is ignored rather than fatal: a brush sweeping off the
     * edge should not need clamping by every caller. */
    kmask_set(mask, -1, 0, 9u);
    kmask_set(mask, 0, 999, 9u);
    kmask_set_at(mask, -5, -5, 9u);
    CHECK(kmask_get(mask, -1, 0) == 0u);
    CHECK(kmask_get(mask, 999, 999) == 0u);

    /* Reads just left of and above the image are out of range too, even
     * though truncating division would land them in cell 0.  A sprite
     * probing x-1 at the left edge must see nothing, not the edge cell. */
    kmask_set(mask, 0, 0, 8u);
    CHECK(kmask_get_at(mask, -3, -3) == 0u);
    CHECK(kmask_get_at(mask, -3, 5) == 0u);
    CHECK(kmask_get_at(mask, 5, -3) == 0u);
    CHECK(kmask_get_at(mask, -1, 0) == 0u);
    CHECK(kmask_get_at(mask, 5, 5) == 8u);
    kmask_set(mask, 0, 0, 0u);

    /* A rect covers every cell it touches, not only whole ones. */
    kmask_fill_rect(mask, 12, 12, 25, 25, 3u);
    CHECK(kmask_get(mask, 1, 1) == 3u);
    CHECK(kmask_get(mask, 2, 2) == 3u);
    CHECK(kmask_get(mask, 0, 0) == 0u);

    /* Clamped, and empty or inverted does nothing. */
    kmask_fill_rect(mask, -50, -50, 5, 5, 4u);
    CHECK(kmask_get(mask, 0, 0) == 4u);
    kmask_fill_rect(mask, 60, 60, 60, 70, 5u);
    kmask_fill_rect(mask, 70, 70, 60, 60, 5u);
    CHECK(kmask_get(mask, 6, 6) == 0u);

    kmask_counts(mask, counts);
    CHECK(counts[7u] == 1u);
    CHECK(counts[3u] == 4u);   /* cells 1..2 x 1..2 */
    CHECK(counts[5u] == 0u);

    kmask_clear_region(mask, 3u);
    kmask_counts(mask, counts);
    CHECK(counts[3u] == 0u);
    CHECK(counts[7u] == 1u);

    kmask_clear(mask);
    kmask_counts(mask, counts);
    CHECK(counts[0u] == 100u);
    kmask_free(mask);
    return true;
}

static bool
test_region_names_and_attributes(void)
{
    kmask *mask = NULL;
    const char *key = NULL;
    const char *value = NULL;

    CHECK(kmask_create(&mask, 64, 64, 1));

    CHECK(kmask_region_set_name(mask, 1u, "walkable"));
    CHECK(strcmp(kmask_region_name(mask, 1u), "walkable") == 0);
    CHECK(kmask_region_name(mask, 2u) == NULL);
    /* Region 0 is the absence of a region, not one of them. */
    CHECK(!kmask_region_set_name(mask, 0u, "unset"));
    CHECK(kmask_region_name(mask, 0u) == NULL);

    /* Free-form attributes: this header does not know what a baseline or
     * a movement cost is, and does not need to. */
    CHECK(kmask_region_set_attr(mask, 3u, "baseline", "412"));
    CHECK(kmask_region_set_attr(mask, 3u, "cost", "3"));
    CHECK(strcmp(kmask_region_attr(mask, 3u, "baseline"), "412") == 0);
    CHECK(kmask_region_attr(mask, 3u, "missing") == NULL);
    /* Setting the same key replaces rather than duplicating. */
    CHECK(kmask_region_set_attr(mask, 3u, "baseline", "500"));
    CHECK(strcmp(kmask_region_attr(mask, 3u, "baseline"), "500") == 0);
    CHECK(kmask_region_attr_at(mask, 3u, 0u, &key, &value));
    CHECK(kmask_region_attr_at(mask, 3u, 1u, NULL, NULL));
    CHECK(!kmask_region_attr_at(mask, 3u, 2u, NULL, NULL));

    /* '=' and newlines are structural on disk, so they are refused rather
     * than written out and silently mangled on the way back. */
    CHECK(!kmask_region_set_attr(mask, 3u, "bad=key", "x"));
    CHECK(!kmask_region_set_attr(mask, 3u, "k", "bad=value"));
    CHECK(!kmask_region_set_name(mask, 4u, "two\nlines"));
    CHECK(!kmask_region_set_name(mask, 4u, ""));

    /* Colours: chosen ones are kept, unchosen ones are generated so a
     * file is legible without anyone having picked any. */
    kmask_region_set_color(mask, 5u, 0x336699u);
    CHECK(kmask_region_color(mask, 5u) == 0x336699u);
    CHECK(kmask_region_color(mask, 6u) != 0u);
    CHECK(kmask_region_color(mask, 6u) != kmask_region_color(mask, 7u));

    kmask_free(mask);
    return true;
}

/* The polarity that is easy to get backwards: a motion mask names what to
 * IGNORE, and the detector reading it treats 0 as ignore. */
static bool
test_expansion_polarity(void)
{
    kmask *mask = NULL;
    uint8_t *out = malloc(100u * 50u);

    CHECK(out != NULL);
    CHECK(kmask_create(&mask, 100, 50, 10));
    kmask_fill_rect(mask, 0, 0, 50, 20, 1u);

    /* Raw ids, expanded to source resolution. */
    CHECK(kmask_expand(mask, out, 100u * 50u));
    CHECK(out[0] == 1u);
    CHECK(out[49] == 1u);
    CHECK(out[50] == 0u);
    CHECK(out[(size_t)19 * 100u + 10u] == 1u);
    CHECK(out[(size_t)20 * 100u + 10u] == 0u);

    /* Exclusion form: painted becomes 0, everything else 255. */
    CHECK(kmask_expand_exclude(mask, 1u, out, 100u * 50u));
    CHECK(out[0] == 0u);
    CHECK(out[50] == 255u);
    CHECK(out[(size_t)20 * 100u + 10u] == 255u);

    /* A region nobody painted excludes nothing. */
    CHECK(kmask_expand_exclude(mask, 9u, out, 100u * 50u));
    CHECK(out[0] == 255u);

    /* Wrong size is refused rather than writing past the buffer. */
    CHECK(!kmask_expand(mask, out, 10u));
    CHECK(!kmask_expand(mask, NULL, 100u * 50u));

    kmask_free(mask);
    free(out);
    return true;
}

static bool
test_round_trip_preserves_everything(void)
{
    kmask *mask = NULL;
    kmask *back = NULL;
    uint8_t *encoded = NULL;
    size_t size = 0u;

    CHECK(kmask_create(&mask, 1280, 720, 6));
    kmask_fill_rect(mask, 100, 100, 400, 300, 1u);
    kmask_fill_rect(mask, 500, 200, 700, 500, 2u);
    kmask_set_at(mask, 900, 600, 15u);
    CHECK(kmask_region_set_name(mask, 1u, "walkable"));
    CHECK(kmask_region_set_name(mask, 2u, "water"));
    CHECK(kmask_region_set_attr(mask, 2u, "cost", "3"));
    CHECK(kmask_region_set_attr(mask, 15u, "baseline", "412"));
    kmask_region_set_color(mask, 1u, 0x20c020u);

    CHECK(kmask_encode(mask, &encoded, &size));
    CHECK(encoded != NULL && size > 0u);
    CHECK(kmask_decode(&back, encoded, size));
    CHECK(back != NULL);

    /* Geometry. */
    CHECK(kmask_source_width(back) == 1280);
    CHECK(kmask_source_height(back) == 720);
    CHECK(kmask_cell(back) == 6);
    CHECK(kmask_grid_width(back) == kmask_grid_width(mask));

    /* Every cell. */
    for (int cy = 0; cy < kmask_grid_height(mask); cy++) {
        for (int cx = 0; cx < kmask_grid_width(mask); cx++) {
            CHECK(kmask_get(back, cx, cy) == kmask_get(mask, cx, cy));
        }
    }

    /* Metadata rides in the same file, so it cannot arrive without the
     * bitmap it describes. */
    CHECK(strcmp(kmask_region_name(back, 1u), "walkable") == 0);
    CHECK(strcmp(kmask_region_name(back, 2u), "water") == 0);
    CHECK(strcmp(kmask_region_attr(back, 2u, "cost"), "3") == 0);
    CHECK(strcmp(kmask_region_attr(back, 15u, "baseline"), "412") == 0);
    CHECK(kmask_region_color(back, 1u) == 0x20c020u);

    kmask_free(back);
    kmask_free(mask);
    free(encoded);
    return true;
}

/* It has to be a real PNG, because being openable in any image viewer is
 * most of why the format was chosen. */
static bool
test_output_is_a_valid_png(void)
{
    static const uint8_t signature[8] = {
        137u, 'P', 'N', 'G', '\r', '\n', 26u, '\n'
    };
    kmask *mask = NULL;
    uint8_t *encoded = NULL;
    size_t size = 0u;
    size_t at = 8u;
    bool saw_ihdr = false;
    bool saw_plte = false;
    bool saw_idat = false;
    bool saw_iend = false;

    CHECK(kmask_create(&mask, 64, 32, 2));
    kmask_fill_rect(mask, 0, 0, 32, 16, 1u);
    CHECK(kmask_encode(mask, &encoded, &size));
    CHECK(size > 8u);
    CHECK(memcmp(encoded, signature, 8u) == 0);

    /* Walk the chunks and check every length and CRC, which is what any
     * real decoder will do. */
    while (at + 12u <= size) {
        const uint32_t length =
            ((uint32_t)encoded[at] << 24) | ((uint32_t)encoded[at + 1] << 16) |
            ((uint32_t)encoded[at + 2] << 8) | (uint32_t)encoded[at + 3];
        const char *type = (const char *)(encoded + at + 4u);

        CHECK(length <= size - at - 12u);
        if (memcmp(type, "IHDR", 4u) == 0) {
            saw_ihdr = true;
            CHECK(length == 13u);
            CHECK(encoded[at + 8u + 8u] == 8u);    /* bit depth */
            CHECK(encoded[at + 8u + 9u] == 3u);    /* palette */
        } else if (memcmp(type, "PLTE", 4u) == 0) {
            saw_plte = true;
            CHECK(length % 3u == 0u);
        } else if (memcmp(type, "IDAT", 4u) == 0) {
            saw_idat = true;
        } else if (memcmp(type, "IEND", 4u) == 0) {
            saw_iend = true;
            CHECK(length == 0u);
        }
        at += 12u + length;
    }
    CHECK(at == size);   /* chunks account for the whole file exactly */
    CHECK(saw_ihdr && saw_plte && saw_idat && saw_iend);

    kmask_free(mask);
    free(encoded);
    return true;
}

static bool
test_decode_rejects_what_it_should(void)
{
    static const uint8_t signature[8] = {
        137u, 'P', 'N', 'G', '\r', '\n', 26u, '\n'
    };
    kmask *mask = NULL;
    kmask *back = NULL;
    uint8_t *encoded = NULL;
    size_t size = 0u;
    uint8_t *damaged;

    CHECK(!kmask_decode(&back, NULL, 0u));
    CHECK(!kmask_decode(&back, signature, 8u));
    CHECK(!kmask_decode(NULL, signature, 8u));

    /* Not a PNG at all. */
    CHECK(!kmask_decode(&back, (const uint8_t *)"not a png at all", 16u));

    CHECK(kmask_create(&mask, 32, 32, 4));
    kmask_fill_rect(mask, 0, 0, 16, 16, 1u);
    CHECK(kmask_encode(mask, &encoded, &size));

    /* A PNG that is not one of ours: the metadata chunk is what carries
     * the geometry, and without it there is nothing to reconstruct. */
    damaged = malloc(size);
    CHECK(damaged != NULL);
    (void)memcpy(damaged, encoded, size);
    for (size_t i = 0u; i + 4u <= size; i++) {
        if (memcmp(damaged + i, "tEXt", 4u) == 0) {
            (void)memcpy(damaged + i, "zEXt", 4u);   /* make it unknown */
            break;
        }
    }
    CHECK(!kmask_decode(&back, damaged, size));
    CHECK(back == NULL);

    /* Truncation. */
    CHECK(!kmask_decode(&back, encoded, size / 2u));

    free(damaged);
    free(encoded);
    kmask_free(mask);
    return true;
}

static bool
test_file_round_trip(void)
{
    kmask *mask = NULL;
    kmask *back = NULL;
    char path[256];

    (void)snprintf(path, sizeof(path), "build/mask-%ld.png", (long)getpid());
    CHECK(kmask_create(&mask, 200, 100, 5));
    kmask_fill_rect(mask, 10, 10, 60, 40, 4u);
    CHECK(kmask_region_set_name(mask, 4u, "tree"));

    CHECK(kmask_save(mask, path));
    CHECK(kmask_load(&back, path));
    CHECK(kmask_get_at(back, 20, 20) == 4u);
    CHECK(strcmp(kmask_region_name(back, 4u), "tree") == 0);

    /* kmask_load() clears the out-pointer before doing anything, so a
     * caller reusing one must free what it already holds. */
    kmask_free(back);
    back = NULL;
    CHECK(!kmask_load(&back, "build/definitely-not-here.png"));
    CHECK(back == NULL);
    CHECK(!kmask_save(mask, "build/no/such/dir/x.png"));

    (void)remove(path);
    kmask_free(back);
    kmask_free(mask);
    return true;
}

/*
 * kmask_import() is the inverse of kmask_expand(), and at a cell of 1 it
 * has to be exactly that: a game's own on-disk mask goes in and comes
 * back out unchanged, or the conversion silently edits the asset.
 */
static bool
test_import_round_trips(void)
{
    kmask *mask = NULL;
    uint8_t *source = NULL;
    uint8_t *back = NULL;
    const size_t pixels = 64u * 48u;

    source = malloc(pixels);
    back = malloc(pixels);
    CHECK(source != NULL && back != NULL);
    for (size_t i = 0u; i < pixels; i++) {
        source[i] = (uint8_t)(i % 5u == 0u ? 0u : 1u + (i % 7u));
    }
    CHECK(kmask_create(&mask, 64, 48, 1));
    CHECK(kmask_import(mask, source, pixels));
    CHECK(kmask_expand(mask, back, pixels));
    CHECK(memcmp(source, back, pixels) == 0);

    /* It replaces rather than merges: anything painted before is gone. */
    kmask_fill_rect(mask, 0, 0, 64, 48, 9u);
    CHECK(kmask_import(mask, source, pixels));
    CHECK(kmask_expand(mask, back, pixels));
    CHECK(memcmp(source, back, pixels) == 0);

    CHECK(!kmask_import(mask, source, pixels - 1u));
    CHECK(!kmask_import(mask, NULL, pixels));
    CHECK(!kmask_import(NULL, source, pixels));

    free(back);
    free(source);
    kmask_free(mask);
    return true;
}

/* Above a cell of 1 the pixels in a cell disagree and the rule has to be
 * stated: the commonest non-zero wins, and a cell is only 0 when nothing
 * in it is set.  Sampling one corner instead would drop the thin parts of
 * a region, which is exactly what a walk-behind edge is made of. */
static bool
test_import_reconciles_a_cell(void)
{
    kmask *mask = NULL;
    uint8_t source[8u * 4u];

    CHECK(kmask_create(&mask, 8, 4, 4));
    CHECK(kmask_grid_width(mask) == 2 && kmask_grid_height(mask) == 1);

    (void)memset(source, 0, sizeof(source));
    /* Left cell: three 2s and two 5s, so 2 wins on count. */
    source[0] = 2u; source[1] = 2u; source[2] = 5u; source[3] = 5u;
    source[8] = 2u;
    /* Right cell: a single set pixel among fifteen zeros still counts,
     * because a region that covers part of a cell covers the cell. */
    source[8 + 5] = 7u;
    CHECK(kmask_import(mask, source, sizeof(source)));
    CHECK(kmask_get(mask, 0, 0) == 2u);
    CHECK(kmask_get(mask, 1, 0) == 7u);

    /* A tie keeps the lower id, so the same input always gives the same
     * map rather than depending on scan order. */
    (void)memset(source, 0, sizeof(source));
    source[0] = 6u;
    source[1] = 3u;
    CHECK(kmask_import(mask, source, sizeof(source)));
    CHECK(kmask_get(mask, 0, 0) == 3u);

    /* Nothing set anywhere in the cell is the only way to get 0. */
    CHECK(kmask_get(mask, 1, 0) == 0u);
    kmask_free(mask);
    return true;
}

/*
 * The reconciliation rule, restated pixel by pixel: count what falls in a
 * cell, the commonest non-zero value wins, ties keep the lowest id.  The
 * library may count however it likes, but against any input - edge cells
 * the image only partly covers included - it has to land on the same
 * winner this naive count does.
 */
static uint8_t naive_winner(const uint8_t *values, int source_w,
                            int source_h, int cell, int cx, int cy)
{
    size_t counts[256] = {0u};
    unsigned best = 0u;
    size_t best_count = 0u;
    const int x1 = (cx + 1) * cell < source_w ? (cx + 1) * cell : source_w;
    const int y1 = (cy + 1) * cell < source_h ? (cy + 1) * cell : source_h;

    for (int y = cy * cell; y < y1; y++) {
        for (int x = cx * cell; x < x1; x++) {
            counts[values[(size_t)y * (size_t)source_w + (size_t)x]]++;
        }
    }
    for (unsigned region = 1u; region <= 255u; region++) {
        if (counts[region] > best_count) {
            best_count = counts[region];
            best = region;
        }
    }
    return (uint8_t)best;
}

static bool
test_import_matches_a_naive_count(void)
{
    static const struct { int w; int h; int cell; } shapes[] = {
        {37, 23, 5},   /* the last row and column of cells run past the
                        * image, so their count covers fewer pixels */
        {64, 48, 4},
        {33, 21, 2}
    };
    unsigned local_seed = 31u;

    for (size_t s = 0u; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
        const int w = shapes[s].w;
        const int h = shapes[s].h;
        kmask *mask = NULL;
        uint8_t *values = malloc((size_t)w * (size_t)h);

        CHECK(values != NULL);
        for (size_t i = 0u; i < (size_t)w * (size_t)h; i++) {
            local_seed = local_seed * 1103515245u + 12345u;
            /* Mostly small ids with runs of zeros, plus occasional ids
             * from the top of the range, and enough repetition that many
             * cells contain genuine ties. */
            switch ((local_seed >> 16) % 6u) {
            case 0u: values[i] = 0u; break;
            case 1u: values[i] = 200u; break;
            case 2u: values[i] = 255u; break;
            default: values[i] = (uint8_t)(1u + (local_seed >> 16) % 3u);
            }
        }
        CHECK(kmask_create(&mask, w, h, shapes[s].cell));
        CHECK(kmask_import(mask, values, (size_t)w * (size_t)h));
        for (int cy = 0; cy < kmask_grid_height(mask); cy++) {
            for (int cx = 0; cx < kmask_grid_width(mask); cx++) {
                const uint8_t expected =
                    naive_winner(values, w, h, shapes[s].cell, cx, cy);

                if (kmask_get(mask, cx, cy) != expected) {
                    (void)fprintf(stderr,
                                  "  %dx%d cell %d: cell %d,%d holds %u, "
                                  "the count says %u\n",
                                  w, h, shapes[s].cell, cx, cy,
                                  kmask_get(mask, cx, cy), expected);
                    return false;
                }
            }
        }
        kmask_free(mask);
        free(values);
    }
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"geometry and rejections", test_geometry_and_rejections},
        {"painting in both coordinate spaces",
         test_painting_in_both_coordinate_spaces},
        {"region names and attributes", test_region_names_and_attributes},
        {"expansion polarity", test_expansion_polarity},
        {"import round trips", test_import_round_trips},
        {"import reconciles a cell", test_import_reconciles_a_cell},
        {"import matches a naive count", test_import_matches_a_naive_count},
        {"round trip preserves everything",
         test_round_trip_preserves_everything},
        {"output is a valid png", test_output_is_a_valid_png},
        {"decode rejects what it should", test_decode_rejects_what_it_should},
        {"file round trip", test_file_round_trip}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
