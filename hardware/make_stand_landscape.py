"""Landscape desk stand for the ESP32-2432S024C (board on its side, USB-C to the right, cable in free air).
Run:  uv run --with trimesh --with manifold3d --with numpy --with shapely --with scipy python make_stand_landscape.py
"""
import math, numpy as np, trimesh
from trimesh.creation import box, cylinder, extrude_polygon
from shapely.geometry import Polygon
from hw_common import filleted_boss

tilt, plate_t, base_t, wall_t = 65.0, 4.0, 4.0, 3.0
board_l, board_w = 74.4, 42.9              # board length (now across the face), width (now up the face)
hole_dx, hole_dy = 68.0, 36.5              # hole grid: 68 across, 36.5 up
margin = 4.0
width  = board_l + 2 * margin              # 82.4 across
board_lift = 6.0                           # ledge height: board bottom edge sits here
face_h = board_lift + board_w + margin     # 53.9 up the face
base_d = 48.0
boss_d, boss_h, boss_hole = 6.5, 5.0, 2.6
ledge_h = 3.0

def B(sx, sy, sz, x, y, z):
    m = box(extents=[sx, sy, sz]); m.apply_translation([x + sx/2, y + sy/2, z + sz/2]); return m
def C(d, h, x, y, z):
    m = cylinder(radius=d/2, height=h, sections=48); m.apply_translation([x, y, z + h/2]); return m
def union(parts): return trimesh.boolean.union(parts, engine="manifold")
def diff(a, b):   return trimesh.boolean.difference([a, b], engine="manifold")

cx = width / 2
by = board_lift + board_w / 2
face = B(width, face_h, plate_t, 0, 0, 0)
adds, cuts = [], []
for sx in (-1, 1):
    for sy in (-1, 1):
        hx, hy = cx + sx * hole_dx / 2, by + sy * hole_dy / 2
        adds.append(filleted_boss(boss_d, boss_h, hx, hy, plate_t - 0.01, fillet=1.5))
        cuts.append(C(boss_hole, plate_t + boss_h + 2, hx, hy, -1))
# continuous ledge under the board's long bottom edge (USB is on the right short edge, in free air)
adds.append(B(board_l - 4, ledge_h, boss_h, cx - (board_l - 4) / 2, board_lift - ledge_h, plate_t - 0.01))
face = diff(union([face] + adds), union(cuts))
face.apply_transform(trimesh.transformations.rotation_matrix(math.radians(tilt), [1, 0, 0]))

front = plate_t * math.sin(math.radians(tilt))
base = B(width, base_d + front, base_t, 0, -front, 0)

face_depth = face_h * math.cos(math.radians(tilt)); face_top = face_h * math.sin(math.radians(tilt))
st, ct = math.sin(math.radians(tilt)), math.cos(math.radians(tilt))
tri = Polygon([(-st, ct - 0.5), (face_depth - st, face_top + ct), (base_d, 0), (0, -0.5)])
walls = []
for x in (0.0, width - wall_t):
    w = extrude_polygon(tri, wall_t)
    w.apply_transform(np.array([[0, 0, 1, x], [1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=float))
    walls.append(w)
# a centre rib keeps the wide face from flexing
rib = extrude_polygon(tri, wall_t); rib.apply_transform(np.array([[0, 0, 1, cx - wall_t / 2], [1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=float))

stand = union([face, base, rib] + walls)
if not stand.is_watertight:
    import manifold3d as mf
    def to_mf(m): return mf.Manifold(mf.Mesh(vert_properties=np.asarray(m.vertices, dtype=np.float32), tri_verts=np.asarray(m.faces, dtype=np.uint32)))
    acc = to_mf(face)
    for part in [base, rib] + walls: acc = acc + to_mf(part)
    mm = acc.to_mesh(); stand = trimesh.Trimesh(vertices=np.asarray(mm.vert_properties)[:, :3], faces=np.asarray(mm.tri_verts), process=False)
stand.export("stand-landscape.stl")
e = stand.bounding_box.extents
print(f"stand-landscape.stl: {len(stand.faces)} faces, {e[0]:.1f} x {e[1]:.1f} x {e[2]:.1f} mm, {stand.volume/1000:.1f} cm3, watertight={stand.is_watertight}")
