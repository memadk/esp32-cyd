"""Shared solid primitives for the 3D-printed parts: bosses with a fillet at the foot, countersinks.
A fillet where a tube meets a plate adds material exactly where a printed joint would otherwise crack."""
import math, numpy as np, trimesh
from trimesh.creation import revolve

def _arc(cx, cz, r, a0, a1, n=10):
    return [(cx + r * math.cos(a), cz + r * math.sin(a)) for a in np.linspace(a0, a1, n)]

def filleted_boss(d, h, x, y, z, fillet=1.5, sections=48):
    """Cylinder of diameter d and height h standing on z, with a concave fillet of radius `fillet` at the foot."""
    r = d / 2
    prof = [(0.0, 0.0), (r + fillet, 0.0)]
    prof += _arc(r + fillet, fillet, fillet, -math.pi / 2, -math.pi)[1:]     # concave quarter circle up to (r, fillet)
    prof += [(r, h), (0.0, h)]
    m = revolve(prof, sections=sections)
    m.apply_translation([x, y, z])
    return m

def countersunk_hole(d_hole, d_head, length, x, y, z_face, into=+1, sections=48):
    """Through hole of d_hole with a 90-degree countersink (d_head at the face z_face). The hole runs from the face
    `length` mm in direction `into` (+1 = towards +z, -1 = towards -z). No reflections: the profile is drawn directly."""
    r, R = d_hole / 2, d_head / 2
    cone_h = R - r                                    # 45-degree wall
    zs = lambda v: into * v                           # signed depth
    prof = [(0.0, zs(-0.5)), (R + 0.5, zs(-0.5)), (R, 0.0), (r, zs(cone_h)), (r, zs(length)), (0.0, zs(length))]
    if into < 0: prof = prof[::-1]                    # keep the profile counter-clockwise for a proper volume
    m = revolve(prof, sections=sections)
    if not m.is_volume: m.fix_normals()
    m.apply_translation([x, y, z_face])
    return m

def plain_hole(d, length, x, y, z, sections=48):
    prof = [(0.0, 0.0), (d / 2, 0.0), (d / 2, length), (0.0, length)]
    m = revolve(prof, sections=sections); m.apply_translation([x, y, z]); return m
