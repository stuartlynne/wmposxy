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
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

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

static Atom atom_net_client_list;
static Atom atom_net_client_list_stacking;
static Atom atom_net_wm_pid;
static Atom atom_net_moveresize;

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

static bool get_window_pid(Display *disp, Window win, pid_t *pid_out)
{
    if (win == None) {
        return false;
    }
    unsigned char *prop = NULL;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    int status = XGetWindowProperty(disp, win, atom_net_wm_pid, 0, 1, False,
                                    XA_CARDINAL, &actual_type, &actual_format,
                                    &nitems, &bytes_after, &prop);
    if (status == Success && prop && actual_type == XA_CARDINAL && actual_format == 32 && nitems >= 1) {
        unsigned long value = *(unsigned long *)prop;
        *pid_out = (pid_t)value;
        XFree(prop);
        return true;
    }
    if (prop) {
        XFree(prop);
    }
    return false;
}

static bool get_window_list(Display *disp, Atom prop, Window **wins_out, unsigned long *count_out)
{
    unsigned char *data = NULL;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;

    int status = XGetWindowProperty(disp, DefaultRootWindow(disp), prop, 0, 1024,
                                    False, XA_WINDOW, &actual_type,
                                    &actual_format, &nitems, &bytes_after, &data);
    if (status != Success || actual_type != XA_WINDOW || actual_format != 32) {
        if (data) {
            XFree(data);
        }
        return false;
    }

    *wins_out = (Window *)data;
    *count_out = nitems;
    return true;
}

static bool find_window_recursive(Display *disp, Window root, pid_t target_pid, Window *result)
{
    pid_t pid;
    if (get_window_pid(disp, root, &pid) && pid == target_pid) {
        *result = root;
        return true;
    }

    Window root_ret, parent;
    Window *children = NULL;
    unsigned int nchildren = 0;

    if (!XQueryTree(disp, root, &root_ret, &parent, &children, &nchildren)) {
        return false;
    }

    bool found = false;
    for (unsigned int i = 0; i < nchildren && !found; ++i) {
        if (find_window_recursive(disp, children[i], target_pid, result)) {
            found = true;
        }
    }

    if (children) {
        XFree(children);
    }

    return found;
}

static bool lookup_window_by_pid(Display *disp, pid_t target_pid, Window *out_win)
{
    if (atom_net_client_list != None) {
        Window *list = NULL;
        unsigned long count = 0;
        if (get_window_list(disp, atom_net_client_list, &list, &count)) {
            for (unsigned long i = 0; i < count; ++i) {
                if (list[i] == None) {
                    continue;
                }
                pid_t pid;
                if (get_window_pid(disp, list[i], &pid) && pid == target_pid) {
                    *out_win = list[i];
                    XFree(list);
                    return true;
                }
            }
            XFree(list);
        }
    }

    if (atom_net_client_list_stacking != None) {
        Window *list = NULL;
        unsigned long count = 0;
        if (get_window_list(disp, atom_net_client_list_stacking, &list, &count)) {
            for (unsigned long i = 0; i < count; ++i) {
                if (list[i] == None) {
                    continue;
                }
                pid_t pid;
                if (get_window_pid(disp, list[i], &pid) && pid == target_pid) {
                    *out_win = list[i];
                    XFree(list);
                    return true;
                }
            }
            XFree(list);
        }
    }

    return find_window_recursive(disp, DefaultRootWindow(disp), target_pid, out_win);
}

static bool send_moveresize(Display *disp, Window win, int x, int y)
{
    if (atom_net_moveresize == None) {
        return false;
    }

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.message_type = atom_net_moveresize;
    ev.xclient.window = win;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (1 << 8) | (1 << 9); /* use X and Y */
    ev.xclient.data.l[1] = x;
    ev.xclient.data.l[2] = y;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = 0;

    long mask = SubstructureRedirectMask | SubstructureNotifyMask;
    if (XSendEvent(disp, DefaultRootWindow(disp), False, mask, &ev) == 0) {
        return false;
    }
    XFlush(disp);
    return true;
}

static bool move_window(Display *disp, Window win, const struct geometry *geom)
{
    if (!geom->have_pos) {
        return true;
    }

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

    if (!send_moveresize(disp, win, target_x, target_y)) {
        XMoveWindow(disp, win, target_x, target_y);
        XFlush(disp);
    }

    return true;
}

static bool wait_for_window(Display *disp, pid_t pid, Window *win_out, int timeout_ms)
{
    const int sleep_step_ms = 50;
    const int iterations = timeout_ms / sleep_step_ms;

    for (int i = 0; i < iterations; ++i) {
        if (lookup_window_by_pid(disp, pid, win_out)) {
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

    Display *disp = XOpenDisplay(NULL);
    if (!disp) {
        fprintf(stderr, "wmposxy: unable to open X display; requires Xorg/XWayland.\n");
        return EXIT_FAILURE;
    }

    int xwayland_opcode, xwayland_event, xwayland_error;
    Bool have_xwayland = XQueryExtension(disp, "XWAYLAND",
                                         &xwayland_opcode,
                                         &xwayland_event,
                                         &xwayland_error);

    if (!have_xwayland) {
        XCloseDisplay(disp);
        /* No XWayland; just let the child run without repositioning. */
        return EXIT_SUCCESS;
    }

    atom_net_client_list = XInternAtom(disp, "_NET_CLIENT_LIST", True);
    atom_net_client_list_stacking = XInternAtom(disp, "_NET_CLIENT_LIST_STACKING", True);
    atom_net_wm_pid = XInternAtom(disp, "_NET_WM_PID", False);
    atom_net_moveresize = XInternAtom(disp, "_NET_MOVERESIZE_WINDOW", True);

    XSetErrorHandler(xerr_ignore_badwindow);

    Window win = None;
    if (!wait_for_window(disp, child, &win, 8000)) {
        fprintf(stderr, "wmposxy: timed out waiting for window (pid %d)\n", child);
        XCloseDisplay(disp);
        return EXIT_FAILURE;
    }

    if (!move_window(disp, win, &geom)) {
        XCloseDisplay(disp);
        return EXIT_FAILURE;
    }

    XCloseDisplay(disp);
    return EXIT_SUCCESS;
}
