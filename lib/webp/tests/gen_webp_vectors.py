#!/usr/bin/env python3
"""Builds the webp test vectors: small files of each feature, which libwebp's cwebp, img2webp and webpmux write, and
the Rgba8 pixels of each frame as libwebp decodes them, through Pillow. Prints the C++ table that lib/webp/tests/webp_vectors.h holds.

The tools must be libwebp 1.6.0's, the version that cmake/dependencies.cmake pins for the comparison with libwebp.
"""

import io
import os
import subprocess
import sys
import tempfile

from PIL import Image, features

VERSION = "1.6.0"


def picture(width, height, alpha, seed):
    """A small picture of gradients and edges, with alpha that ranges from transparent to opaque if `alpha`."""
    image = Image.new("RGBA", (width, height))
    state = seed * 2654435761 % (1 << 32) or 1
    for y in range(height):
        for x in range(width):
            state = (state * 1103515245 + 12345) % (1 << 31)
            noise = state % 17 - 8
            r = max(0, min(255, x * 255 // max(1, width - 1) + noise))
            g = max(0, min(255, y * 255 // max(1, height - 1) - noise))
            b = 220 if (x + y) % 5 == 0 else (x * y * 9) % 256
            a = 255 if not alpha else (0 if (x + 2 * y) % 7 == 0 else min(255, (x + y) * 40))
            image.putpixel((x, y), (r, g, b, a))
    return image


def palette(width, height, colors):
    entries = [(255, 0, 0, 255), (0, 128, 255, 255), (20, 200, 40, 128), (250, 250, 250, 255), (0, 0, 0, 0)]
    image = Image.new("RGBA", (width, height))
    for y in range(height):
        for x in range(width):
            image.putpixel((x, y), entries[(x // 2 + y) % colors])
    return image


def run(*command):
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def still(directory, name, image, options):
    source = os.path.join(directory, name + ".png")
    out = os.path.join(directory, name + ".webp")
    image.save(source)
    run("cwebp", "-quiet", *options, source, "-o", out)
    with open(out, "rb") as f:
        return f.read()


def animation(directory, name, frames, options):
    command = ["img2webp", "-loop", "2", *options]
    for i, (image, duration, frame_options) in enumerate(frames):
        source = os.path.join(directory, "%s_%d.png" % (name, i))
        image.save(source)
        command += ["-d", str(duration), *frame_options, source]
    out = os.path.join(directory, name + ".webp")
    run(*command, "-o", out)
    with open(out, "rb") as f:
        return f.read()


def with_chunks(directory, name, data, chunks):
    path = os.path.join(directory, name + "_plain.webp")
    with open(path, "wb") as f:
        f.write(data)
    for kind, payload in chunks:
        payload_path = os.path.join(directory, name + "_" + kind)
        with open(payload_path, "wb") as f:
            f.write(payload)
        out = os.path.join(directory, name + "_" + kind + ".webp")
        run("webpmux", "-set", kind, payload_path, path, "-o", out)
        path = out
    with open(path, "rb") as f:
        return f.read()


def decode(data):
    """Each frame's Rgba8 pixels and duration, as libwebp decodes them."""
    image = Image.open(io.BytesIO(data))
    frames = []
    for index in range(getattr(image, "n_frames", 1)):
        image.seek(index)
        frames.append((image.convert("RGBA").tobytes(), image.info.get("duration", 0) if index or
                       getattr(image, "n_frames", 1) > 1 else 0))
    return image.size, frames


def vectors(directory):
    gradient = picture(9, 7, False, 1)
    alpha = picture(6, 5, True, 2)
    yield "lossless gradient", still(directory, "lossless", picture(5, 4, True, 3), ["-lossless", "-z", "9", "-exact"])
    yield "lossless palette", still(directory, "palette", palette(7, 5, 3), ["-lossless", "-exact"])
    yield "lossless many colors", still(directory, "colors", picture(12, 3, False, 4), ["-lossless", "-m", "0"])
    yield "lossy normal filter", still(directory, "lossy", gradient, ["-q", "50", "-f", "80", "-strong"])
    yield "lossy simple filter", still(directory, "simple", gradient,
                                      ["-q", "30", "-f", "60", "-nostrong", "-sharpness", "4", "-segments", "1"])
    yield "lossy alpha compressed", still(directory, "alpha", alpha, ["-q", "70", "-alpha_filter", "best"])
    yield "lossy alpha raw", still(directory, "raw", alpha, ["-q", "70", "-alpha_method", "0", "-alpha_filter",
                                                             "none"])
    frames = [(picture(4, 4, True, 5), 80, ["-lossless"]), (picture(4, 4, False, 6), 120, ["-lossy", "-q", "60"]),
              (picture(4, 4, True, 7), 40, ["-lossless"])]
    yield "animation", animation(directory, "animation", frames, ["-mixed", "-kmin", "1", "-kmax", "2"])
    plain = still(directory, "extended", picture(3, 2, False, 8), ["-lossless"])
    exif = bytes([0x49, 0x49, 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0])
    yield "extended metadata", with_chunks(directory, "extended", plain,
                                           [("icc", b"not an ICC profile"), ("exif", exif), ("xmp", b"<x/>")])


def array(name, data):
    rows = []
    for at in range(0, len(data), 15):
        rows.append("    " + ", ".join("0x%02X" % b for b in data[at:at + 15]) + ",")
    return "constexpr uint8_t %s[] = {\n%s\n};\n" % (name, "\n".join(rows))


def identifier(name):
    return "k" + "".join(word.capitalize() for word in name.split())


def main():
    if features.version("webp") != VERSION:
        sys.exit("Pillow's libwebp is %s, not %s" % (features.version("webp"), VERSION))
    out = []
    with tempfile.TemporaryDirectory() as directory:
        for name, data in vectors(directory):
            size, frames = decode(data)
            base = identifier(name)
            out.append(array(base + "Bytes", data))
            out.append("constexpr uint32_t %sDurations[] = {%s};\n" % (base, ", ".join(str(d) for _, d in frames)))
            for index, (pixels, _) in enumerate(frames):
                out.append(array("%sFrame%d" % (base, index), pixels))
            out.append("\n")
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
