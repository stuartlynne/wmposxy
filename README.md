# wmposxy
# Fri Sep 26 09:57:46 AM PDT 2025

Postulate a small program, call it wmposxy,
▌ It would take another program that supports --geometry as arguments, e.g. wmposxy konsole --geometry  124x48+0+840 -- ssh -C -X -Y -o
▌ TCPKeepAlive=no -o ServerAliveInterval=60 -A -e none -t sl@whiskey
▌
▌ wmposxy would scan the arguments to find --geometry to get the x/y positioning information.
▌ It would then use a fork/exec to get a child process that would run the given program with its arguments, returning the child pid to the parent.
▌ The parent process would wait for the child process to be available as a window,
▌ then the parent process would exec wmctrl with the appropriate arguements, e.g. wmctrl -ir 0x03000011 -e 0,720,1226,1200,900
▌
▌ Does that sound difficult to implement and package as a standalone package for linux systems that have wayland?

• I’m gathering thoughts on intercepting window geometry using tools like xdg-desktop-portal and relying on X11 utilities like wmctrl even under
  Wayland via XWayland. The main challenge centers on translating terminal columns to pixel dimensions and handling environment setup to wait
  for the target window, possibly using PIDs or sync search tools. I’m considering implementations in Python, Go, C, or Rust, ensuring geometry
  parsing and size computations convert correctly between columns and pixels, especially with the -e argument needing widths/heights. Packaging
  ideas include lightweight Python scripts with pipx or small C binaries, acknowledging the reliance on XWayland/wmctrl as a constant. Overall,
  this approach seems manageable and feasible.

> The stub you describe (“wrap whatever command understands --geometry, grab the position, spawn it, then move the window with wmctrl”) is
  absolutely doable. In fact, it’s very close to what the current script already does—only generalized so you can point it at any child process
  instead of templates you generate yourself.

  Key points you’d have to handle:

  - Parsing geometry
    WxH+X+Y is easy to split; you’d just harvest the X/Y portion before handing the whole argument set to the real program. Leave --geometry
  untouched so the child still sizes itself (even under Wayland the width/height part is honoured).
  - Spawning and matching
    fork/exec (or subprocess.Popen if you write it in Python) gives you the child PID. Once it’s running, loop on wmctrl -lp (or xdotool search
  --sync --pid …) until you find a window whose PID matches. That’s exactly the “wait for a window” step we use now.
  - Moving the window
    Call wmctrl -ir <winid> -e 0,x,y,w,h. Remember that wmctrl wants pixel sizes, not columns/rows, so if you were hoping to convert a text-
  terminal geometry like 124x48 into pixels you’d either need to know the terminal’s character cell dimensions or let the program set its own size
  and only adjust position (that’s the safer default). Titles can be set afterwards with another wmctrl -T.
  - Toolkit-agnostic
    Because you’re matching by PID you don’t need -qwindowtitle or any other toolkit-specific knob. It’ll work just as well against GTK, Qt,
  xterm, etc., as long as XWayland is present (which it must be if wmctrl works).
  - Packaging
    A small self-contained binary or script is straightforward. Python + argparse + subprocess gets you there in a hundred lines and can be
  shipped as a virtualenv, pipx, or a tiny .deb. A C/Rust/Go helper would be similarly compact. The only hard dependency is wmctrl (and therefore
  XWayland) plus whatever tool you want to use for the PID → window lookup.

  So: not difficult—just be explicit that it remains an X11/XWayland workaround (pure Wayland compositors refuse external positioning entirely).
  If you want to distribute it, bundle a clear README about needing wmctrl and the fact that you still rely on --geometry for sizing only.

▌ wmctrl seems to be a very old program and open-source and quite smallish. From wikipedia:
▌
▌ wmctrl is a command used to control windows in EWMH- and NetWM-compatible X Window window managers. Some of its common operations are list,
▌ resize, and close window. It also has the ability to interact with virtual desktops and give information about the window manager. wmctrl is a
▌ command-line program, however, it has some functions that allow the mouse to select a window for an operation
▌
▌ So the limitation is EWMH- or NetWM- compatible X Window managers? Is that an issue? For mainstream Linux desktops, Mint, Ubuntu, Kubuntu, etc?

> EWMH/NetWM compatibility is basically the “modern X desktop” baseline. Every mainstream environment you named—GNOME on Ubuntu, KDE Plasma
  (Kubuntu), Cinnamon/Xfce on Mint, even most tiling managers (i3, sway via XWayland)—exposes those hints, so wmctrl works out of the box. Only
  very old or intentionally minimal WMs that never adopted EWMH (e.g., vanilla twm, fluxbox before certain revisions) would be problematic. On
  Wayland the same support comes through XWayland: as long as the application you’re moving is running under XWayland (which is true for Konsole
  when you force QT_QPA_PLATFORM=xcb), wmctrl can do its job.

▌ Ok, this appears to be the repo: https://github.com/saravanabalagi/wmctrl, it has a single main.c which is about 1600 LoC.
▌
▌ I'm guessing at most we need a couple hundred LoC to implement our side of things, and unless there is already a libwmctrl, we extract what is
▌ needed from wmctrl:main.c, I suspect we'll still come in with a very smallish C program.

> Yeah, wmctrl itself is a single C file because it re‑implements a bunch of EWMH/Xlib plumbing by hand. For your helper you don’t need most of
  that:

  - Parsing the WIDTHxHEIGHT+X+Y token and spawning the target app is just a few dozen lines (open/process argv, fork/exec, keep the child PID).
  - Finding the window is the “hardest” part, but you can just reuse the wmctrl binary you already depend on: poll wmctrl -lp, match the PID, and
  record the window ID. That saves you from bundling any Xlib code at all.
  - Once you have the ID, a single execlp("wmctrl", "wmctrl", "-ir", wid, "-e", …, NULL) positions it. (You can optionally run a second wmctrl -T
  to set a custom title.)

  If you really want it completely self-contained and not shell out to wmctrl, you’d copy the handful of helper routines from main.c that talk to
  Xlib: open display, enumerate windows, read _NET_WM_PID, set _NET_MOVERESIZE_WINDOW. Even that probably lands under ~200 LoC once stripped down.

  There isn’t a packaged “libwmctrl”, but you don’t need the whole program—just the few calls that set geometry or read window props. So you can
  absolutely build a tiny wrapper with minimal code; using the existing wmctrl executable keeps it simpler, rolling your own Xlib plumbing keeps
  it dependency-free. Either way, it’s well within the “smallish C program” scope.



