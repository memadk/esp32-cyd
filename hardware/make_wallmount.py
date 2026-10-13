"""Wall-mount enclosure for the Sunton ESP32-2432S024C (2 parts: shell + bezel).
Run:  uv run --with trimesh --with manifold3d --with numpy --with shapely --with scipy python make_wallmount.py
Produces wallmount-shell.stl and wallmount-bezel.stl. Same parameters as wallmount.scad.
Frame: X across the board (width), Y along the board (length, USB-C end at the bottom), Z out of the wall.
"""
import math, numpy as np, trimesh
from trimesh.creation import extrude_polygon
from shapely.geometry import Polygon, box as sbox, Point
from hw_common import filleted_boss, countersunk_hole, plain_hole

# ---- board (factory drawing) ----
board_l, board_w = 74.4, 42.9
hole_dx, hole_dy = 68.0, 36.5          # hole grid along / across
# ---- enclosure ----
clear      = 0.6                        # clearance around the PCB, per side
wall       = 2.4
plate_t    = 3.0                        # back plate against the wall
boss_h     = 5.0                        # PCB standoff (clears ESP32 module, USB-C, SD slot on the back)
boss_d     = 6.5
boss_hole  = 2.6                        # M3 self-tapping
pcb_t      = 1.6
glass_h    = 4.5                        # display + capacitive touch glass above the PCB
bezel_gap  = 0.3                        # air between glass and bezel
bezel_t    = 2.0
plug_len   = 14.0                       # room below the board for a straight USB-C plug body
plug_w     = 13.0                       # bottom notch for the plug / cable
plug_dx    = -2.5                       # USB-C connector is ~2.5 mm left of the board centre (seen from the front, USB down)
rear_hole  = 10.0                       # rear cable hole diameter (0 = none) for in-wall cabling
corner_r   = 3.0
window_w, window_h = 41.0, 54.0         # bezel window (2.4" active area is 36.7 x 49; margin for the glass border)
window_dy  = 2.0                        # active area sits above the PCB centre: window shifted away from the USB end
keyhole_y  = (18.0, 58.0)               # keyhole positions from the TOP edge, on the centre line
keyhole_d, keyhole_slot_w, keyhole_slot_l = 8.0, 4.5, 9.0
head_recess_d, head_recess_t = 9.5, 2.0

W = board_w + 2 * clear + 2 * wall
L = board_l + 2 * clear + plug_len + 2 * wall
inner_h = boss_h + pcb_t + glass_h + bezel_gap      # shell wall height above the plate
shell_h = plate_t + inner_h
cx = W / 2
y0 = wall + clear + plug_len                        # board bottom edge
cy = y0 + board_l / 2
holes = [(cx + sx * hole_dy / 2, cy + sy * hole_dx / 2) for sx in (-1, 1) for sy in (-1, 1)]

def rrect(x0, y0_, w, h, r):
    return sbox(x0 + r, y0_ + r, x0 + w - r, y0_ + h - r).buffer(r, join_style=1, resolution=12)

def prism(poly, h, z=0.0):
    m = extrude_polygon(poly, h); m.apply_translation([0, 0, z]); return m

def cyl(d, h, x, y, z):
    return prism(Point(x, y).buffer(d / 2, resolution=24), h, z)

def union(parts): return trimesh.boolean.union(parts, engine="manifold")
def diff(a, parts): return trimesh.boolean.difference([a] + parts, engine="manifold")

# ================= shell =================
outer = prism(rrect(0, 0, W, L, corner_r), shell_h)
cavity = prism(rrect(wall, wall, W - 2 * wall, L - 2 * wall, max(corner_r - wall, 0.5)), inner_h + 1, plate_t)
shell = diff(outer, [cavity])
# standoffs (added after the cavity so they stand on the plate)
shell = union([shell] + [filleted_boss(boss_d, boss_h, x, y, plate_t - 0.01, fillet=2.0) for x, y in holes])
cuts = []
cuts += [cyl(boss_hole, boss_h + 1.5, x, y, plate_t - 1.0) for x, y in holes]           # blind screw holes
cuts.append(prism(sbox(cx + plug_dx - plug_w / 2, -1, cx + plug_dx + plug_w / 2, wall + 1), shell_h, plate_t + 2.0))   # bottom notch
if rear_hole > 0:
    cuts.append(cyl(rear_hole, plate_t + 2, cx + plug_dx, wall + clear + plug_len / 2, -1))   # rear cable hole under the plug
for ky in keyhole_y:                                                                     # keyholes: big hole, slot upward
    y = L - ky
    cuts.append(cyl(keyhole_d, plate_t + 2, cx, y, -1))
    cuts.append(prism(sbox(cx - keyhole_slot_w / 2, y, cx + keyhole_slot_w / 2, y + keyhole_slot_l), plate_t + 2, -1))
    cuts.append(prism(sbox(cx - head_recess_d / 2, y - keyhole_d / 2, cx + head_recess_d / 2, y + keyhole_slot_l + 1),
                      head_recess_t + 1, plate_t - head_recess_t))                       # recess for the screw head (inside)
shell = diff(shell, cuts)

# ================= bezel =================
bezel = prism(rrect(0, 0, W, L, corner_r), bezel_t)
spacer_h = glass_h + bezel_gap - 0.2
bezel = union([bezel] + [filleted_boss(boss_d, spacer_h, x, y, bezel_t - 0.01, fillet=1.5) for x, y in holes])   # spacers with a fillet at the plate
bcuts = [prism(rrect(cx - window_w / 2, cy + window_dy - window_h / 2, window_w, window_h, 2.0), bezel_t + spacer_h + 2, -1)]
# M3 through holes with a 90-degree countersink on the front face (z = 0); the spacers grow towards +z
for x, y in holes:
    bcuts.append(countersunk_hole(3.4, 6.6, bezel_t + spacer_h + 2, x, y, 0.0, into=+1))
bezel = diff(bezel, bcuts)

for name, m in (("wallmount-shell.stl", shell), ("wallmount-bezel.stl", bezel)):
    m.export(name)
    e = m.bounding_box.extents
    print(f"{name}: {len(m.faces)} faces, {e[0]:.1f} x {e[1]:.1f} x {e[2]:.1f} mm, {m.volume/1000:.1f} cm3, watertight={m.is_watertight}")
print(f"assembled depth {shell_h + bezel_t:.1f} mm; screws: 4 x M3 x {math.ceil(bezel_t + spacer_h + pcb_t + boss_h - 1):d} mm")
