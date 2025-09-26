# wmposxy

`wmposxy` launches an X11 client (typically under XWayland) and then repositions its window using the coordinates extracted from the client's `--geometry` argument. This allows geometry hints to take effect on modern Wayland desktops where position requests are ignored.

Modern Wayland compositors intentionally disregard client-supplied window coordinates. The idea is that only the compositor should choose where a surface lands; clients merely suggest their preferred size. That policy makes spoofing harder and gives the compositor full control over window layouts. The downside is that long-lived X11 workflows—lining up multiple terminals, restoring complex layouts—no longer honour the positional half of `--geometry`. `wmposxy` (via XWayland) reproduces the classic X11 behaviour by moving the window after the compositor has created it.

> ⚠️ **Security caveat**: re-enabling arbitrary repositioning weakens that Wayland guarantee. A malicious program could place a fake password dialog exactly where a legitimate one usually appears. Use this tool only on machines you trust, and be aware that it reintroduces some of the spoofing surface Wayland was trying to eliminate.

### When you might want it

- Restoring a familiar “multiple terminal” workflow where each window tucks into a specific monitor corner or quadrant (e.g. a 4×1920×1200 wall running 8–10 Konsole/xterm sessions for editing, builds, logs, and REPLs).
- Launching scripted layouts that rely on `--geometry` to arrange panes, dashboards, or monitoring consoles across several screens.
- Any X11/XWayland application whose toolkit (Qt, GTK, etc.) ignores the positional hints yet needs to land in a predictable place.

## Requirements

- X11/XWayland environment (Wayland sessions expose this automatically)
- `libX11` headers/libraries for building

## Build

```sh
cd wmposxy
make            # produces ./wmposxy
```

## Usage

```sh
wmposxy konsole --geometry 124x48+0+840 -- ssh user@host
# or, for classic X11 clients:
wmposxy xterm -geometry 100x30+1920+0
```

The wrapper forwards all arguments unchanged. It simply parses the `--geometry` token, spawns the target program, waits for a window whose PID matches the child, and issues an `_NET_MOVERESIZE_WINDOW` request to reposition it (falling back to `XMoveWindow` if the compositor does not support the EWMH request). Width/height are left untouched; only X/Y are adjusted. Negative offsets are interpreted relative to the right/bottom edges using the current window size.

### Behaviour notes

- If no `--geometry` token is present, `wmposxy` exits with an error.
- If the child exits before creating a window, positioning is aborted and a non-zero status is returned.
- `_NET_CLIENT_LIST` / `_NET_WM_PID` are used to look up windows by PID; if unavailable, the wrapper traverses the entire window tree as a fallback.
- No dependency on the external `wmctrl` utility.

## Limitations

- Pure Wayland clients (no XWayland support) cannot be controlled.
- Decorations and compositor quirks may introduce small offsets, as `wmposxy` relies on the geometry reported by Xlib.
