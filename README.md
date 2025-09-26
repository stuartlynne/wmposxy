# wmposxy

`wmposxy` launches an X11 client (typically under XWayland) and then repositions its window using the coordinates extracted from the client's `--geometry` argument. This allows geometry hints to take effect on modern Wayland desktops where position requests are ignored.

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
