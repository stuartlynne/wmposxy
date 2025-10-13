/*
 * De-orange helper for X11 windows (Konsole in our case)
 *
 * Why:
 *   Some Konsoles set attention flags at startup. KWin/Task Manager uses that
 *   to paint orange indicators and prevent autohide. We strip those flags
 *   per-window right after we position them.
 *
 * What:
 *   - Remove EWMH state _NET_WM_STATE_DEMANDS_ATTENTION
 *   - Clear legacy XUrgencyHint in WM_HINTS
 *   - Also attempt on the window-group leader (some toolkits set on leader)
 *   - Retry briefly to beat immediate reassertions during startup
 *
 * Safe to call repeatedly; no effect if flags aren’t present.
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <unistd.h>     // usleep
#include <stdbool.h>
#include <stdio.h>

static void send_net_wm_state(Display *dpy, Window root, Window win, long action, Atom atom) {
    XEvent e;
    memset(&e, 0, sizeof(e));
    e.xclient.type = ClientMessage;
    e.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
    e.xclient.display = dpy;
    e.xclient.window = win;
    e.xclient.format = 32;
    e.xclient.data.l[0] = action;                   // 0=remove, 1=add, 2=toggle
    e.xclient.data.l[1] = atom;                     // first property
    e.xclient.data.l[2] = None;                     // second (unused)
    e.xclient.data.l[3] = 1;                        // source indication: normal
    e.xclient.data.l[4] = 0;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

static bool has_demands_attention(Display *dpy, Window win, Atom net_wm_state, Atom demattn) {
    Atom type; int format; unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    bool found = false;

    if (XGetWindowProperty(dpy, win, net_wm_state, 0, (~0L), False, AnyPropertyType,
                           &type, &format, &nitems, &bytes_after, &data) == Success && data) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < nitems; ++i) {
            if (atoms[i] == demattn) { found = true; break; }
        }
        XFree(data);
    }
    return found;
}

static void clear_urgency_hint(Display *dpy, Window win) {
    XWMHints *h = XGetWMHints(dpy, win);
    if (!h) return;
    if (h->flags & XUrgencyHint) {
        h->flags &= ~XUrgencyHint;
        XSetWMHints(dpy, win, h);
    }
    XFree(h);
}

static Window get_group_leader(Display *dpy, Window win) {
    Window leader = 0;
    XWMHints *h = XGetWMHints(dpy, win);
    if (h) {
        if (h->flags & WindowGroupHint) leader = h->window_group;
        XFree(h);
    }
    return leader;
}

/*
 * Call after you have the window mapped and positioned.
 * Params:
 *   dpy: already-open X display (or open one locally)
 *   wid: the X11 Window id of the Konsole to clean (from your find step)
 *   retries: how many 100ms retries to run (e.g. 10 = ~1s)
 */
static void deorange_window(Display *dpy, Window wid, int retries) {
    if (!dpy || !wid) return;

    Window root = DefaultRootWindow(dpy);
    Atom demattn = XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    Atom net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);

    Window leader = get_group_leader(dpy, wid);

    // one pass now
    send_net_wm_state(dpy, root, wid, 0 /* remove */, demattn);
    clear_urgency_hint(dpy, wid);
    if (leader && leader != wid) {
        send_net_wm_state(dpy, root, leader, 0, demattn);
        clear_urgency_hint(dpy, leader);
    }
    XFlush(dpy);

    // grace loop: in case app reasserts immediately as it settles
    for (int i = 0; i < retries; ++i) {
        usleep(100 * 1000); // 100ms
        bool back = has_demands_attention(dpy, wid, net_wm_state, demattn);
        if (!back && leader && leader != wid)
            back = has_demands_attention(dpy, leader, net_wm_state, demattn);
        if (!back) break;

        send_net_wm_state(dpy, root, wid, 0, demattn);
        clear_urgency_hint(dpy, wid);
        if (leader && leader != wid) {
            send_net_wm_state(dpy, root, leader, 0, demattn);
            clear_urgency_hint(dpy, leader);
        }
        XFlush(dpy);
    }
}

/*
 * Perfect. Here’s a drop-in, well-commented C helper you can paste into main.c (or a tiny deorange.c) 
 * so Codex/code review has the “why” and the “how” right next to the code. It’s X11-only 
 * (your seat1 is X11), uses only Xlib, and mirrors what wmctrl -b remove,demands_attention does—plus 
 * it also clears the legacy XUrgencyHint and retries briefly to squash reassertions during startup.
 *
 * What this fixes (put this in a comment header)

 * Some Konsole windows start with _NET_WM_STATE_DEMANDS_ATTENTION (EWMH) and/or XUrgencyHint set in WM_HINTS.

 * KWin/Task Manager turns that into an orange badge and can defeat autohide.

 * We clear both flags per window after mapping and geometry placement, and re-check a few times to catch immediate reassertions.
 */

/* Example integration (pseudo-code):
 *
 *   Display *dpy = XOpenDisplay(NULL);
 *   // ... launch konsole (fork/exec), compute the expected title, wait for it to appear ...
 *   Window wid = find_window_by_title_or_wmclass(dpy, "slot-C-100688-1669", "konsole");
 *   // move/resize with your existing logic
 *   position_window(dpy, wid, x, y, w, h);
 *   // now, strip attention flags (10 retries ~ 1s)
 *   deorange_window(dpy, wid, 10);
 *   XCloseDisplay(dpy);
 *
 * Notes:
 *   - Prefer passing titles silently at launch:
 *       konsole --qwindowtitle 'slot-C-…'  -p tabtitle='slot-C-…'
 *     (No OSC/BEL; easier to match; no notifications.)
 *   - Keep Bell disabled in profiles (BellMode=0) and bash/readline (set bell-style none).
 */

