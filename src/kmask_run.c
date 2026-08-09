/*
 * The terminal front end: events in, frames out.
 *
 * Everything that decides what an edit means lives in kilix_mask_edit.h.
 * What is left here is the part that genuinely needs a terminal - reading
 * the pointer, placing it in the frame, and getting pixels back out - and
 * it is deliberately thin, because this is the part no test can reach.
 */

#include "kmask_run.h"
#include "kmask_ui.h"

#include "kitty_terminal_session.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define POLL_INTERVAL_MS 40

/* One short of the presenter's own limit, leaving room for the status
 * strip's rectangle without tipping it into a full frame. */
#define EDIT_DAMAGE_MAX 15

typedef struct app {
    kmaskedit *editor;
    kmask *mask;
    const char *path;
    char message[160];
    bool help;
    bool chrome_dirty;
    bool quitting;
    bool confirm_quit;
} app;

/*
 * The session, for the fatal-signal path only.  A handler cannot take a
 * lock or free anything, so this exists purely so the terminal is handed
 * back before the process dies rather than left in the alternate screen
 * with a hidden cursor.
 */
static kittyts_session *g_session;

static void restore_and_die(int signal_number)
{
    if (g_session != NULL) {
        kittyts_emergency_restore(g_session);
    }
    _exit(128 + signal_number);
}

static void install_handlers(void)
{
    static const int fatal[] = {SIGINT, SIGTERM, SIGHUP, SIGSEGV, SIGBUS,
                                SIGFPE, SIGILL, SIGABRT};
    struct sigaction action;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = restore_and_die;
    (void)sigemptyset(&action.sa_mask);
    for (size_t i = 0u; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        (void)sigaction(fatal[i], &action, NULL);
    }
    /* A terminal that goes away mid-write must not kill us before the
     * restore runs. */
    (void)signal(SIGPIPE, SIG_IGN);
}

static void say(app *state, const char *text)
{
    (void)snprintf(state->message, sizeof(state->message), "%s", text);
    state->chrome_dirty = true;
}

static void save(app *state)
{
    if (state->path == NULL) {
        say(state, "no path to save to; pass one on the command line");
        return;
    }
    if (!kmask_save(state->mask, state->path)) {
        say(state, "save failed");
        return;
    }
    kmaskedit_mark_saved(state->editor);
    say(state, "saved");
}

/* Returns false when the key means "stop". */
static bool handle_key(app *state, const kittykb_event *event, int view_w,
                       int view_h)
{
    kmaskedit *editor = state->editor;
    const uint32_t key = event->key;

    if (event->action == KITTYKB_ACTION_RELEASE) {
        return true;
    }
    /* Any key clears a transient message, so a stale "saved" does not sit
     * there through the next ten edits. */
    if (state->message[0] != '\0') {
        state->message[0] = '\0';
        state->chrome_dirty = true;
    }
    if (key >= '1' && key <= '9') {
        kmaskedit_set_region(editor, (uint8_t)(key - '0'));
        state->chrome_dirty = true;
        return true;
    }
    switch (key) {
    case 'q':
        if (kmaskedit_modified(editor) && !state->confirm_quit) {
            state->confirm_quit = true;
            say(state, "unsaved changes - q again to discard, s to save");
            return true;
        }
        return false;
    case 's':
        save(state);
        break;
    case 'b': kmaskedit_set_tool(editor, KMASKEDIT_TOOL_BRUSH); break;
    case 'r': kmaskedit_set_tool(editor, KMASKEDIT_TOOL_RECT); break;
    case 'w': kmaskedit_set_tool(editor, KMASKEDIT_TOOL_WAND); break;
    case 'p': kmaskedit_set_tool(editor, KMASKEDIT_TOOL_PICK); break;
    case '[':
        kmaskedit_set_brush(editor, kmaskedit_get_brush(editor) - 2);
        break;
    case ']':
        kmaskedit_set_brush(editor, kmaskedit_get_brush(editor) + 2);
        break;
    case ',':
        kmaskedit_set_wand_tolerance(
            editor, kmaskedit_get_wand_tolerance(editor) - 4);
        break;
    case '.':
        kmaskedit_set_wand_tolerance(
            editor, kmaskedit_get_wand_tolerance(editor) + 4);
        break;
    case 'g':
        kmaskedit_set_grid(editor, !kmaskedit_get_grid(editor));
        break;
    case 'f':
        kmaskedit_fit(editor);
        break;
    case '+':
    case '=':
        kmaskedit_zoom(editor, 1, view_w / 2, view_h / 2);
        break;
    case '-':
        kmaskedit_zoom(editor, -1, view_w / 2, view_h / 2);
        break;
    case 'u':
        if (!kmaskedit_undo(editor)) {
            say(state, "nothing to undo");
        }
        break;
    case 'R':
        if (!kmaskedit_redo(editor)) {
            say(state, "nothing to redo");
        }
        break;
    case 'c':
        kmaskedit_clear_region(editor, kmaskedit_get_region(editor));
        break;
    case '?':
        state->help = !state->help;
        kmaskedit_damage_all(editor);
        break;
    case KITTYKB_KEY_ESCAPE:
        if (state->help) {
            state->help = false;
            kmaskedit_damage_all(editor);
        } else {
            kmaskedit_cancel(editor);
        }
        break;
    case KITTYKB_KEY_LEFT:  case 'h': kmaskedit_pan(editor,  32,   0); break;
    case KITTYKB_KEY_RIGHT: case 'l': kmaskedit_pan(editor, -32,   0); break;
    case KITTYKB_KEY_UP:    case 'k': kmaskedit_pan(editor,   0,  32); break;
    case KITTYKB_KEY_DOWN:  case 'j': kmaskedit_pan(editor,   0, -32); break;
    default:
        return true;
    }
    /* Any key that got this far changed something the strip reports. */
    state->chrome_dirty = true;
    if (key != 'q') {
        state->confirm_quit = false;
    }
    return true;
}

static void handle_mouse(app *state, const kittyin_mouse_event *mouse,
                         int origin_x, int origin_y)
{
    /* A pixel report is relative to the terminal; the frame is centred
     * inside it, and the editor's view starts at the frame's corner. */
    const int x = (int)mouse->x - origin_x;
    const int y = (int)mouse->y - origin_y;

    if (state->help) {
        return;
    }
    switch ((kittyin_mouse_action)mouse->action) {
    case KITTYIN_MOUSE_PRESS:
        kmaskedit_press(state->editor, x, y,
                        mouse->button == 3u ? KMASKEDIT_BUTTON_ERASE
                                            : KMASKEDIT_BUTTON_PAINT);
        state->chrome_dirty = true;
        break;
    case KITTYIN_MOUSE_MOVE:
        /* Button 0 means no button is down, which is a hover; anything
         * else continues the stroke that press started. */
        if (mouse->button != 0u && kmaskedit_stroking(state->editor)) {
            kmaskedit_drag(state->editor, x, y);
        } else {
            kmaskedit_hover(state->editor, x, y);
        }
        state->chrome_dirty = true;
        break;
    case KITTYIN_MOUSE_RELEASE:
        kmaskedit_release(state->editor, x, y);
        state->chrome_dirty = true;
        break;
    case KITTYIN_MOUSE_WHEEL:
        if (mouse->wheel_y != 0) {
            kmaskedit_zoom(state->editor, mouse->wheel_y > 0 ? -1 : 1, x, y);
            state->chrome_dirty = true;
        }
        break;
    default:
        break;
    }
}

int kmask_run(kmaskedit *editor, kmask *mask, const char *path)
{
    kittyts_session session;
    kittyts_options options;
    app state;
    sr_canvas frame;
    uint8_t *rgba = NULL;
    int width;
    int height;
    int view_h;
    bool needs_full = true;
    int status = 0;

    if (editor == NULL || mask == NULL) {
        return 2;
    }
    (void)memset(&state, 0, sizeof(state));
    state.editor = editor;
    state.mask = mask;
    state.path = path;

    kittyts_session_init(&session);
    kittyts_options_init(&options);
    /* ANY-event tracking, because the cursor has to follow the pointer
     * with no button held - the terminal hides the system pointer over a
     * graphics placement, so the editor draws its own. */
    options.mouse_tracking = KITTYIN_MOUSE_TRACKING_MOTION;
    options.pixel_mouse = true;
    options.focus_events = true;

    if (kittyts_start(&session, STDIN_FILENO, STDOUT_FILENO, &options) != 0) {
        (void)fprintf(stderr, "kilix-mask: %s\n",
                      errno == ENOTSUP
                          ? "this terminal does not support graphics"
                          : strerror(errno));
        return 1;
    }
    g_session = &session;
    install_handlers();

    width = kittyts_width(&session);
    height = kittyts_height(&session);
    view_h = height - KMASK_UI_STATUS_HEIGHT;
    if (view_h < 1 || !sr_canvas_init(&frame, width, height) ||
        !kmaskedit_set_view(editor, width, view_h)) {
        kittyts_stop(&session);
        g_session = NULL;
        (void)fprintf(stderr, "kilix-mask: terminal is too small\n");
        return 1;
    }
    rgba = malloc((size_t)width * (size_t)height * 4u);
    if (rgba == NULL) {
        sr_canvas_free(&frame);
        kittyts_stop(&session);
        g_session = NULL;
        return 1;
    }

    while (!state.quitting) {
        struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        kittyin_event event;
        int ready;

        if (kittyts_check_resize(&session, &width, &height)) {
            uint8_t *grown;

            view_h = height - KMASK_UI_STATUS_HEIGHT;
            sr_canvas_free(&frame);
            grown = realloc(rgba, (size_t)width * (size_t)height * 4u);
            if (grown == NULL || view_h < 1 ||
                !sr_canvas_init(&frame, width, height) ||
                !kmaskedit_set_view(editor, width, view_h)) {
                rgba = grown != NULL ? grown : rgba;
                status = 1;
                break;
            }
            rgba = grown;
            needs_full = true;
            state.chrome_dirty = true;
        }

        ready = poll(&descriptor, 1u, POLL_INTERVAL_MS);
        if (ready < 0 && errno != EINTR) {
            status = 1;
            break;
        }
        if (ready > 0 && kittyts_read_input(&session) < 0 && errno != EINTR) {
            status = 1;
            break;
        }
        while (kittyts_next_event(&session, &event)) {
            if (event.kind == KITTYIN_EVENT_KEY) {
                if (!handle_key(&state, &event.data.key, width, view_h)) {
                    state.quitting = true;
                    break;
                }
            } else if (event.kind == KITTYIN_EVENT_MOUSE) {
                handle_mouse(&state, &event.data.mouse,
                             kittyts_origin_x(&session),
                             kittyts_origin_y(&session));
            } else if (event.kind == KITTYIN_EVENT_FOCUS &&
                       !event.data.focus.focused) {
                /* The pointer is somewhere else now, so stop drawing a
                 * cursor that no longer tracks anything. */
                kmaskedit_hover(editor, -1, -1);
                kmaskedit_cancel(editor);
            }
        }
        if (state.quitting) {
            break;
        }

        {
            kmaskedit_rect rects[EDIT_DAMAGE_MAX + 1];
            kittyfb_rect patches[EDIT_DAMAGE_MAX + 1];
            size_t count = kmaskedit_take_damage(editor, rects,
                                                 EDIT_DAMAGE_MAX);

            if (count == 0u && !state.chrome_dirty && !needs_full) {
                continue;
            }
            kmaskedit_compose(editor, &frame, 0, 0);
            kmask_ui_status(&frame, view_h, width, editor, path,
                            state.message);
            if (state.help) {
                kmask_ui_help(&frame, width, view_h);
            }
            if (!sr_pack_rgba(&frame, rgba,
                              (size_t)width * (size_t)height * 4u)) {
                status = 1;
                break;
            }
            if (state.chrome_dirty) {
                rects[count].x0 = 0;
                rects[count].y0 = view_h;
                rects[count].x1 = width;
                rects[count].y1 = height;
                count++;
                state.chrome_dirty = false;
            }
            if (needs_full || state.help) {
                if (!kittyts_present(&session, rgba, width, height)) {
                    status = 1;
                    break;
                }
                needs_full = false;
                continue;
            }
            /* Copied field by field rather than cast.  The two structs
             * hold the same four ints in the same order, but relying on
             * that would make a reordering in either header a silent
             * corruption instead of a compile error. */
            for (size_t i = 0u; i < count; i++) {
                patches[i].x0 = rects[i].x0;
                patches[i].y0 = rects[i].y0;
                patches[i].x1 = rects[i].x1;
                patches[i].y1 = rects[i].y1;
            }
            if (!kittyts_present_damage(&session, rgba, width, height,
                                        patches, count)) {
                status = 1;
                break;
            }
        }
    }

    free(rgba);
    sr_canvas_free(&frame);
    kittyts_stop(&session);
    g_session = NULL;
    return status;
}
