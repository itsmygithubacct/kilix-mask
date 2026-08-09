#include "kmask_marks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

static const char *PATH = "build/test-marks.tmp";

static bool write_file(const char *text)
{
    FILE *file = fopen(PATH, "w");

    if (file == NULL) {
        return false;
    }
    (void)fputs(text, file);
    (void)fclose(file);
    return true;
}

static bool
test_reads_what_it_should(void)
{
    kmask_marks marks;
    char error[KMASK_MARK_ERROR_MAX];

    CHECK(write_file(
        "# a comment, and the blank line below\n"
        "\n"
        "rect 10 20 30 40 3399FF door to living\n"
        "point 240 235 00FFFF bedroom-postcard\n"
        "   rect 0 0 5 5 FF0000\n"          /* leading space, no label */
        "point -4 -8 112233 off the edge\n"  /* negatives are legal */
    ));
    CHECK(kmask_marks_load(&marks, PATH, error, sizeof(error)));
    CHECK(marks.count == 4u);
    CHECK(marks.dropped == 0u);

    CHECK(marks.items[0].kind == KMASK_MARK_RECT);
    CHECK(marks.items[0].x == 10 && marks.items[0].y == 20);
    CHECK(marks.items[0].w == 30 && marks.items[0].h == 40);
    CHECK(marks.items[0].rgb == 0x3399FFu);
    /* The label is the rest of the line, spaces and all - a door's label
     * is "to living", not "to". */
    CHECK(strcmp(marks.items[0].label, "door to living") == 0);

    CHECK(marks.items[1].kind == KMASK_MARK_POINT);
    CHECK(marks.items[1].x == 240 && marks.items[1].y == 235);
    CHECK(strcmp(marks.items[1].label, "bedroom-postcard") == 0);

    CHECK(marks.items[2].label[0] == '\0');
    CHECK(marks.items[3].x == -4 && marks.items[3].y == -8);
    return true;
}

/*
 * A malformed line is refused rather than skipped.  A door that quietly
 * fails to draw is worse than no annotations at all: the map gets painted
 * around a door that was never shown, and nothing says so.
 */
static bool
test_refuses_what_it_cannot_read(void)
{
    kmask_marks marks;
    char error[KMASK_MARK_ERROR_MAX];

    CHECK(write_file("rect 1 2 3 4 FFFFFF fine\nnonsense\n"));
    CHECK(!kmask_marks_load(&marks, PATH, error, sizeof(error)));
    CHECK(strstr(error, ":2") != NULL);
    /* Nothing is left behind from the lines that did parse. */
    CHECK(marks.count == 0u);

    CHECK(write_file("rect 1 2 3 FFFFFF short\n"));
    CHECK(!kmask_marks_load(&marks, PATH, error, sizeof(error)));

    CHECK(write_file("point 1 2 GGGGGG bad colour\n"));
    CHECK(!kmask_marks_load(&marks, PATH, error, sizeof(error)));

    CHECK(write_file("rect 1 2 3 4 1FFFFFFF too wide\n"));
    CHECK(!kmask_marks_load(&marks, PATH, error, sizeof(error)));

    CHECK(!kmask_marks_load(&marks, "build/definitely-not-here", error,
                            sizeof(error)));
    CHECK(strstr(error, "cannot be opened") != NULL);
    CHECK(!kmask_marks_load(NULL, PATH, error, sizeof(error)));
    CHECK(!kmask_marks_load(&marks, NULL, error, sizeof(error)));
    return true;
}

/* Past capacity the overflow is counted, not silently forgotten: a
 * caller told "12 shown, 508 dropped" can do something about it. */
static bool
test_counts_what_it_dropped(void)
{
    kmask_marks marks;
    char error[KMASK_MARK_ERROR_MAX];
    FILE *file = fopen(PATH, "w");

    CHECK(file != NULL);
    for (int i = 0; i < KMASK_MARK_MAX + 7; i++) {
        (void)fprintf(file, "point %d %d 00FF00 p%d\n", i, i, i);
    }
    (void)fclose(file);

    CHECK(kmask_marks_load(&marks, PATH, error, sizeof(error)));
    CHECK(marks.count == (size_t)KMASK_MARK_MAX);
    CHECK(marks.dropped == 7u);
    return true;
}

/* Drawing is clipped to the view and never touches the mask. */
static bool
test_drawing_stays_in_bounds(void)
{
    kmask *mask = NULL;
    kmaskedit *editor = NULL;
    kmask_marks marks;
    char error[KMASK_MARK_ERROR_MAX];
    sr_canvas frame;
    size_t counts_before[256];
    size_t counts_after[256];

    CHECK(write_file(
        "rect 10 10 40 40 FF0000 inside\n"
        "rect -900 -900 20 20 00FF00 far off\n"
        "point 100000 100000 0000FF also off\n"
    ));
    CHECK(kmask_marks_load(&marks, PATH, error, sizeof(error)));
    CHECK(kmask_create(&mask, 120, 90, 2));
    CHECK(kmaskedit_create(&editor, mask, NULL));
    CHECK(kmaskedit_set_view(editor, 200, 150));
    CHECK(sr_canvas_init(&frame, 200, 150));
    kmask_counts(mask, counts_before);

    kmask_marks_draw(&frame, editor, &marks, 200, 150, true);
    kmask_marks_draw(&frame, editor, &marks, 200, 150, false);
    kmask_marks_draw(NULL, editor, &marks, 200, 150, true);
    kmask_marks_draw(&frame, editor, NULL, 200, 150, true);

    /* Annotations are not the map. */
    kmask_counts(mask, counts_after);
    CHECK(memcmp(counts_before, counts_after, sizeof(counts_before)) == 0);

    sr_canvas_free(&frame);
    kmaskedit_free(editor);
    kmask_free(mask);
    (void)remove(PATH);
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
        {"reads what it should", test_reads_what_it_should},
        {"refuses what it cannot read", test_refuses_what_it_cannot_read},
        {"counts what it dropped", test_counts_what_it_dropped},
        {"drawing stays in bounds", test_drawing_stays_in_bounds}
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
