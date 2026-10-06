#!/usr/bin/env python3
"""Generate a synthetic legacy-format dataset for modern/legacy interchange tests.

Requires Pillow. No real camera data is used. The analytic preview is a format
fixture, not output produced by either reconstruction algorithm.
"""
import argparse
import json
import math
from pathlib import Path
import struct

from PIL import Image


def multiply(a, b):
    return [[sum(a[row][k] * b[k][column] for k in range(4))
             for column in range(4)] for row in range(4)]


def colour(x, y):
    return (220, 70, 40) if (math.floor((x + 2) * 8) + math.floor((y + 2) * 8)) % 2 else (40, 170, 220)


def generate(destination):
    destination.mkdir(parents=True, exist_ok=False)
    width = height = 64
    focal = 80.0
    centre = 32.0
    depth = 2.0
    near, far = .01, 100.0
    projection = [[2 * focal / width, 0, 0, 0], [0, 2 * focal / height, 0, 0],
                  [0, 0, (far + near) / (near - far), 2 * far * near / (near - far)],
                  [0, 0, -1, 0]]
    vertices = [(-.8, -.8, depth), (.8, -.8, depth), (.8, .8, depth), (-.8, .8, depth)]
    faces = [(0, 2, 1), (0, 3, 2)]
    preview = struct.pack("<i3iII", 1, 0, 0, 0, len(faces), len(vertices))
    preview += b"".join(struct.pack("<3f", *vertex) for vertex in vertices)
    preview += struct.pack("<3f", 0, 0, -1) * len(vertices)
    preview += b"".join(bytes((*colour(x, y), 255)) for x, y, _ in vertices)
    preview += b"".join(struct.pack("<3I", *face) for face in faces)

    for index, camera_x in enumerate((-.08, 0.0, .08)):
        stem = destination / f"{index:08d}"
        camera = [[1, 0, 0, camera_x], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
        gl_camera = [[1, 0, 0, camera_x], [0, -1, 0, 0], [0, 0, -1, 0], [0, 0, 0, 1]]
        gl_view = [[1, 0, 0, -camera_x], [0, -1, 0, 0], [0, 0, -1, 0], [0, 0, 0, 1]]
        poses = (camera, gl_camera, multiply(projection, gl_view))
        # Dataset::WritePose serializes each GLM column as one text row.
        stem.with_suffix(".mat").write_text("".join(
            " ".join(format(matrix[row][column], ".9g") for row in range(4)) + "\n"
            for matrix in poses for column in range(4)))
        stem.with_suffix(".tms").write_text(f"{index / 30:.9f}\n")
        points = [((u - centre) * depth / focal, (v - centre) * depth / focal, depth, 1.0)
                  for v in range(0, height, 2) for u in range(0, width, 2)]
        stem.with_suffix(".pcl").write_bytes(struct.pack("<I", len(points)) +
                                             b"".join(struct.pack("<4f", *p) for p in points))
        stem.with_suffix(".bin").write_bytes(preview)
        image = Image.new("RGB", (width, height))
        image.putdata([colour(camera_x + (u - centre) * depth / focal,
                              (v - centre) * depth / focal)
                       for v in range(height) for u in range(width)])
        image.save(stem.with_suffix(".jpg"), quality=97, subsampling=0)

    (destination / "distortion.txt").write_text("3\n0\n0\n0\n")
    (destination / "rotation.txt").write_text("-90\n")
    (destination / "fixture.json").write_text(json.dumps({
        "synthetic": True, "frames": 3, "points_per_frame": 1024,
        "plane_world_z_metres": depth, "camera_x_metres": [-.08, 0, .08],
        "preview_is_analytic_format_fixture": True,
        "coordinate_convention": "camera +Z forward/+Y down; GL camera is colour pose times Rx(pi)",
    }, indent=2) + "\n")
    # Commit marker last, matching the live writer's convention.
    (destination / "state.txt").write_text(f"3 {width} {height} {centre} {centre} {focal} {focal}\n")
    print("Generated synthetic legacy-format dataset:", destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path, help="New directory; existing paths are refused")
    generate(parser.parse_args().destination)
