# Shapes

`suites/prax-paths` holds shapes that lib/path's fills and combinations are checked over: random polygons and curves
from fixed seeds, which `python3 -m tools.suite.prax.gen_paths` writes. A shape is a file of SVG path data, a contour
on each line. The runner (`tools/suite/prax/paths.py`) and its harness (`tools/suite/prax/path_check.cpp`) check each
file by what its directory holds.

| directory | shapes | checked |
| --- | --- | --- |
| `polygons` | one to three rings of three to fourteen points, every fourth snapped to a grid of 20 for shared vertices and collinear edges | the tessellator's triangles cover each point of a grid as each fill rule says, and with a fringe wherever the point lies away from the boundary |
| `curves` | one to three contours of lines and quadratic curves, with a box that cuts them | the sparse rasterizer's spans of each kind cover each pixel at most once and as the rasterizer covers it, by each fill rule |
| `pairs` | two self-intersecting polygons | each combination winds once around what it covers, and covers each point as the two shapes' windings say |
