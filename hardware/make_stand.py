"""Generate stand.stl for the ESP32-2432S024C desk stand (same geometry/parameters as stand.scad).
Run:  uv run --with trimesh --with manifold3d --with numpy python make_stand.py
"""
import math, numpy as np, trimesh
from trimesh.creation import box, cylinder, extrude_polygon
from shapely.geometry import Polygon
from hw_common import filleted_boss

# ---- parameters (keep in sync with stand.scad)
tilt, plate_t, base_t, wall_t = 65.0, 4.0, 4.0, 3.0
width, face_h, base_d = 54.0, 95.0, 60.0
boss_d, boss_h, boss_hole = 6.5, 5.0, 2.6
hole_dx, hole_dy = 68.0, 36.5
board_h, board_w = 74.4, 42.9
cable_w, cable_h, ledge_h, board_lift = 13.0, 9.0, 3.0, 16.0

def B(sx, sy, sz, x, y, z):
    """axis-aligned box with its min corner at (x, y, z)"""
    m = box(extents=[sx, sy, sz]); m.apply_translation([x + sx/2, y + sy/2, z + sz/2]); return m

def C(d, h, x, y, z):
    """z-axis cylinder, base at z"""
    m = cylinder(radius=d/2, height=h, sections=48); m.apply_translation([x, y, z + h/2]); return m

def union(parts):  return trimesh.boolean.union(parts, engine="manifold")
def diff(a, b):    return trimesh.boolean.difference([a, b], engine="manifold")

# ---- face plate in its own frame: X across, Y up the face, Z out of the face
face = B(width, face_h, plate_t, 0, 0, 0)
adds, cuts = [], []
cx = width / 2
by = board_lift + board_h / 2
for sx in (-1, 1):
    for sy in (-1, 1):
        hx, hy = cx + sx * hole_dy / 2, by + sy * hole_dx / 2
        adds.append(filleted_boss(boss_d, boss_h, hx, hy, plate_t - 0.01, fillet=1.5))
        cuts.append(C(boss_hole, plate_t + boss_h + 2, hx, hy, -1))
adds.append(B((board_w - cable_w) / 2, ledge_h, boss_h, (width - board_w) / 2, board_lift - ledge_h, plate_t - 0.01))
adds.append(B((board_w - cable_w) / 2, ledge_h, boss_h, cx + cable_w / 2, board_lift - ledge_h, plate_t - 0.01))
cuts.append(B(cable_w, board_lift + 1, plate_t + boss_h + 2, cx - cable_w / 2, -1, -1))
face = diff(union([face] + adds), union(cuts))

# lean the plate back by `tilt` from horizontal; its back (z=0) hinges at the origin, bosses face the front (-y)
rot = trimesh.transformations.rotation_matrix(math.radians(tilt), [1, 0, 0])
face.apply_transform(rot)

# ---- base, starting just under the plate's front edge (cable exits through the open back: no channel)
front = plate_t * math.sin(math.radians(tilt))
base = B(width, base_d + front, base_t, 0, -front, 0)

# ---- triangular side walls (in the Y-Z plane, extruded along X)
face_depth = face_h * math.cos(math.radians(tilt))
face_top = face_h * math.sin(math.radians(tilt))
# walls follow the back of the face plate but bite 1 mm into it so the union has real overlap (no coplanar contact)
st, ct = math.sin(math.radians(tilt)), math.cos(math.radians(tilt))
tri = Polygon([(-st, ct - 0.5), (face_depth - st, face_top + ct), (base_d, 0), (0, -0.5)])
walls = []
for x in (0.0, width - wall_t):
    w = extrude_polygon(tri, wall_t)                    # in XY plane, extruded along Z
    # map: polygon x -> world Y (depth), polygon y -> world Z (height), extrusion -> world X
    m = np.array([[0, 0, 1, x], [1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=float)
    w.apply_transform(m)
    walls.append(w)

for name, part in (("face", face), ("base", base), ("wall0", walls[0]), ("wall1", walls[1])):
    print(f"  {name}: watertight={part.is_watertight} volume={part.volume:.0f}")
stand = union([face, base] + walls)
print(f"  union: watertight={stand.is_watertight} bodies={stand.body_count}")
if not stand.is_watertight:                        # fall back to the CSG engine's own mesh output
    import manifold3d as mf
    def to_mf(m): return mf.Manifold(mf.Mesh(vert_properties=np.asarray(m.vertices, dtype=np.float32), tri_verts=np.asarray(m.faces, dtype=np.uint32)))
    acc = to_mf(face)
    for part in [base] + walls: acc = acc + to_mf(part)
    mm = acc.to_mesh()
    stand = trimesh.Trimesh(vertices=np.asarray(mm.vert_properties)[:, :3], faces=np.asarray(mm.tri_verts), process=False)
    print(f"  manifold direct: watertight={stand.is_watertight} volume={stand.volume/1000:.1f}")
stand.export("stand.stl")
ext = stand.bounding_box.extents
print(f"stand.stl: {len(stand.faces)} faces, size {ext[0]:.1f} x {ext[1]:.1f} x {ext[2]:.1f} mm, volume {stand.volume/1000:.1f} cm3, watertight={stand.is_watertight}")
