#include "kmask_marks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LENGTH 512

static void trim_end(char *text)
{
    size_t length = strlen(text);

    while (length > 0u && (text[length - 1u] == '\n' ||
                           text[length - 1u] == '\r' ||
                           text[length - 1u] == ' ' ||
                           text[length - 1u] == '\t')) {
        text[--length] = '\0';
    }
}

/* Advance past one whitespace-separated field, returning where it began
 * and NUL-terminating it.  Returns NULL at the end of the line. */
static char *next_field(char **cursor)
{
    char *start = *cursor;

    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (*start == '\0') {
        *cursor = start;
        return NULL;
    }
    {
        char *end = start;

        while (*end != '\0' && *end != ' ' && *end != '\t') {
            end++;
        }
        if (*end != '\0') {
            *end = '\0';
            end++;
        }
        *cursor = end;
    }
    return start;
}

static bool parse_int(const char *text, int *out)
{
    char *end = NULL;
    long value;

    if (text == NULL) {
        return false;
    }
    value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < -1000000L || value > 1000000L) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parse_colour(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL) {
        return false;
    }
    value = strtoul(text, &end, 16);
    if (end == text || *end != '\0' || value > 0xFFFFFFuL) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

/* Whatever is left of the line, with leading blanks removed. */
static void take_label(char *cursor, char *out)
{
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    (void)snprintf(out, KMASK_MARK_LABEL_MAX, "%s", cursor);
}

bool kmask_marks_load(
    kmask_marks *marks, const char *path, char *error, size_t error_size)
{
    FILE *file;
    char line[LINE_MAX_LENGTH];
    unsigned number = 0u;

    if (marks == NULL || path == NULL) {
        return false;
    }
    (void)memset(marks, 0, sizeof(*marks));
    file = fopen(path, "r");
    if (file == NULL) {
        if (error != NULL) {
            (void)snprintf(error, error_size, "%s cannot be opened", path);
        }
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *cursor = line;
        const char *kind;
        kmask_mark mark;

        number++;
        trim_end(line);
        kind = next_field(&cursor);
        if (kind == NULL || kind[0] == '#') {
            continue;
        }
        (void)memset(&mark, 0, sizeof(mark));

        if (strcmp(kind, "rect") == 0) {
            mark.kind = KMASK_MARK_RECT;
            if (!parse_int(next_field(&cursor), &mark.x) ||
                !parse_int(next_field(&cursor), &mark.y) ||
                !parse_int(next_field(&cursor), &mark.w) ||
                !parse_int(next_field(&cursor), &mark.h) ||
                !parse_colour(next_field(&cursor), &mark.rgb)) {
                goto malformed;
            }
        } else if (strcmp(kind, "point") == 0) {
            mark.kind = KMASK_MARK_POINT;
            if (!parse_int(next_field(&cursor), &mark.x) ||
                !parse_int(next_field(&cursor), &mark.y) ||
                !parse_colour(next_field(&cursor), &mark.rgb)) {
                goto malformed;
            }
        } else {
            goto malformed;
        }
        take_label(cursor, mark.label);
        if (marks->count >= KMASK_MARK_MAX) {
            marks->dropped++;
            continue;
        }
        marks->items[marks->count++] = mark;
    }
    (void)fclose(file);
    return true;

malformed:
    (void)fclose(file);
    if (error != NULL) {
        (void)snprintf(error, error_size, "%s:%u is not a mark", path,
                       number);
    }
    (void)memset(marks, 0, sizeof(*marks));
    return false;
}

void kmask_marks_draw(
    sr_canvas *out,
    const kmaskedit *editor,
    const kmask_marks *marks,
    int width,
    int view_height,
    bool labels)
{
    if (out == NULL || editor == NULL || marks == NULL) {
        return;
    }
    for (size_t i = 0u; i < marks->count; i++) {
        const kmask_mark *mark = &marks->items[i];
        int x0 = 0;
        int y0 = 0;
        int label_y;

        kmaskedit_to_view(editor, mark->x, mark->y, &x0, &y0);

        if (mark->kind == KMASK_MARK_RECT) {
            int x1 = 0;
            int y1 = 0;

            kmaskedit_to_view(editor, mark->x + mark->w, mark->y + mark->h,
                              &x1, &y1);
            if (x1 <= 0 || y1 <= 0 || x0 >= width || y0 >= view_height) {
                continue;   /* wholly off the view */
            }
            sr_stroke_rect(out, (float)x0, (float)y0, (float)(x1 - x0),
                           (float)(y1 - y0), 1.0f, mark->rgb, 0.85f);
            label_y = y0 - 15;
        } else {
            if (x0 < -8 || y0 < -8 || x0 > width + 8 ||
                y0 > view_height + 8) {
                continue;
            }
            /* A cross rather than a dot: a spawn is a coordinate, and a
             * filled dot hides the pixel it is meant to point at. */
            sr_line(out, (float)(x0 - 5), (float)y0, (float)(x0 + 5),
                    (float)y0, 1.0f, mark->rgb, 0.9f, 0, 0);
            sr_line(out, (float)x0, (float)(y0 - 5), (float)x0,
                    (float)(y0 + 5), 1.0f, mark->rgb, 0.9f, 0, 0);
            sr_ring(out, (float)x0, (float)y0, 4.0f, 1.0f, mark->rgb, 0.7f);
            label_y = y0 + 7;
        }
        if (labels && mark->label[0] != '\0' && label_y >= 0 &&
            label_y < view_height - 8) {
            sr_text_shadow(out, (float)(x0 + 2), (float)label_y, mark->label,
                           mark->rgb, 0.95f, 1);
        }
    }
}
