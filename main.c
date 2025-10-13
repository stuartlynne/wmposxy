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
#include <X11/Xatom.h>

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

static void send_net_wm_state(Display *disp, Window root, Window win, long action,
                              Atom net_wm_state, Atom atom)
{
    if (!disp || !win || atom == None || net_wm_state == None) {
        return;
    }

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = action; /* 0=remove */
    ev.xclient.data.l[1] = atom;
    ev.xclient.data.l[2] = None;
    ev.xclient.data.l[3] = 1; /* source indication: normal */
    ev.xclient.data.l[4] = 0;

    XSendEvent(disp, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

static void remove_atom_from_property(Display *disp, Window win, Atom property,
                                      Atom atom_to_remove)
{
    if (!disp || !win || property == None || atom_to_remove == None) {
        return;
    }

    Atom type_return = None;
    int format_return = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(disp, win, property, 0, (~0L), False, AnyPropertyType,
                           &type_return, &format_return, &nitems, &bytes_after,
                           &data) != Success || data == NULL) {
        return;
    }

    if (format_return != 32 || type_return == None) {
        XFree(data);
        return;
    }

    Atom *atoms = (Atom *)data;
    unsigned long write_idx = 0;
    bool removed = false;

    for (unsigned long i = 0; i < nitems; ++i) {
        if (atoms[i] == atom_to_remove) {
            removed = true;
            continue;
        }
        atoms[write_idx++] = atoms[i];
    }

    if (removed) {
        if (write_idx > 0) {
            XChangeProperty(disp, win, property, type_return, format_return,
                            PropModeReplace, (unsigned char *)atoms, write_idx);
        } else {
            XDeleteProperty(disp, win, property);
        }
    }

    XFree(data);
}

static bool window_has_atom(Display *disp, Window win, Atom property, Atom atom)
{
    Atom type_return = None;
    int format_return = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;
    bool found = false;

    if (!disp || !win || property == None || atom == None) {
        return false;
    }

    if (XGetWindowProperty(disp, win, property, 0, (~0L), False, AnyPropertyType,
                           &type_return, &format_return, &nitems, &bytes_after,
                           &data) == Success && data != NULL) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < nitems; ++i) {
            if (atoms[i] == atom) {
                found = true;
                break;
            }
        }
        XFree(data);
    }

    return found;
}

static void clear_urgency_hint(Display *disp, Window win)
{
    if (!disp || !win) {
        return;
    }

    XWMHints *hints = XGetWMHints(disp, win);
    if (!hints) {
        return;
    }

    if (hints->flags & XUrgencyHint) {
        hints->flags &= ~XUrgencyHint;
        XSetWMHints(disp, win, hints);
    }

    XFree(hints);
}

static Window get_window_group_leader(Display *disp, Window win)
{
    if (!disp || !win) {
        return None;
    }

    Window leader = None;
    XWMHints *hints = XGetWMHints(disp, win);
    if (hints) {
        if (hints->flags & WindowGroupHint) {
            leader = hints->window_group;
        }
        XFree(hints);
    }

    return leader;
}

static Window get_client_leader(Display *disp, Window win)
{
    if (!disp || !win) {
        return None;
    }

    Atom prop = XInternAtom(disp, "WM_CLIENT_LEADER", False);
    if (prop == None) {
        return None;
    }

    Atom type_return = None;
    int format_return = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;
    Window leader = None;

    if (XGetWindowProperty(disp, win, prop, 0, 1, False, AnyPropertyType,
                           &type_return, &format_return, &nitems, &bytes_after,
                           &data) == Success && data != NULL && nitems >= 1 && format_return == 32) {
        leader = (Window)((unsigned long *)data)[0];
    }

    if (data) {
        XFree(data);
    }

    return leader;
}

static void strip_attention_flags(Display *disp, Window win, int retries, bool debug)
{
    if (!disp || !win) {
        return;
    }

    Window root = DefaultRootWindow(disp);
    Atom atom_net_wm_state = XInternAtom(disp, "_NET_WM_STATE", False);
    Atom atom_demands_attention =
        XInternAtom(disp, "_NET_WM_STATE_DEMANDS_ATTENTION", False);

    Window targets[16] = {None};
    size_t target_count = 0;

    Window leader_group = get_window_group_leader(disp, win);
    Window leader_client = get_client_leader(disp, win);

    /* Some Qt apps create transient notification windows; scan reparent tree for leaders. */
    Window transient_for = None;
    Atom atom_transient_for = XInternAtom(disp, "WM_TRANSIENT_FOR", False);
    if (atom_transient_for != None) {
        Atom type_return = None;
        int format_return = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(disp, win, atom_transient_for, 0, 1, False,
                               AnyPropertyType, &type_return, &format_return,
                               &nitems, &bytes_after, &data) == Success &&
            data != NULL && nitems >= 1 && format_return == 32) {
            transient_for = (Window)((unsigned long *)data)[0];
        }
        if (data) {
            XFree(data);
        }
    }

    Window candidates[16] = {None};
    size_t candidate_slots = 0;
    candidates[candidate_slots++] = win;
    candidates[candidate_slots++] = leader_group;
    candidates[candidate_slots++] = leader_client;
    candidates[candidate_slots++] = transient_for;

    /* Walk WM_TRANSIENT_FOR chain a few hops to catch notification parents. */
    Window chain = transient_for;
    for (int depth = 0; chain && depth < 4 && candidate_slots < 16; ++depth) {
        Atom type_return = None;
        int format_return = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(disp, chain, atom_transient_for, 0, 1, False,
                               AnyPropertyType, &type_return, &format_return,
                               &nitems, &bytes_after, &data) == Success &&
            data != NULL && nitems >= 1 && format_return == 32) {
            Window next = (Window)((unsigned long *)data)[0];
            if (next) {
                candidates[candidate_slots++] = next;
            }
            chain = next;
        } else {
            chain = None;
        }
        if (data) {
            XFree(data);
        }
    }

    /* Walk up the X11 parent chain to catch group leaders created via reparenting. */
    Window current = win;
    for (int depth = 0; depth < 6 && current && candidate_slots < 16; ++depth) {
        Window root_return = None;
        Window parent = None;
        Window *children = NULL;
        unsigned int nchildren = 0;
        if (!XQueryTree(disp, current, &root_return, &parent, &children, &nchildren)) {
            break;
        }
        if (children) {
            XFree(children);
        }
        if (!parent || parent == current || parent == root_return) {
            break;
        }
        candidates[candidate_slots++] = parent;
        current = parent;
    }

    for (size_t i = 0; i < candidate_slots; ++i) {
        Window candidate = candidates[i];
        if (!candidate) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < target_count; ++j) {
            if (targets[j] == candidate) {
                seen = true;
                break;
            }
        }
        if (!seen && target_count < 16) {
            targets[target_count++] = candidate;
        }
    }

    if (debug) {
        fprintf(stderr, "wmposxy: attention targets for 0x%lx:", (unsigned long)win);
        for (size_t i = 0; i < target_count; ++i) {
            fprintf(stderr, " 0x%lx", (unsigned long)targets[i]);
        }
        putchar('\n');
        fflush(stdout);
    }

    struct timespec delay = {0};
    delay.tv_nsec = 100 * 1000000L; /* 100 ms */

    for (int pass = 0; pass < (retries + 1); ++pass) {
        bool touched = false;
        for (size_t i = 0; i < target_count; ++i) {
            Window target = targets[i];
            if (!target) {
                continue;
            }
            send_net_wm_state(disp, root, target, 0, atom_net_wm_state,
                              atom_demands_attention);
            clear_urgency_hint(disp, target);
            remove_atom_from_property(disp, target, atom_net_wm_state,
                                      atom_demands_attention);
            touched = true;
        }
        if (touched) {
            XFlush(disp);
        }

        if (debug) {
            fprintf(stderr, "wmposxy: pass %d attention clear issued\n", pass);
            fflush(stdout);
        }

        if (pass == retries) {
            break;
        }

        nanosleep(&delay, NULL);

        bool any_back = false;
        for (size_t i = 0; i < target_count; ++i) {
            Window target = targets[i];
            if (!target) {
                continue;
            }
            bool has = window_has_atom(disp, target, atom_net_wm_state,
                                       atom_demands_attention);
            if (has) {
                any_back = true;
                if (debug) {
                    fprintf(stderr, "wmposxy: pass %d target 0x%lx still has DEMATTN\n",
                           pass, (unsigned long)target);
                    fflush(stdout);
                }
            }
        }
        if (!any_back) {
            if (debug) {
                fprintf(stderr, "wmposxy: attention cleared after pass %d\n", pass);
                fflush(stdout);
            }
            break;
        }
    }

    /* Monitor for late reassertions (~5s) even without debug enabled. */
    const int monitor_iterations = 50;
    for (int i = 0; i < monitor_iterations; ++i) {
        bool any_back = false;
        for (size_t j = 0; j < target_count; ++j) {
            Window target = targets[j];
            if (!target) {
                continue;
            }
            if (window_has_atom(disp, target, atom_net_wm_state,
                                atom_demands_attention)) {
                any_back = true;
                if (debug) {
                    fprintf(stderr, "wmposxy: monitor %d saw DEMATTN on 0x%lx, clearing\n",
                            i, (unsigned long)target);
                    fflush(stdout);
                }
                send_net_wm_state(disp, root, target, 0, atom_net_wm_state,
                                  atom_demands_attention);
                clear_urgency_hint(disp, target);
                remove_atom_from_property(disp, target, atom_net_wm_state,
                                          atom_demands_attention);
            }
        }
        if (any_back) {
            XFlush(disp);
        }
        nanosleep(&delay, NULL);
    }
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

    fprintf(stderr, "wmposxy: launched pid %d, waiting for window...\n", child);
    fflush(stderr);
    wmctrl_context ctx;
    if (wmctrl_init(&ctx, NULL) != 0) {
        fprintf(stderr, "wmposxy: unable to open X display; requires Xorg/XWayland.\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "wmposxy: connected to X display '%s'\n", XDisplayName(NULL));
    fflush(stderr);

    int have_xwayland = wmctrl_supports_xwayland(&ctx);
    if (have_xwayland) {
        fprintf(stderr, "wmposxy: XWayland support detected\n");
    } else {
        fprintf(stderr, "wmposxy: XWayland extension not found; continuing under X11\n");
    }
    fflush(stderr);

    XSetErrorHandler(xerr_ignore_badwindow);

    fprintf(stderr, "wmposxy: waiting up to 8s for window of pid %d...\n", child);
    fflush(stderr);
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
    fprintf(stderr, "wmposxy: moved window 0x%lx to %s%d%s%d\n",
           (unsigned long)win, geom.x_negative ? "-" : "", geom.x,
           geom.y_negative ? "-" : "", geom.y);
    fflush(stderr);

    bool debug_attention = getenv("WMPOSXY_DEBUG") != NULL;
    fprintf(stderr, "wmposxy: clearing attention flags on 0x%lx (debug=%d)\n",
           (unsigned long)win, debug_attention ? 1 : 0);
    fflush(stderr);
    strip_attention_flags(ctx.display, win, debug_attention ? 40 : 30, debug_attention);

    if (debug_attention) {
        fprintf(stderr, "wmposxy: attention scrub finished for 0x%lx\n",
               (unsigned long)win);
        fflush(stderr);
    }

    wmctrl_window_info *win_list = NULL;
    size_t win_count = 0;
    if (wmctrl_list_windows(&ctx, &win_list, &win_count) == 0 && win_list) {
        for (size_t i = 0; i < win_count; ++i) {
            wmctrl_window_info *info = &win_list[i];
            if (info->pid != child || info->window == win) {
                continue;
            }
            if (debug_attention) {
                fprintf(stderr,
                        "wmposxy: extra window 0x%lx for pid %d -> clearing attention\n",
                        (unsigned long)info->window, child);
                fflush(stderr);
            }
            strip_attention_flags(ctx.display, info->window,
                                  debug_attention ? 40 : 30, debug_attention);
        }
        wmctrl_free_window_list(win_list, win_count);
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
