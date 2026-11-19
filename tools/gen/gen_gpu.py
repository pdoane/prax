#!/usr/bin/env python3
"""Generates the table of lib/gpu's formats (lib/gpu/src/gpu_formats.h): each format's block, family, sRGB and plain
forms, the format that a swizzled format stores its texels in, and what it holds, from the list below. The list is in
the order of PrGpuFormat, which pr/gpu.h writes out with its documentation, and the generator refuses to write a table
for an enumeration of another order."""

import os
import re

from tools.gen.gen_common import ROOT, banner, namespaced, write

OUT = os.path.join(ROOT, "lib", "gpu", "src", "gpu_formats.h")
HEADER = os.path.join(ROOT, "lib", "gpu", "include", "pr", "gpu.h")

# The formats, each with its block's width, height and bytes and its family. `plain` names the format whose Srgb form a
# format is, `stored` the format that a swizzled format samples, under `swizzle`.
FORMATS = []


def fmt(name, size, family="Plain", width=1, height=1, plain=None, stored=None, swizzle="None", depth=False):
    FORMATS.append(dict(name=name, width=width, height=height, bytes=size, family=family, plain=plain, stored=stored,
                        swizzle=swizzle, depth=depth, srgb=None))


def swizzled(base, size, family="Plain", width=1, height=1):
    """The four swizzled formats of a format of one channel."""
    for swizzle in ("Alpha", "Luminance", "Intensity", "Mask"):
        fmt(base + swizzle, size, family, width, height, stored=base, swizzle=swizzle)


fmt("None", 0, width=0, height=0)

# 8 bits a channel
fmt("R8", 1)
fmt("R8Snorm", 1)
fmt("R8Uint", 1)
fmt("R8Sint", 1)
fmt("R8Srgb", 1, plain="R8")
swizzled("R8", 1)
fmt("Rg8", 2)
fmt("Rg8Snorm", 2)
fmt("Rg8Uint", 2)
fmt("Rg8Sint", 2)
fmt("Rg8Srgb", 2, plain="Rg8")
fmt("Rg8LuminanceAlpha", 2, stored="Rg8", swizzle="LuminanceAlpha")
fmt("Rgba8", 4)
fmt("Rgba8Snorm", 4)
fmt("Rgba8Uint", 4)
fmt("Rgba8Sint", 4)
fmt("Rgba8Srgb", 4, plain="Rgba8")
fmt("Bgra8", 4)
fmt("Bgra8Srgb", 4, plain="Bgra8")

# 16 bits a channel
fmt("R16", 2)
fmt("R16Snorm", 2)
fmt("R16Uint", 2)
fmt("R16Sint", 2)
fmt("R16F", 2)
swizzled("R16", 2)
swizzled("R16F", 2)
fmt("Rg16", 4)
fmt("Rg16Snorm", 4)
fmt("Rg16Uint", 4)
fmt("Rg16Sint", 4)
fmt("Rg16F", 4)
fmt("Rg16LuminanceAlpha", 4, stored="Rg16", swizzle="LuminanceAlpha")
fmt("Rg16FLuminanceAlpha", 4, stored="Rg16F", swizzle="LuminanceAlpha")
fmt("Rgba16", 8)
fmt("Rgba16Snorm", 8)
fmt("Rgba16Uint", 8)
fmt("Rgba16Sint", 8)
fmt("Rgba16F", 8)

# 32 bits a channel
fmt("R32Uint", 4)
fmt("R32Sint", 4)
fmt("R32F", 4)
swizzled("R32F", 4)
fmt("Rg32Uint", 8)
fmt("Rg32Sint", 8)
fmt("Rg32F", 8)
fmt("Rg32FLuminanceAlpha", 8, stored="Rg32F", swizzle="LuminanceAlpha")
fmt("Rgba32Uint", 16)
fmt("Rgba32Sint", 16)
fmt("Rgba32F", 16)

# Packed into 16 or 32 bits
fmt("B5G6R5", 2)
fmt("Bgr5A1", 2)
fmt("Bgra4", 2)
fmt("Rgb10A2", 4)
fmt("Rgb10A2Uint", 4)
fmt("Bgr10A2", 4)
fmt("Rg11B10F", 4)
fmt("Rgb9E5F", 4)

# Depth and stencil
fmt("D16", 2, depth=True)
fmt("D24S8", 4, depth=True)
fmt("D32F", 4, depth=True)
fmt("D32FS8", 8, depth=True)
fmt("S8", 1, depth=True)

# BC
for name, size in (("Bc1", 8), ("Bc2", 16), ("Bc3", 16)):
    fmt(name, size, "Bc", 4, 4)
    fmt(name + "Srgb", size, "Bc", 4, 4, plain=name)
fmt("Bc4", 8, "Bc", 4, 4)
fmt("Bc4Snorm", 8, "Bc", 4, 4)
swizzled("Bc4", 8, "Bc", 4, 4)
fmt("Bc5", 16, "Bc", 4, 4)
fmt("Bc5Snorm", 16, "Bc", 4, 4)
fmt("Bc5LuminanceAlpha", 16, "Bc", 4, 4, stored="Bc5", swizzle="LuminanceAlpha")
fmt("Bc6HUfloat", 16, "Bc", 4, 4)
fmt("Bc6HSfloat", 16, "Bc", 4, 4)
fmt("Bc7", 16, "Bc", 4, 4)
fmt("Bc7Srgb", 16, "Bc", 4, 4, plain="Bc7")

# ETC2 and EAC
for name, size in (("Etc2Rgb8", 8), ("Etc2Rgb8A1", 8), ("Etc2Rgba8", 16)):
    fmt(name, size, "Etc2", 4, 4)
    fmt(name + "Srgb", size, "Etc2", 4, 4, plain=name)
fmt("EacR11", 8, "Etc2", 4, 4)
fmt("EacR11Snorm", 8, "Etc2", 4, 4)
swizzled("EacR11", 8, "Etc2", 4, 4)
fmt("EacRg11", 16, "Etc2", 4, 4)
fmt("EacRg11Snorm", 16, "Etc2", 4, 4)
fmt("EacRg11LuminanceAlpha", 16, "Etc2", 4, 4, stored="EacRg11", swizzle="LuminanceAlpha")

# ASTC: Unorm, Srgb and Hdr of each block size
for width, height in ((4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8), (10, 5), (10, 6), (10, 8),
                      (10, 10), (12, 10), (12, 12)):
    name = "Astc%dx%d" % (width, height)
    fmt(name, 16, "Astc", width, height)
    fmt(name + "Srgb", 16, "Astc", width, height, plain=name)
    fmt(name + "Hdr", 16, "AstcHdr", width, height)

# The formats that every device that renders to them also filters linearly (prGpuFormatFilters).
FILTERS = {"R8", "R8Snorm", "R8Srgb", "Rg8", "Rg8Snorm", "Rg8Srgb", "Rgba8", "Rgba8Snorm", "Rgba8Srgb", "Bgra8",
           "Bgra8Srgb", "R16F", "Rg16F", "Rgba16F", "B5G6R5", "Bgr5A1", "Bgra4", "Rgb10A2", "Bgr10A2", "Rg11B10F",
           "Rgb9E5F"}


def check():
    """Fills in each format's Srgb form, and checks the list against itself and against pr/gpu.h's enumeration."""
    names = [f["name"] for f in FORMATS]
    assert len(names) == len(set(names)) and len(names) <= 256
    by_name = {f["name"]: f for f in FORMATS}
    for f in FORMATS:
        if f["plain"]:
            by_name[f["plain"]]["srgb"] = f["name"]
        assert f["stored"] is None or f["stored"] in by_name, f["name"]
    assert FILTERS <= set(names)
    with open(HEADER, encoding="utf-8") as h:
        text = h.read()
    start = text.index("enum {\n    PrGpuFormat_None,")
    end = text.index("PrGpuFormat_Count,", start)
    declared = re.findall(r"^\s*PrGpuFormat_(\w+),", text[start:end], re.M)
    if declared != names:
        first = next(i for i, (a, b) in enumerate(zip(declared + [""], names + [""])) if a != b)
        raise SystemExit("gen_gpu: pr/gpu.h's PrGpuFormat differs from the list at %d: %s and %s" % (
            first, declared[first] if first < len(declared) else "its end",
            names[first] if first < len(names) else "the list's end"))


def record(f):
    other = lambda name: "PrGpuFormat_" + (name or "None")
    flags = [flag for flag, on in (("kGpuFormatDepthStencil", f["depth"]), ("kGpuFormatFilters", f["name"] in FILTERS))
             if on]
    return "    {PrGpuFormat_%s, {%d, %d, %d}, PrGpuFormatFamily_%s, %s, %s, %s, PrGpuSwizzle_%s, %s}," % (
        f["name"], f["width"], f["height"], f["bytes"], f["family"], other(f["srgb"]), other(f["plain"]),
        other(f["stored"]), f["swizzle"], " | ".join(flags) or "0")


def main():
    check()
    body = ["""
// What a format is, apart from what a device does with it.
constexpr uint8_t kGpuFormatDepthStencil = 1u << 0;    // holds depth, stencil or both
constexpr uint8_t kGpuFormatFilters = 1u << 1;         // every device that renders to it filters it linearly

// A format's properties.
struct GpuFormatInfo {
    PrGpuFormat format;         // the record's own format, which is its index
    PrGpuFormatBlock block;
    PrGpuFormatFamily family;
    PrGpuFormat srgb;           // its Srgb form, or none if it has none
    PrGpuFormat plain;          // the plain form of a Srgb format, or none for any other
    PrGpuFormat stored;         // the format whose texels a swizzled format samples, or none for any other
    PrGpuSwizzle swizzle;
    uint8_t flags;
};

// Each format's properties, by PrGpuFormat.
constexpr GpuFormatInfo kGpuFormats[PrGpuFormat_Count] = {
""" + "\n".join(record(f) for f in FORMATS) + """
};

// Returns true if every record of kGpuFormats is at the index of its format.
constexpr bool gpuFormatsInOrder() {
    for (uint32_t i = 0; i < PrGpuFormat_Count; ++i) {
        if (kGpuFormats[i].format != i)
            return false;
    }
    return true;
}
static_assert(gpuFormatsInOrder(), "kGpuFormats lists the formats in the order of PrGpuFormat");
"""]
    header = namespaced([banner(__file__), "\n// The properties of lib/gpu's formats, by PrGpuFormat.\n",
                         '\n#pragma once\n\n#include "pr/base.h"\n#include "pr/gpu.h"\n'], body)
    write(OUT, header)


if __name__ == "__main__":
    main()
