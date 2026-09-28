#!/usr/bin/env python3
"""Project the four microphone grill faces through OpenSCAD cameras.

For each still rendered with --projection=p --camera=eye,centre, write where
each grill's face outline lands in the image, as an ellipse, so overlays
(rings, callouts) can sit exactly on the grills.

    grill-ellipses.py OUT.json WIDTH HEIGHT FILE=EYE_X,EYE_Y,EYE_Z,CX,CY,CZ ...

The grill face is the outer edge of the fitting's flat Ø19 top, a circle of
radius 9.5 mm at z = 10.8 mm centred on (x, 0, 10.8) for the socket
positions in 3dprint/micarray.scad (M1 = x -45 ... M4 = x +45).

OpenSCAD's perspective camera looks from the eye at the centre with world +Z
up (no roll) and a 22.5 degree vertical field of view; the image plane maps
[-1, 1] normalised device coordinates onto the pixel edges. That model
reproduces rendered silhouettes to within a pixel at 3840x2160.
"""

import json
import sys

import numpy as np

FOV_Y_DEG = 22.5
MIC_X = [-45.0, -15.0, 15.0, 45.0]
FACE_Z = 10.8
FACE_R = 9.5


def project(points, eye, centre, width, height):
    forward = centre - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0.0, 0.0, 1.0])
    right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    rel = points - eye
    depth = rel @ forward
    if np.any(depth <= 0):
        raise ValueError("point behind the camera")
    t = np.tan(np.radians(FOV_Y_DEG) / 2)
    sx = (rel @ right) / depth / (t * width / height)
    sy = (rel @ up) / depth / t
    return np.stack([(sx + 1) / 2 * width, (1 - sy) / 2 * height], axis=1)


def fit_ellipse(xy):
    """Exact conic through the projected circle: centre, semi-axes, angle."""
    mean = xy.mean(axis=0)
    scale = np.abs(xy - mean).max()
    x, y = ((xy - mean) / scale).T
    design = np.stack([x * x, x * y, y * y, x, y, np.ones_like(x)], axis=1)
    a, b, c, d, e, f = np.linalg.svd(design)[2][-1]
    x0, y0 = np.linalg.solve([[2 * a, b], [b, 2 * c]], [-d, -e])
    f0 = a * x0 * x0 + b * x0 * y0 + c * y0 * y0 + d * x0 + e * y0 + f
    values, vectors = np.linalg.eigh([[a, b / 2], [b / 2, c]])
    axes = np.sqrt(-f0 / values)
    major = int(np.argmax(axes))
    vx, vy = vectors[:, major]
    angle = np.degrees(np.arctan2(vy, vx))
    angle = (angle + 90) % 180 - 90   # (-90, 90]
    if abs(axes[0] - axes[1]) < 1e-3 * axes.max():
        angle = 0.0                   # a circle: no meaningful axis
    return (mean + np.array([x0, y0]) * scale,
            axes[major] * scale, axes[1 - major] * scale, angle)


def main():
    out, width, height = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    theta = np.linspace(0, 2 * np.pi, 720, endpoint=False)
    frames = []
    for arg in sys.argv[4:]:
        name, camera = arg.split("=", 1)
        values = [float(v) for v in camera.split(",")]
        eye, centre = np.array(values[:3]), np.array(values[3:])
        mics = []
        for i, mx in enumerate(MIC_X):
            ring = np.stack([mx + FACE_R * np.cos(theta),
                             FACE_R * np.sin(theta),
                             np.full_like(theta, FACE_Z)], axis=1)
            (cx, cy), rx, ry, angle = fit_ellipse(
                project(ring, eye, centre, width, height))
            point = project(np.array([[mx, 0.0, FACE_Z]]),
                            eye, centre, width, height)[0]
            mics.append({
                "id": f"M{i + 1}",
                "x_mm": mx,
                "x": round(float(cx), 2),
                "y": round(float(cy), 2),
                "rx": round(float(rx), 2),
                "ry": round(float(ry), 2),
                "rotation_deg": round(float(angle), 2),
                "face_centre_point": [round(float(point[0]), 2),
                                      round(float(point[1]), 2)],
            })
        frames.append({"file": name, "camera": camera, "mics": mics})

    doc = {
        "image_size": [width, height],
        "coordinates": "pixels from the image's top-left corner, x right, "
                       "y down; pixel (i, j) covers [i, i+1) x [j, j+1)",
        "grill_face": "outer edge of the flat 19 mm face at z = 10.8 mm, "
                      "centred on (x_mm, 0, 10.8); M1 is x = -45",
        "ellipse": "x, y = centre of the projected outline; rx, ry = "
                   "semi-major and semi-minor axes; rotation_deg = angle "
                   "of the rx axis from +x, clockwise on screen, as in SVG "
                   "rotate() and CSS rotate(). face_centre_point is the "
                   "projected face centre, which perspective shifts "
                   "slightly from the ellipse centre",
        "camera": "OpenSCAD --projection=p --camera=eye_x,eye_y,eye_z,"
                  "centre_x,centre_y,centre_z (mm), 22.5 degree vertical "
                  "field of view, +Z up",
        "frames": frames,
    }
    with open(out, "w") as fh:
        json.dump(doc, fh, indent=2)
        fh.write("\n")


if __name__ == "__main__":
    main()
