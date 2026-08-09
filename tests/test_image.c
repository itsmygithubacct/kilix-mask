#include "kilix_mask_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "image_fixtures.h"

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define FIXTURE_W 5
#define FIXTURE_H 3

static bool
check_fixture(const char *name, const uint8_t *data, size_t size,
              const uint32_t *expected)
{
    sr_canvas canvas;
    bool ok = true;

    if (!kmask_image_decode(&canvas, data, size)) {
        (void)fprintf(stderr, "  %s: refused (%s)\n", name,
                      kmask_image_error() != NULL ? kmask_image_error()
                                                  : "no reason given");
        return false;
    }
    if (canvas.w != FIXTURE_W || canvas.h != FIXTURE_H) {
        (void)fprintf(stderr, "  %s: got %dx%d\n", name, canvas.w, canvas.h);
        sr_canvas_free(&canvas);
        return false;
    }
    for (int i = 0; i < FIXTURE_W * FIXTURE_H; i++) {
        if (canvas.px[i] != expected[i]) {
            (void)fprintf(stderr,
                          "  %s: pixel %d is %08X, an independent decoder "
                          "reads %08X\n",
                          name, i, canvas.px[i], expected[i]);
            ok = false;
        }
    }
    sr_canvas_free(&canvas);
    return ok;
}

/*
 * Real files, written by an encoder that has nothing to do with this one,
 * against the pixels an independent decoder reads back from them.
 *
 * Round-tripping our own output would only prove the two halves of this
 * module agree with each other, which is exactly the mistake that lets a
 * reader be confidently wrong about every file it did not write.
 */
static bool
test_real_files_of_every_kind(void)
{
#define FIXTURE(name)                                                         \
    CHECK(check_fixture(#name, fixture_##name, sizeof(fixture_##name),        \
                        expect_##name))

    FIXTURE(grey8);        /* colour 0, depth 8 */
    FIXTURE(grey1);        /* colour 0, depth 1, stretched to full range */
    FIXTURE(grey16);       /* colour 0, depth 16, high byte kept */
    FIXTURE(rgb8);         /* colour 2 */
    FIXTURE(palette8);     /* colour 3, depth 8 */
    FIXTURE(palette4);     /* colour 3, sub-byte indices */
    FIXTURE(palette_trns); /* colour 3 with tRNS */
    FIXTURE(greyalpha8);   /* colour 4 */
    FIXTURE(rgba8);        /* colour 6 */
#undef FIXTURE
    return true;
}

/* ------------------------- a forced-filter encoder ---------------------- */

typedef struct growable {
    uint8_t *data;
    size_t used;
    size_t capacity;
} growable;

static void put(growable *buffer, const void *bytes, size_t count)
{
    if (count == 0u) {
        /* IEND has an empty body, and memcpy from NULL is undefined even
         * for zero bytes. */
        return;
    }
    if (buffer->used + count > buffer->capacity) {
        const size_t grown = (buffer->used + count) * 2u + 64u;
        uint8_t *bigger = realloc(buffer->data, grown);

        if (bigger == NULL) {
            abort();
        }
        buffer->data = bigger;
        buffer->capacity = grown;
    }
    (void)memcpy(buffer->data + buffer->used, bytes, count);
    buffer->used += count;
}

static void put_u32(growable *buffer, uint32_t value)
{
    const uint8_t bytes[4] = {
        (uint8_t)(value >> 24), (uint8_t)(value >> 16),
        (uint8_t)(value >> 8), (uint8_t)value
    };

    put(buffer, bytes, 4u);
}

static void put_chunk(growable *buffer, const char *type, const uint8_t *body,
                      size_t length)
{
    uLong crc = crc32(0uL, Z_NULL, 0);

    put_u32(buffer, (uint32_t)length);
    put(buffer, type, 4u);
    put(buffer, body, length);
    crc = crc32(crc, (const Bytef *)type, 4u);
    crc = crc32(crc, body, (uInt)length);
    put_u32(buffer, (uint32_t)crc);
}

static int paeth_predict(int a, int b, int c)
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
 * Encode 8-bit RGB with every scanline forced to one filter type.
 *
 * Encoders pick filters adaptively, so a handful of sample files will
 * exercise whichever ones their content happened to favour and quietly
 * leave the rest untested.  Forcing each in turn is the only way to know
 * all five are right.
 */
static uint8_t *encode_rgb(const uint8_t *rgb, int width, int height,
                           int filter, size_t *out_size)
{
    growable file = {NULL, 0u, 0u};
    growable raw = {NULL, 0u, 0u};
    static const uint8_t signature[8] = {
        137u, 'P', 'N', 'G', '\r', '\n', 26u, '\n'
    };
    uint8_t header[13];
    const size_t stride = (size_t)width * 3u;
    uint8_t *line = malloc(stride);
    uint8_t *compressed;
    uLongf compressed_size;

    if (line == NULL) {
        abort();
    }
    put(&file, signature, 8u);
    header[0] = (uint8_t)(width >> 24); header[1] = (uint8_t)(width >> 16);
    header[2] = (uint8_t)(width >> 8);  header[3] = (uint8_t)width;
    header[4] = (uint8_t)(height >> 24); header[5] = (uint8_t)(height >> 16);
    header[6] = (uint8_t)(height >> 8);  header[7] = (uint8_t)height;
    header[8] = 8u;   /* depth */
    header[9] = 2u;   /* truecolour */
    header[10] = 0u;
    header[11] = 0u;
    header[12] = 0u;  /* not interlaced */
    put_chunk(&file, "IHDR", header, sizeof(header));

    for (int y = 0; y < height; y++) {
        const uint8_t *current = rgb + (size_t)y * stride;
        const uint8_t *above = y > 0 ? current - stride : NULL;
        const uint8_t type = (uint8_t)filter;

        for (size_t i = 0u; i < stride; i++) {
            const int left = i >= 3u ? current[i - 3u] : 0;
            const int up = above != NULL ? above[i] : 0;
            const int corner = (above != NULL && i >= 3u) ? above[i - 3u] : 0;
            int value;

            switch (filter) {
            case 1:  value = current[i] - left; break;
            case 2:  value = current[i] - up; break;
            case 3:  value = current[i] - (left + up) / 2; break;
            case 4:  value = current[i] - paeth_predict(left, up, corner);
                     break;
            default: value = current[i]; break;
            }
            line[i] = (uint8_t)value;
        }
        put(&raw, &type, 1u);
        put(&raw, line, stride);
    }
    free(line);

    compressed_size = compressBound((uLong)raw.used);
    compressed = malloc(compressed_size);
    if (compressed == NULL ||
        compress(compressed, &compressed_size, raw.data,
                 (uLong)raw.used) != Z_OK) {
        abort();
    }
    put_chunk(&file, "IDAT", compressed, compressed_size);
    put_chunk(&file, "IEND", NULL, 0u);
    free(compressed);
    free(raw.data);
    *out_size = file.used;
    return file.data;
}

static bool
test_every_scanline_filter(void)
{
    enum { W = 23, H = 17 };
    static uint8_t rgb[(size_t)W * H * 3u];

    /* Content with gradients and edges in both directions, so no filter
     * degenerates into another. */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *pixel = rgb + ((size_t)y * W + (size_t)x) * 3u;

            pixel[0] = (uint8_t)(x * 11 + y);
            pixel[1] = (uint8_t)(y * 7 + (x > W / 2 ? 200 : 0));
            pixel[2] = (uint8_t)((x * y) & 0xFF);
        }
    }
    for (int filter = 0; filter <= 4; filter++) {
        size_t size = 0u;
        uint8_t *file = encode_rgb(rgb, W, H, filter, &size);
        sr_canvas canvas;
        bool ok = true;

        if (!kmask_image_decode(&canvas, file, size)) {
            (void)fprintf(stderr, "  filter %d refused (%s)\n", filter,
                          kmask_image_error());
            free(file);
            return false;
        }
        for (int i = 0; i < W * H && ok; i++) {
            const uint32_t want = 0xFF000000u |
                                  ((uint32_t)rgb[i * 3] << 16) |
                                  ((uint32_t)rgb[i * 3 + 1] << 8) |
                                  (uint32_t)rgb[i * 3 + 2];

            if (canvas.px[i] != want) {
                (void)fprintf(stderr, "  filter %d: pixel %d %08X != %08X\n",
                              filter, i, canvas.px[i], want);
                ok = false;
            }
        }
        sr_canvas_free(&canvas);
        free(file);
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Split across chunks is legal and common; zlib needs the stream whole. */
static bool
test_image_data_split_across_chunks(void)
{
    enum { W = 9, H = 5 };
    static uint8_t rgb[(size_t)W * H * 3u];
    size_t size = 0u;
    uint8_t *file;
    uint8_t *split;
    size_t split_size;
    size_t idat_at = 0u;
    uint32_t idat_length;
    sr_canvas canvas;
    growable rebuilt = {NULL, 0u, 0u};
    bool ok = true;

    for (size_t i = 0u; i < sizeof(rgb); i++) {
        rgb[i] = (uint8_t)(i * 13u + 7u);
    }
    file = encode_rgb(rgb, W, H, 1, &size);

    /* Find the single IDAT and re-emit it as two. */
    for (size_t at = 8u; at + 12u <= size;) {
        const uint32_t length = ((uint32_t)file[at] << 24) |
                                ((uint32_t)file[at + 1] << 16) |
                                ((uint32_t)file[at + 2] << 8) |
                                (uint32_t)file[at + 3];

        if (memcmp(file + at + 4u, "IDAT", 4u) == 0) {
            idat_at = at;
            break;
        }
        at += 12u + length;
    }
    CHECK(idat_at != 0u);
    idat_length = ((uint32_t)file[idat_at] << 24) |
                  ((uint32_t)file[idat_at + 1] << 16) |
                  ((uint32_t)file[idat_at + 2] << 8) |
                  (uint32_t)file[idat_at + 3];
    CHECK(idat_length > 4u);

    put(&rebuilt, file, idat_at);
    put_chunk(&rebuilt, "IDAT", file + idat_at + 8u, 3u);
    put_chunk(&rebuilt, "IDAT", file + idat_at + 8u + 3u, idat_length - 3u);
    put(&rebuilt, file + idat_at + 12u + idat_length,
        size - idat_at - 12u - idat_length);
    split = rebuilt.data;
    split_size = rebuilt.used;

    CHECK(kmask_image_decode(&canvas, split, split_size));
    for (int i = 0; i < W * H && ok; i++) {
        const uint32_t want = 0xFF000000u | ((uint32_t)rgb[i * 3] << 16) |
                              ((uint32_t)rgb[i * 3 + 1] << 8) |
                              (uint32_t)rgb[i * 3 + 2];

        ok = canvas.px[i] == want;
    }
    sr_canvas_free(&canvas);
    free(split);
    free(file);
    CHECK(ok);
    return true;
}

/* Interlacing is refused by name, so the message can say what to do
 * about it rather than "could not read". */
static bool
test_interlaced_is_refused_by_name(void)
{
    enum { W = 6, H = 4 };
    static uint8_t rgb[(size_t)W * H * 3u];
    size_t size = 0u;
    uint8_t *file = encode_rgb(rgb, W, H, 0, &size);
    sr_canvas canvas;

    /* The interlace byte is the last of IHDR's thirteen: 8 signature
     * bytes, 4 length, 4 type, then the body. */
    file[8u + 4u + 4u + 12u] = 1u;
    CHECK(!kmask_image_decode(&canvas, file, size));
    CHECK(kmask_image_error() != NULL);
    CHECK(strstr(kmask_image_error(), "interlac") != NULL);
    free(file);
    return true;
}

static bool
test_rejections(void)
{
    static const uint8_t not_a_png[16] = {'h', 'e', 'l', 'l', 'o'};
    sr_canvas canvas;
    size_t size = 0u;
    uint8_t *file;

    CHECK(!kmask_image_decode(NULL, fixture_rgb8, sizeof(fixture_rgb8)));
    CHECK(!kmask_image_decode(&canvas, NULL, 10u));
    CHECK(!kmask_image_decode(&canvas, not_a_png, sizeof(not_a_png)));
    CHECK(strstr(kmask_image_error(), "PNG") != NULL);

    /* A signature and nothing else. */
    CHECK(!kmask_image_decode(&canvas, fixture_rgb8, 20u));

    file = encode_rgb((const uint8_t[3]){1u, 2u, 3u}, 1, 1, 0, &size);
    CHECK(kmask_image_decode(&canvas, file, size));
    CHECK(canvas.px[0] == 0xFF010203u);
    sr_canvas_free(&canvas);

    /* A colour type nobody defines. */
    file[8u + 4u + 4u + 9u] = 7u;
    CHECK(!kmask_image_decode(&canvas, file, size));
    CHECK(strstr(kmask_image_error(), "colour type") != NULL);

    /* A depth the spec does not allow for truecolour. */
    file[8u + 4u + 4u + 9u] = 2u;
    file[8u + 4u + 4u + 8u] = 4u;
    CHECK(!kmask_image_decode(&canvas, file, size));
    CHECK(strstr(kmask_image_error(), "depth") != NULL);

    file[8u + 4u + 4u + 8u] = 8u;

    /* Losing the terminator alone is tolerated: IEND carries no pixels,
     * and every one of them is still here.  A photograph that opens is
     * better than a photograph refused over a missing full stop. */
    CHECK(kmask_image_decode(&canvas, file, size - 12u));
    CHECK(canvas.px[0] == 0xFF010203u);
    sr_canvas_free(&canvas);

    /* Chopped into the image data is a different matter - and it is the
     * inflated size, not the chunk framing, that settles it. */
    CHECK(!kmask_image_decode(&canvas, file, size - 20u));
    free(file);

    CHECK(!kmask_image_load(&canvas, "/definitely/not/here.png"));
    CHECK(!kmask_image_load(NULL, "x"));
    CHECK(!kmask_image_load(&canvas, NULL));
    return true;
}

/*
 * The reader is chosen by what the file contains, not what it is called.
 * Plates get renamed and converted; opening one should not depend on
 * whether the extension survived.
 */
static bool
test_format_is_chosen_by_content(void)
{
    const char *png_path = "build/test-image-content.png";
    const char *ppm_path = "build/test-image-content.ppm";
    sr_canvas canvas;
    FILE *file;

    /* A PPM wearing a .png name. */
    file = fopen(png_path, "wb");
    CHECK(file != NULL);
    (void)fprintf(file, "P6\n2 1\n255\n");
    (void)fwrite("\xFF\x00\x00\x00\xFF\x00", 1u, 6u, file);
    (void)fclose(file);
    CHECK(kmask_image_load(&canvas, png_path));
    CHECK(canvas.w == 2 && canvas.h == 1);
    sr_canvas_free(&canvas);

    /* ...and a PNG wearing a .ppm name. */
    file = fopen(ppm_path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(fixture_rgb8, 1u, sizeof(fixture_rgb8), file) ==
          sizeof(fixture_rgb8));
    (void)fclose(file);
    CHECK(kmask_image_load(&canvas, ppm_path));
    CHECK(canvas.w == FIXTURE_W && canvas.h == FIXTURE_H);
    CHECK(canvas.px[0] == expect_rgb8[0]);
    sr_canvas_free(&canvas);

    (void)remove(png_path);
    (void)remove(ppm_path);
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
        {"real files of every kind", test_real_files_of_every_kind},
        {"every scanline filter", test_every_scanline_filter},
        {"image data split across chunks",
         test_image_data_split_across_chunks},
        {"interlaced is refused by name",
         test_interlaced_is_refused_by_name},
        {"rejections", test_rejections},
        {"format is chosen by content", test_format_is_chosen_by_content}
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
