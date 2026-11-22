#!/usr/bin/env python3
"""Writes suites/prax-paths: shapes that the path library's fills and combinations are checked over
(tools/suite/prax/paths.py). The shapes are random from fixed seeds, so the corpus is the same on every run, and is
written here rather than drawn by hand, so that a shape's kind is read from its generator.

A shape is a file of SVG path data, a contour on each line. A file of curves also gives the box that its fill is cut
to, on a line `box <left> <top> <width> <height>`. A file of pairs holds two shapes, the second after an empty line.

Usage: python3 -m tools.suite.prax.gen_paths
"""

import os
import random

import tools

ROOT = os.path.join(tools.ROOT, "suites", "prax-paths")

# The shapes of each directory.
COUNT = {"polygons": 60, "curves": 60, "pairs": 12}


def number(value):
    return ("%.4f" % value).rstrip("0").rstrip(".")


def contour(points, curves=()):
    """A closed contour through `points`, as path data. A point whose index is in `curves` is reached by a quadratic
    curve through its control point, given as (control, point)."""
    words = ["M %s %s" % (number(points[0][0]), number(points[0][1]))]
    for i, p in enumerate(points[1:], 1):
        if i in curves:
            control, end = p
            words.append("Q %s %s %s %s" % (number(control[0]), number(control[1]), number(end[0]), number(end[1])))
        else:
            words.append("L %s %s" % (number(p[0]), number(p[1])))
    return " ".join(words) + " Z"


def polygons(rng, index):
    """One to three rings of three to fourteen points within 100 by 100. Every fourth shape snaps its points to a grid
    of 20, for shared vertices and collinear edges."""
    rings = []
    for _ in range(1 + index % 3):
        points = []
        for _ in range(3 + int(rng.random() * 12)):
            x, y = rng.random() * 100, rng.random() * 100
            if index % 4 == 0:
                x, y = x // 20 * 20, y // 20 * 20
            points.append((x, y))
        rings.append(contour(points))
    return "\n".join(rings) + "\n"


def curves(rng, index):
    """One to three contours through random points within 120 by 120, a third of their segments quadratic curves, and
    a box that cuts them on some sides."""
    size = 120
    shifted = (rng.random() * 3 - 1.5, rng.random() * 3 - 1.5)

    def point():
        return (rng.random() * size + shifted[0], rng.random() * size + shifted[1])

    lines = []
    for _ in range(1 + int(rng.random() * 3)):
        points = [point()]
        bends = set()
        for i in range(2 + int(rng.random() * 8)):
            if rng.random() < 0.3:
                bends.add(i + 1)
                points.append((point(), point()))
            else:
                points.append(point())
        lines.append(contour(points, bends))
    left, top = int(rng.random() * 30) - 10, int(rng.random() * 30) - 10
    lines.append("box %d %d %d %d" % (left, top, 60 + int(rng.random() * 80), 60 + int(rng.random() * 80)))
    return "\n".join(lines) + "\n"


def pairs(rng, index):
    """Two self-intersecting polygons within 100 by 100, of more points with each shape."""
    def polygon(count):
        return contour([(rng.random() * 100, rng.random() * 100) for _ in range(count)])

    return "%s\n\n%s\n" % (polygon(12 + index), polygon(9 + index))


def main():
    makers = {"polygons": polygons, "curves": curves, "pairs": pairs}
    for directory, make in makers.items():
        rng = random.Random(directory)
        os.makedirs(os.path.join(ROOT, directory), exist_ok=True)
        for index in range(COUNT[directory]):
            with open(os.path.join(ROOT, directory, "%03d.txt" % index), "w", encoding="utf-8", newline="\n") as f:
                f.write(make(rng, index))


if __name__ == "__main__":
    main()
