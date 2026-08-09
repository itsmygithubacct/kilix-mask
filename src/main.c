/*
 * kilix-mask: paint a region map over a picture.
 *
 * Three ways in.  The interactive editor is the point of the thing; the
 * other two exist because a painting tool that can only be checked by a
 * person looking at it is a painting tool nobody can check.
 *
 *   --render   compose one frame and write it out, no terminal involved
 *   --selftest exercise the installed binary end to end
 */

#include "kmask_run.h"
#include "kmask_ui.h"

#include "kilix_mask_image.h"
#include "kilix_mask_rects.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_RENDER_W 960
#define DEFAULT_RENDER_H 600

typedef struct arguments {
    const char *mask_path;
    const char *image_path;
    const char *render_path;
    int cell;
    int width;
    int height;
    bool selftest;
    bool help;
} arguments;

static void usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "usage: kilix-mask [options] [mask.png]\n"
        "\n"
        "Paint a region map over a picture: which parts of a camera view to\n"
        "ignore, which parts of a room can be walked on.\n"
        "\n"
        "  --image FILE     picture to paint over: PNG or binary PPM\n"
        "  --cell N         source pixels per map cell for a new mask\n"
        "                   (default 1; larger stores a grid)\n"
        "  --size WxH       source size for a new mask with no picture\n"
        "  --render FILE    compose one frame to a PPM and exit\n"
        "  --selftest       check this build end to end and exit\n"
        "  --help\n"
        "\n"
        "An existing mask is loaded and its geometry used; otherwise one is\n"
        "created from the picture's size, or from --size.\n"
        "\n"
        "The picture's format is chosen by what the file contains, not by\n"
        "what it is called.  Interlaced PNG is the one valid file refused;\n"
        "convert it first:\n"
        "  ffmpeg -i snapshot.jpg plate.png\n");
}

static bool parse_size(const char *text, int *width, int *height)
{
    char *end = NULL;
    long w;
    long h;

    w = strtol(text, &end, 10);
    if (end == text || (*end != 'x' && *end != 'X')) {
        return false;
    }
    h = strtol(end + 1, &end, 10);
    if (*end != '\0' || w <= 0 || h <= 0 || w > 100000 || h > 100000) {
        return false;
    }
    *width = (int)w;
    *height = (int)h;
    return true;
}

static bool parse_arguments(int argc, char **argv, arguments *out)
{
    (void)memset(out, 0, sizeof(*out));
    out->cell = 1;

    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        const bool has_value = i + 1 < argc;

        if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
            out->help = true;
        } else if (strcmp(argument, "--selftest") == 0) {
            out->selftest = true;
        } else if (strcmp(argument, "--image") == 0 && has_value) {
            out->image_path = argv[++i];
        } else if (strcmp(argument, "--render") == 0 && has_value) {
            out->render_path = argv[++i];
        } else if (strcmp(argument, "--cell") == 0 && has_value) {
            out->cell = atoi(argv[++i]);
            if (out->cell < 1) {
                (void)fprintf(stderr, "kilix-mask: --cell must be at least 1\n");
                return false;
            }
        } else if (strcmp(argument, "--size") == 0 && has_value) {
            if (!parse_size(argv[++i], &out->width, &out->height)) {
                (void)fprintf(stderr, "kilix-mask: --size wants WxH\n");
                return false;
            }
        } else if (argument[0] == '-' && argument[1] != '\0') {
            (void)fprintf(stderr, "kilix-mask: unknown option %s\n", argument);
            return false;
        } else if (out->mask_path == NULL) {
            out->mask_path = argument;
        } else {
            (void)fprintf(stderr, "kilix-mask: one mask at a time\n");
            return false;
        }
    }
    return true;
}

/*
 * Load the mask if it is there, create one if it is not.
 *
 * An existing mask decides the geometry, because its cell size and source
 * size are what its contents mean; --cell and --size describe a new one
 * only.  Silently re-gridding a loaded mask would move every painted cell.
 */
static bool open_mask(const arguments *options, const sr_canvas *image,
                      kmask **mask)
{
    int width = options->width;
    int height = options->height;

    if (options->mask_path != NULL && access(options->mask_path, R_OK) == 0) {
        if (!kmask_load(mask, options->mask_path)) {
            (void)fprintf(stderr, "kilix-mask: %s is not a mask this "
                                  "reads\n", options->mask_path);
            return false;
        }
        if (image != NULL &&
            (kmask_source_width(*mask) != image->w ||
             kmask_source_height(*mask) != image->h)) {
            (void)fprintf(stderr,
                          "kilix-mask: mask is %dx%d but the picture is "
                          "%dx%d\n",
                          kmask_source_width(*mask),
                          kmask_source_height(*mask), image->w, image->h);
            kmask_free(*mask);
            *mask = NULL;
            return false;
        }
        return true;
    }
    if (image != NULL) {
        width = image->w;
        height = image->h;
    }
    if (width <= 0 || height <= 0) {
        (void)fprintf(stderr, "kilix-mask: need --image or --size to make a "
                              "new mask\n");
        return false;
    }
    if (!kmask_create(mask, width, height, options->cell)) {
        (void)fprintf(stderr, "kilix-mask: could not create a %dx%d mask\n",
                      width, height);
        return false;
    }
    return true;
}

static int render(const arguments *options, kmaskedit *editor, kmask *mask)
{
    sr_canvas frame;
    int width = options->width > 0 ? options->width : DEFAULT_RENDER_W;
    int height = options->height > 0 ? options->height : DEFAULT_RENDER_H;

    if (!sr_canvas_init(&frame, width, height)) {
        return 1;
    }
    if (!kmaskedit_set_view(editor, width, height - KMASK_UI_STATUS_HEIGHT)) {
        sr_canvas_free(&frame);
        return 1;
    }
    /* A cursor in the middle, so the render shows the tool rather than a
     * bare picture with a tint on it. */
    kmaskedit_hover(editor, width / 2, (height - KMASK_UI_STATUS_HEIGHT) / 2);
    kmaskedit_compose(editor, &frame, 0, 0);
    kmask_ui_status(&frame, height - KMASK_UI_STATUS_HEIGHT, width, editor,
                    options->mask_path, NULL);
    if (!sr_write_ppm(&frame, options->render_path)) {
        (void)fprintf(stderr, "kilix-mask: could not write %s\n",
                      options->render_path);
        sr_canvas_free(&frame);
        return 1;
    }
    sr_canvas_free(&frame);
    (void)mask;
    return 0;
}

#define TEST(name, condition)                                                 \
    do {                                                                      \
        const bool passed = (condition);                                      \
        (void)printf("%s %s\n", passed ? "ok" : "not ok", (name));            \
        if (!passed) {                                                        \
            return 1;                                                         \
        }                                                                     \
    } while (false)

/*
 * A smoke test of this binary, not a substitute for `make test`.
 *
 * The unit tests need the source tree; this needs only the installed
 * program, so it answers a different question - whether the thing that
 * actually got deployed works - and it is the only check available on a
 * machine that has the tool but not the repository.
 */
static int selftest(void)
{
    kmask *mask = NULL;
    kmask *loaded = NULL;
    kmaskedit *editor = NULL;
    sr_canvas plate;
    sr_canvas frame;
    kmask_rect bounds;
    kmask_rect holes[64];
    size_t needed = 0u;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    bool ok;

    TEST("a mask can be created", kmask_create(&mask, 320, 200, 4));
    TEST("regions carry names",
         kmask_region_set_name(mask, 1u, "walkable") &&
             strcmp(kmask_region_name(mask, 1u), "walkable") == 0);

    kmask_fill_rect(mask, 40, 30, 200, 150, 1u);
    kmask_fill_rect(mask, 80, 60, 120, 100, 0u);
    TEST("painting lands where it was asked to",
         kmask_get_at(mask, 50, 40) == 1u && kmask_get_at(mask, 90, 70) == 0u);

    TEST("a mask encodes", kmask_encode(mask, &encoded, &encoded_size) &&
                               encoded_size > 8u);
    TEST("and it is a PNG", memcmp(encoded, "\x89PNG\r\n\x1a\n", 8u) == 0);
    TEST("and it decodes back", kmask_decode(&loaded, encoded, encoded_size));
    free(encoded);
    TEST("with its geometry", kmask_source_width(loaded) == 320 &&
                                  kmask_cell(loaded) == 4);
    TEST("with its names",
         strcmp(kmask_region_name(loaded, 1u), "walkable") == 0);
    TEST("and its contents", kmask_get_at(loaded, 50, 40) == 1u &&
                                 kmask_get_at(loaded, 90, 70) == 0u);

    TEST("a region decomposes",
         kmask_decompose(mask, 1u, &bounds, holes, 64u, &needed));
    kmask_clear_region(loaded, 1u);
    TEST("and paints back", kmask_apply(loaded, 1u, &bounds, holes, needed));
    ok = true;
    for (int cy = 0; cy < kmask_grid_height(mask) && ok; cy++) {
        for (int cx = 0; cx < kmask_grid_width(mask); cx++) {
            if ((kmask_get(mask, cx, cy) == 1u) !=
                (kmask_get(loaded, cx, cy) == 1u)) {
                ok = false;
                break;
            }
        }
    }
    TEST("cell for cell", ok);
    kmask_free(loaded);

    TEST("a plate loads", sr_canvas_init(&plate, 320, 200));
    for (int i = 0; i < plate.w * plate.h; i++) {
        plate.px[i] = 0xFF204060u;
    }
    TEST("an editor opens", kmaskedit_create(&editor, mask, &plate));
    TEST("a view is set", kmaskedit_set_view(editor, 640, 400));

    kmaskedit_set_region(editor, 2u);
    kmaskedit_press(editor, 100, 100, KMASKEDIT_BUTTON_PAINT);
    kmaskedit_drag(editor, 300, 260);
    kmaskedit_release(editor, 300, 260);
    TEST("a stroke paints", kmaskedit_modified(editor));
    TEST("and undoes", kmaskedit_undo(editor) && !kmaskedit_modified(editor));

    TEST("a scene composes", sr_canvas_init(&frame, 640, 400));
    kmaskedit_hover(editor, 320, 200);
    kmaskedit_compose(editor, &frame, 0, 0);
    ok = false;
    for (int i = 0; i < frame.w * frame.h; i++) {
        if ((frame.px[i] & 0x00FFFFFFu) != 0x00204060u) {
            ok = true;   /* the tint, grid or cursor drew something */
            break;
        }
    }
    TEST("and is not a flat field", ok);

    sr_canvas_free(&frame);
    sr_canvas_free(&plate);
    kmaskedit_free(editor);
    kmask_free(mask);
    (void)printf("selftest passed\n");
    return 0;
}

int main(int argc, char **argv)
{
    arguments options;
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    sr_canvas plate;
    sr_canvas *image = NULL;
    int status;

    if (!parse_arguments(argc, argv, &options)) {
        usage(stderr);
        return 2;
    }
    if (options.help) {
        usage(stdout);
        return 0;
    }
    if (options.selftest) {
        return selftest();
    }
    if (options.image_path != NULL) {
        if (!kmask_image_load(&plate, options.image_path)) {
            const char *reason = kmask_image_error();

            (void)fprintf(stderr, "kilix-mask: %s: %s\n", options.image_path,
                          reason != NULL ? reason : "could not be read");
            /* Interlacing is the one thing a valid PNG can be that this
             * will not read, so name the way out of it. */
            if (reason != NULL && strstr(reason, "interlac") != NULL) {
                (void)fprintf(stderr, "  ffmpeg -i %s plate.ppm\n",
                              options.image_path);
            }
            return 1;
        }
        image = &plate;
    }
    if (!open_mask(&options, image, &mask)) {
        if (image != NULL) {
            sr_canvas_free(image);
        }
        return 1;
    }
    if (!kmaskedit_create(&editor, mask, image)) {
        (void)fprintf(stderr, "kilix-mask: could not open the editor\n");
        kmask_free(mask);
        if (image != NULL) {
            sr_canvas_free(image);
        }
        return 1;
    }

    status = options.render_path != NULL
                 ? render(&options, editor, mask)
                 : kmask_run(editor, mask, options.mask_path);

    kmaskedit_free(editor);
    kmask_free(mask);
    if (image != NULL) {
        sr_canvas_free(image);
    }
    return status;
}
