#!/usr/bin/env python3
"""Builds the gif test vectors: the encoded bytes and the rgba8 screens a
decode of each frame must reproduce.

Run with no argument to print the C++ table gif_tests.cpp holds, and with
--corpus <dir> to write one seed file per vector for the image fuzz harness.
"""

import argparse
import os
import sys

TRANSPARENT = (0, 0, 0, 0)

GREEN = (0, 0xFF, 0)
RED = (0xFF, 0, 0)
BLUE = (0, 0, 0xFF)
BLACK = (0, 0, 0)
WHITE = (0xFF, 0xFF, 0xFF)


def lzw(indices, min_code_size):
    """The lzw code stream of one frame, as gif89a appendix F codes it."""
    clear = 1 << min_code_size
    end = clear + 1
    table = {(i,): i for i in range(clear)}
    next_code = clear + 2
    width = min_code_size + 1
    out = bytearray()
    bits = 0
    count = 0

    def emit(code):
        nonlocal bits, count
        bits |= code << count
        count += width
        while count >= 8:
            out.append(bits & 0xFF)
            bits >>= 8
            count -= 8

    emit(clear)
    string = ()
    for value in indices:
        candidate = string + (value,)
        if candidate in table:
            string = candidate
            continue
        emit(table[string])
        if next_code < 4096:
            table[candidate] = next_code
            next_code += 1
            if next_code == (1 << width) + 1 and width < 12:
                width += 1
        string = (value,)
    if string:
        emit(table[string])
    emit(end)
    if count:
        out.append(bits & 0xFF)
    return bytes(out)


def sub_blocks(data):
    out = bytearray()
    for start in range(0, len(data), 255):
        chunk = data[start:start + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def table_size(count):
    """The exponent of the power-of-two colour table holding `count` entries."""
    size = 0
    while (2 << size) < count:
        size += 1
    return size


def palette_bytes(colors, count):
    out = bytearray()
    for index in range(count):
        out += bytes(colors[index] if index < len(colors) else BLACK)
    return bytes(out)


def interlace(rows):
    """The rows in the order an interlaced frame stores them (gif89a 20)."""
    order = []
    for origin, step in ((0, 8), (4, 8), (2, 4), (1, 2)):
        order += list(range(origin, len(rows), step))
    return [rows[row] for row in order]


class Frame:
    def __init__(self, indices, width, height, left=0, top=0, palette=None,
                 delay=0, disposal=0, transparent=None, interlaced=False,
                 min_code_size=2):
        self.indices = indices
        self.width = width
        self.height = height
        self.left = left
        self.top = top
        self.palette = palette
        self.delay = delay
        self.disposal = disposal
        self.transparent = transparent
        self.interlaced = interlaced
        self.min_code_size = min_code_size

    def encode(self, version):
        out = bytearray()
        if version == b'89a':
            packed = (self.disposal << 2) | (1 if self.transparent is not None else 0)
            out += bytes([0x21, 0xF9, 4, packed])
            out += self.delay.to_bytes(2, 'little')
            out += bytes([self.transparent or 0, 0])
        descriptor = (1 if self.interlaced else 0) << 6
        if self.palette is not None:
            descriptor |= 0x80 | table_size(len(self.palette))
        out += bytes([0x2C])
        out += self.left.to_bytes(2, 'little') + self.top.to_bytes(2, 'little')
        out += self.width.to_bytes(2, 'little') + self.height.to_bytes(2, 'little')
        out += bytes([descriptor])
        if self.palette is not None:
            out += palette_bytes(self.palette, 2 << table_size(len(self.palette)))
        rows = [self.indices[row * self.width:(row + 1) * self.width]
                for row in range(self.height)]
        if self.interlaced:
            rows = interlace(rows)
        stored = [value for row in rows for value in row]
        out += bytes([self.min_code_size])
        out += sub_blocks(lzw(stored, self.min_code_size))
        return bytes(out)


def encode(width, height, frames, palette, version=b'89a', loop=False):
    out = bytearray(b'GIF' + version)
    out += width.to_bytes(2, 'little') + height.to_bytes(2, 'little')
    out += bytes([0x80 | 0x70 | table_size(len(palette)), 0, 0])
    out += palette_bytes(palette, 2 << table_size(len(palette)))
    if loop:
        out += bytes([0x21, 0xFF, 11]) + b'NETSCAPE2.0' + bytes([3, 1, 0, 0, 0])
    for frame in frames:
        out += frame.encode(version)
    out += bytes([0x3B])
    return bytes(out)


def compose(width, height, frames, palette):
    """The rgba8 screen a presenter shows at each frame, and the delays."""
    screen = [TRANSPARENT] * (width * height)
    shown = []
    delays = []
    for frame in frames:
        table = frame.palette if frame.palette is not None else palette
        saved = list(screen)
        for row in range(frame.height):
            for column in range(frame.width):
                value = frame.indices[row * frame.width + column]
                y = frame.top + row
                x = frame.left + column
                if y >= height or x >= width or value == frame.transparent:
                    continue
                screen[y * width + x] = tuple(table[value]) + (255,)
        shown.append(list(screen))
        delays.append(100 if frame.delay <= 1 else frame.delay * 10)
        if frame.disposal == 2:
            for row in range(frame.height):
                for column in range(frame.width):
                    y = frame.top + row
                    x = frame.left + column
                    if y < height and x < width:
                        screen[y * width + x] = TRANSPARENT
        elif frame.disposal == 3:
            screen = saved
    return shown, delays


def checkerboard(width, height):
    return [(row + column) % 2 for row in range(height) for column in range(width)]


def vectors():
    """One case per gif feature: the screen it composes on and its frames."""
    palette = [BLACK, WHITE, RED, GREEN]
    wide = [BLACK, WHITE, RED, GREEN, BLUE, (1, 2, 3), (4, 5, 6), (7, 8, 9)]
    return [
        ('87a global table', 6, 4, palette, b'87a', False,
         [Frame(checkerboard(6, 4), 6, 4)]),
        ('interlaced rows', 6, 8, palette, b'89a', False,
         [Frame([row % 4 for row in range(8) for _ in range(6)], 6, 8, interlaced=True)]),
        ('transparent index', 4, 2, palette, b'89a', False,
         [Frame([0, 1, 2, 3, 3, 2, 1, 0], 4, 2, transparent=1)]),
        ('two frames local tables', 4, 2, palette, b'89a', True,
         [Frame([0] * 8, 4, 2, palette=[GREEN], delay=1, disposal=1),
          Frame([0] * 8, 4, 2, palette=[RED], delay=1000, disposal=1)]),
        ('restore to background', 4, 4, palette, b'89a', False,
         [Frame([2] * 16, 4, 4, delay=5, disposal=1),
          Frame([1] * 4, 2, 2, left=1, top=1, delay=5, disposal=2),
          Frame([3] * 4, 2, 2, left=0, top=0, delay=5, disposal=1)]),
        ('restore to previous', 4, 4, palette, b'89a', False,
         [Frame([3] * 16, 4, 4, delay=5, disposal=1),
          Frame([1] * 4, 2, 2, left=0, top=0, delay=5, disposal=3),
          Frame([2] * 4, 2, 2, left=2, top=2, delay=5, disposal=1)]),
        ('eight bit codes', 4, 4, wide, b'89a', False,
         [Frame([value % 8 for value in range(16)], 4, 4, min_code_size=8)]),
    ]


def hexlist(values, indent):
    """The values as C++ hex bytes, wrapped at the column clang-format keeps."""
    out = []
    line = ' ' * indent
    for value in values:
        item = '0x%02X,' % value
        if len(line) + len(item) > 99:
            out.append(line.rstrip())
            line = ' ' * indent
        line += item + ' '
    out.append(line.rstrip().rstrip(','))
    return '\n'.join(out).lstrip()


def cpp(name, width, height, data, screens, delays):
    lines = ['        {"%s",' % name]
    lines.append('         {%s},' % hexlist(data, 10))
    lines.append('         %d,' % width)
    lines.append('         %d,' % height)
    lines.append('         {%s},' % ', '.join(str(delay) for delay in delays))
    lines.append('         {')
    for index, screen in enumerate(screens):
        flat = [channel for pixel in screen for channel in pixel]
        comma = ',' if index + 1 < len(screens) else ''
        lines.append('             {%s}%s' % (hexlist(flat, 14), comma))
    lines.append('         }},')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--corpus')
    args = parser.parse_args()

    for name, width, height, palette, version, loop, frames in vectors():
        data = encode(width, height, frames, palette, version=version, loop=loop)
        if args.corpus:
            os.makedirs(args.corpus, exist_ok=True)
            seed = 'image-gif-' + name.replace(' ', '-')
            with open(os.path.join(args.corpus, seed), 'wb') as handle:
                handle.write(data)
            continue
        screens, delays = compose(width, height, frames, palette)
        sys.stdout.write(cpp(name, width, height, data, screens, delays) + '\n')


if __name__ == '__main__':
    main()
