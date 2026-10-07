#!/usr/bin/env python3
"""Build the "emergency" Xcursor theme: the cursors compiled into
libwayland-cursor (its fallback when no theme is installed), unchanged,
each padded into a 24x24 image of nominal size 24.

Why: Mahogany and GTK 3 draw the fallback images (10x16 and so on) pixel for
pixel, but GTK 4 stretches whatever image it gets to its cursor size, so the
same cursor came out two to three times bigger over GTK 4 windows.  With a
real theme at the requested size GTK 4 draws it 1:1 as well.

usage: mkcursors.py WAYLAND-SRC/cursor/cursor-data.h OUTDIR
"""
import os, re, struct, sys

SIZE = 24
src = open(sys.argv[1]).read()
out = sys.argv[2]

data_text = src[src.index("cursor_data[]"):]
data_text = data_text[data_text.index("{") + 1:data_text.index("};")]
data = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", data_text)]

meta = re.findall(r'\{\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}',
                  src[src.index("cursor_metadata[]"):])

def xcursor(w, h, hx, hy, off):
    pixels = [0] * (SIZE * SIZE)
    for y in range(h):
        for x in range(w):
            pixels[y * SIZE + x] = data[off + y * w + x]
    header = struct.pack("<4sIII", b"Xcur", 16, 0x10000, 1)
    toc = struct.pack("<III", 0xfffd0002, SIZE, 16 + 12)
    chunk = struct.pack("<IIIIIIIII", 36, 0xfffd0002, SIZE, 1, SIZE, SIZE, hx, hy, 0)
    return header + toc + chunk + struct.pack("<%dI" % len(pixels), *pixels)

cdir = os.path.join(out, "cursors")
os.makedirs(cdir, exist_ok=True)
written = {}
for name, w, h, hx, hy, off in meta:
    key = (int(w), int(h), int(hx), int(hy), int(off))
    path = os.path.join(cdir, name)
    if key in written:                     # same image under another name: link it
        if os.path.lexists(path): os.unlink(path)
        os.symlink(written[key], path)
    else:
        open(path, "wb").write(xcursor(*key))
        written[key] = name

# Names toolkits ask for that the built-in table lacks: closest shape.
aliases = {
    "left_ptr": ["arrow", "top_left_arrow", "context-menu", "help", "question_arrow",
                 "copy", "alias", "cell", "progress", "left_ptr_watch", "no-drop",
                 "not-allowed", "crossed_circle", "dnd-none", "dnd-copy", "dnd-move",
                 "dnd-link", "dnd-ask", "zoom-in", "zoom-out", "center_ptr", "pirate"],
    "xterm": ["ibeam", "vertical-text"],
    "hand1": ["hand2", "hand", "pointing_hand"],
    "grabbing": ["grab", "openhand", "closedhand", "fleur", "move", "size_all",
                 "crosshair", "cross", "tcross"],
    "left_side": ["col-resize", "ew-resize", "sb_h_double_arrow", "size_hor", "h_double_arrow"],
    "top_side": ["row-resize", "ns-resize", "sb_v_double_arrow", "size_ver", "v_double_arrow"],
    "top_left_corner": ["nwse-resize", "size_fdiag", "fd_double_arrow"],
    "top_right_corner": ["nesw-resize", "size_bdiag", "bd_double_arrow"],
}
for target, names in aliases.items():
    for n in names:
        p = os.path.join(cdir, n)
        if not os.path.lexists(p):
            os.symlink(target, p)

open(os.path.join(out, "index.theme"), "w").write(
    "[Icon Theme]\nName=emergency\nComment=libwayland-cursor's built-in cursors at 24 px\n")
print(len(written), "images,", len(os.listdir(cdir)), "names")
