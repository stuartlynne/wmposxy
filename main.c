/*
 * wmposxy - launch an application and reposition its window based on --geometry.
 *
 * Inspired by wmctrl; authored by Stuart Lynne with ChatGPT assistance.
 *
 * Copyright (C) 2025 Stuart Lynne
 * Contact: stuart.lynne@gmail.com
 *
 * GNU GPL v2 or later.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <wmctrl.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "wmposxy – honour --geometry positions under Wayland (via X11/XWayland).\n"
            "Launch the target program, wait for its X11 window, then move it to the\n"
            "requested coordinates. The program must include a --geometry WxH+X+Y token.\n\n"
            "Usage: %s <program> [args...]\n\n"
            "Example:\n"
            "  %s konsole --geometry 1200x600+0+840 -e ssh user@host\n",
            prog, prog);
}

struct geometry {
    bool have_pos;
    bool x_negative;
    bool y_negative;
    int x; /* absolute value */
    int y; /* absolute value */
};

static bool parse_geometry(const char *arg, struct geometry *geom)
{
    if (!arg || !geom) {
        return false;
    }

    const char *p = strpbrk(arg, "+-");
    if (!p) {
        return false;
    }

    geom->have_pos = true;

    geom->x_negative = (*p == '-');
    p++;
    char *end = NULL;
    geom->x = (int)strtol(p, &end, 10);
    if (end == p) {
        return false;
    }

    p = end;
    if (*p != '+' && *p != '-') {
        return false;
    }
    geom->y_negative = (*p == '-');
    p++;
    geom->y = (int)strtol(p, &end, 10);
    if (end == p) {
        return false;
    }

    return true;
}

static const char *find_geometry(int argc, char *const argv[])
{
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg) {
            continue;
        }
        if (strcmp(arg, "--geometry") == 0 || strcmp(arg, "-geometry") == 0) {
            if (i + 1 < argc) {
                return argv[i + 1];
            }
            return NULL;
        }
        if (strncmp(arg, "--geometry=", 11) == 0) {
            return arg + 11;
        }
        if (strncmp(arg, "-geometry=", 10) == 0) {
            return arg + 10;
        }
    }
    return NULL;
}

static bool should_minimize_program(const char *program)
{
    if (!program || *program == '\0') {
        return false;
    }
    const char *base = strrchr(program, '/');
    base = base ? base + 1 : program;
    if (*base == '\0') {
        return false;
    }
    return strcasecmp(base, "konsole") == 0;
}

static bool window_is_konsole(Display *disp, Window win)
{
    if (!disp || win == None) {
        return false;
    }
    XClassHint hint;
    if (!XGetClassHint(disp, win, &hint)) {
        return false;
    }
    bool match = false;
    if (hint.res_name && strcasecmp(hint.res_name, "konsole") == 0) {
        match = true;
    }
    if (!match && hint.res_class && strcasecmp(hint.res_class, "konsole") == 0) {
        match = true;
    }
    if (hint.res_name) {
        XFree(hint.res_name);
    }
    if (hint.res_class) {
        XFree(hint.res_class);
    }
    return match;
}

static int xerr_ignore_badwindow(Display *d, XErrorEvent *e)
{
    if (e->error_code == BadWindow) {
        return 0;
    }
    char buf[256];
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    fprintf(stderr, "wmposxy: X error: %s (opcode=%d resource=0x%lx)\n",
            buf, e->request_code, e->resourceid);
    return 0;
}

static bool move_window(wmctrl_context *ctx, Window win, const struct geometry *geom)
{
    if (!geom->have_pos) {
        return true;
    }

    Display *disp = ctx->display;
    Window root_return;
    int x_return, y_return;
    unsigned int width, height, border_width, depth;

    if (!XGetGeometry(disp, win, &root_return, &x_return, &y_return,
                      &width, &height, &border_width, &depth)) {
        fprintf(stderr, "wmposxy: XGetGeometry failed\n");
        return false;
    }

    int screen = DefaultScreen(disp);
    int screen_w = DisplayWidth(disp, screen);
    int screen_h = DisplayHeight(disp, screen);

    int frame_w = (int)width + (int)border_width * 2;
    int frame_h = (int)height + (int)border_width * 2;

    int target_x = geom->x_negative ? (screen_w - frame_w - geom->x) : geom->x;
    int target_y = geom->y_negative ? (screen_h - frame_h - geom->y) : geom->y;

    if (wmctrl_move_window(ctx, win, target_x, target_y, 0, 0, 1, 0) != 0) {
        fprintf(stderr, "wmposxy: failed to move window\n");
        return false;
    }

    return true;
}

static bool wait_for_window(wmctrl_context *ctx, pid_t pid, Window *win_out, int timeout_ms)
{
    const int sleep_step_ms = 50;
    const int iterations = timeout_ms / sleep_step_ms;

    for (int i = 0; i < iterations; ++i) {
        if (wmctrl_find_window_by_pid(ctx, pid, win_out) == 0) {
            return true;
        }

        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return false;
        } else if (r < 0 && errno != ECHILD) {
            perror("waitpid");
            return false;
        }

        struct timespec ts = {0};
        ts.tv_nsec = sleep_step_ms * 1000000L;
        nanosleep(&ts, NULL);
    }
    return false;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }

    const char *geom_arg = find_geometry(argc, argv);
    if (!geom_arg) {
        fprintf(stderr, "wmposxy: no --geometry argument supplied\n");
        return EXIT_FAILURE;
    }

    struct geometry geom = {0};
    if (!parse_geometry(geom_arg, &geom) || !geom.have_pos) {
        fprintf(stderr, "wmposxy: unable to parse geometry '%s'\n", geom_arg);
        return EXIT_FAILURE;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (child == 0) {
        execvp(argv[1], &argv[1]);
        perror("execvp");
        _exit(EXIT_FAILURE);
    }

    wmctrl_context ctx;
    if (wmctrl_init(&ctx, NULL) != 0) {
        fprintf(stderr, "wmposxy: unable to open X display; requires Xorg/XWayland.\n");
        return EXIT_FAILURE;
    }

    if (!wmctrl_supports_xwayland(&ctx)) {
        wmctrl_finish(&ctx);
        /* No XWayland; just let the child run without repositioning. */
        return EXIT_SUCCESS;
    }

    XSetErrorHandler(xerr_ignore_badwindow);

    Window win = None;
    if (!wait_for_window(&ctx, child, &win, 8000)) {
        fprintf(stderr, "wmposxy: timed out waiting for window (pid %d)\n", child);
        wmctrl_finish(&ctx);
        return EXIT_FAILURE;
    }

    if (!move_window(&ctx, win, &geom)) {
        wmctrl_finish(&ctx);
        return EXIT_FAILURE;
    }

    bool minimize = should_minimize_program(argv[1]) || window_is_konsole(ctx.display, win);
    if (minimize) {
        if (wmctrl_minimize_window(&ctx, win) != 0) {
            fprintf(stderr, "wmposxy: failed to minimize window\n");
        }
    }

    wmctrl_finish(&ctx);
    return EXIT_SUCCESS;
}
