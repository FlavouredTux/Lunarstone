#!/usr/bin/env python3
"""Tests for tools/png_compare.py: that it reads the PNGs it claims to read
(there is no pillow here to read them with, so the reader is its own worst
case), that the numbers it prints are the numbers the two images hold, and that
the budgets decide the exit code.

    python3 -m unittest discover -s tests -p 'png_compare_tests.py'

The images are written by hand -- chunked, compressed and filtered here rather
than by an encoder, since a fixture written by the same code that reads it only
proves the two agree. Every filter, both interlaced and not, and all four
colour types go in, against pixels whose values are known before they are
encoded.
"""

import contextlib
import io
import pathlib
import struct
import sys
import tempfile
import unittest
import zlib

# tools/png_compare.py sits next to this file's directory, not on the path.
root = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "tools"))

import png_compare


# One chunk as a png file holds it: the length, the type, the body, and the crc
# of the type and the body together.
def chunk(kind, body):
    """A png chunk, crc and all."""
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


# The pixel rows an image is stored as, each one of `channels` bytes per pixel.
def rows_of(pixels, width, height, channels):
    """The rows of a flat block of rgba-ish bytes, as png stores them."""
    stride = width * channels
    return [pixels[row * stride:(row + 1) * stride] for row in range(height)]


def solid(width, height, channels, value):
    """A block of one repeated pixel, `channels` bytes of `value` in each."""
    return bytes([value]) * (width * height * channels)


def speckle(width, height, step):
    """A rgba image of known pixels that all differ, so a filter has something
    to work on and a pixel moved to the wrong place cannot hide."""
    return b"".join(bytes([index * step % 256, 255 - index % 256, index * 3 % 256, 255])
                    for index in range(width * height))


# The bytes of a row after the filter of that kind has been taken off it, which
# is the other half of what unfilter puts back.
def filtered(line, previous, channels, kind):
    """One row as a png stores it: the filter byte, then the row in terms of
    the row above and the pixel to the left."""
    out = bytearray()
    for index, value in enumerate(line):
        left = line[index - channels] if index >= channels else 0
        up = previous[index]
        upper_left = previous[index - channels] if index >= channels else 0
        if kind == 0:
            out.append(value)
        elif kind == 1:
            out.append((value - left) & 0xFF)
        elif kind == 2:
            out.append((value - up) & 0xFF)
        elif kind == 3:
            out.append((value - (left + up) // 2) & 0xFF)
        else:
            out.append((value - png_compare.paeth(left, up, upper_left)) & 0xFF)
    return bytes([kind]) + bytes(out)


# An interlaced image is stored as seven passes, and this walks the same table
# the reader does, so a pass of the wrong size shows up as a wrong image rather
# than as an exception.
def passes_of(rows, width, height, channels):
    """The rows of an image in the order Adam7 stores them, one block per pass
    that this size has."""
    passes = []
    for start_x, start_y, step_x, step_y in png_compare.ADAM7:
        ys = range(start_y, height, step_y)
        xs = range(start_x, width, step_x)
        if not len(ys) or not len(xs):
            continue
        passes.append([b"".join(rows[y][x * channels:(x + 1) * channels] for x in xs) for y in ys])
    return passes


def png(rows, width, height, colour=6, filters=None, interlace=0, palette=b"", alpha=b""):
    """A whole png file: the rows, at the given colour type, filtered with
    `filters` (None for no filter on any row) and interlaced or not."""
    channels = png_compare.COLOURS[colour][1]
    blocks = passes_of(rows, width, height, channels) if interlace else [rows]
    raw = bytearray()
    for block in blocks:
        previous = bytes(len(block[0]))
        for index, line in enumerate(block):
            kind = filters[index % len(filters)] if filters else 0
            raw += filtered(line, previous, channels, kind)
            previous = line
    body = b"\x89PNG\r\n\x1a\n"
    body += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, colour, 0, 0, interlace))
    if palette:
        body += chunk(b"PLTE", palette)
    if alpha:
        body += chunk(b"tRNS", alpha)
    body += chunk(b"IDAT", zlib.compress(bytes(raw)))
    return body + chunk(b"IEND", b"")


class ReadTest(unittest.TestCase):
    """The reader, against files written here with the pixel values known."""

    def read(self, data):
        """The image in a file holding `data`; returns the header and the rgba
        bytes the reader gave back."""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "image.png"
            path.write_bytes(data)
            return png_compare.read_png(str(path))

    def image(self, width, height, pixels, colour, **kwargs):
        """A png of `pixels`, laid out as the reader will find them."""
        channels = png_compare.COLOURS[colour][1]
        return png(rows_of(pixels, width, height, channels), width, height, colour, **kwargs)

    def test_rgba_reads_back_exactly(self):
        """A four-channel image of known pixels comes back as those pixels,
        with nothing dropped and nothing widened."""
        pixels = speckle(4, 3, 7)
        header, rgba = self.read(self.image(4, 3, pixels, 6))
        self.assertEqual((header["width"], header["height"]), (4, 3))
        self.assertEqual(header["name"], "8-bit rgba")
        self.assertEqual(rgba, bytes(pixels))

    def test_rgb_gains_an_opaque_alpha(self):
        """Three channels are compared as four, so an rgb capture and an rgba
        capture of the same picture come back the same."""
        pixels = b"".join(bytes([index * 11, 20, 30]) for index in range(3 * 2))
        header, rgba = self.read(self.image(3, 2, pixels, 2))
        self.assertEqual(header["channels"], 3)
        self.assertEqual(rgba, b"".join(bytes([index * 11, 20, 30, 255]) for index in range(3 * 2)))

    def test_grayscale_is_widened_to_grey_rgb(self):
        """A gray value is a colour with three identical channels and an opaque
        alpha, which is what makes a depth or normal dump comparable to
        anything else."""
        pixels = bytes([0, 40, 80, 120, 160, 200])
        _, rgba = self.read(self.image(3, 2, pixels, 0))
        self.assertEqual(rgba, b"".join(bytes([value] * 3) + b"\xff" for value in pixels))

    def test_palette_is_expanded_to_colour(self):
        """A palette image reads as the colours its PLTE holds, opaque unless a
        tRNS says otherwise."""
        palette = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255])
        _, rgba = self.read(self.image(3, 1, bytes([0, 1, 2]), 3, palette=palette))
        self.assertEqual(rgba, bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255]))

        _, rgba = self.read(self.image(3, 1, bytes([0, 1, 2]), 3, palette=palette, alpha=bytes([0, 128])))
        self.assertEqual(rgba, bytes([255, 0, 0, 0, 0, 255, 0, 128, 0, 0, 255, 255]))

    def test_every_filter_decodes_to_the_same_image(self):
        """The five row filters are the same picture stored five ways: None,
        Sub, Up, Average and Paeth, one image in a single filter and one that
        cycles through all of them, over rows that move so that each filter has
        something to do."""
        pixels = speckle(5, 5, 13)
        for kind in [None, 0, 1, 2, 3, 4]:
            with self.subTest(filter="mixed" if kind is None else kind):
                filters = None if kind is None else [kind]
                _, rgba = self.read(self.image(5, 5, pixels, 6, filters=filters))
                self.assertEqual(rgba, pixels)
        _, rgba = self.read(self.image(5, 5, pixels, 6, filters=range(5)))
        self.assertEqual(rgba, pixels)

    def test_an_interlaced_image_reads_back_exactly(self):
        """Adam7's seven passes are put back in the right places, at sizes odd
        enough that several of the passes are not a whole number of pixels."""
        for width, height in ((7, 5), (8, 8), (1, 1), (16, 3)):
            with self.subTest(size=f"{width}x{height}"):
                pixels = speckle(width, height, 9)
                _, rgba = self.read(self.image(width, height, pixels, 6, interlace=1))
                self.assertEqual(rgba, pixels)

    def test_a_file_that_is_not_a_png(self):
        """A file with no signature, a truncated one and one whose chunk fails
        its crc are all usage errors rather than images read wrong."""
        good = self.image(2, 2, solid(2, 2, 4, 7), 6)
        cases = [
            (b"not a png at all, just some bytes", "signature"),
            (good[:-8], "truncated"),
            (good[:20] + b"\x00\x00\x00\x00" + good[24:], "crc"),
        ]
        for data, what in cases:
            with self.subTest(what=what):
                with self.assertRaises(png_compare.UsageError) as raised:
                    self.read(data)
                self.assertIn(what, str(raised.exception))

    def test_formats_the_reader_refuses(self):
        """A 16-bit file and a grayscale-with-alpha file are refused by name,
        rather than read as the 8 bits that happen to be at the front."""
        for depth, colour, what in ((16, 2, "bit depth 16"), (8, 4, "grayscale with alpha"),
                                    (8, 1, "deprecated")):
            with self.subTest(what=what):
                body = b"\x89PNG\r\n\x1a\n"
                body += chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, depth, colour, 0, 0, 0))
                body += chunk(b"IDAT", zlib.compress(b"\x00" * 6))
                body += chunk(b"IEND", b"")
                with self.assertRaises(png_compare.UsageError) as raised:
                    self.read(body)
                self.assertIn(what, str(raised.exception))

    def test_a_missing_file(self):
        """A path that is not there is a usage error, not a traceback."""
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(png_compare.UsageError):
                png_compare.read_png(str(pathlib.Path(directory) / "absent.png"))


class CompareTest(unittest.TestCase):
    """Two images written to a temporary directory and run through the tool's
    own main(), so the exit code and the output are the real ones."""

    def compare(self, before, after, *args):
        """Runs the tool on two images; returns (exit code, output)."""
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for name, data in (("before.png", before), ("after.png", after)):
                path = pathlib.Path(directory) / name
                path.write_bytes(data)
                paths.append(str(path))
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                status = png_compare.main([*paths, *args])
        return status, out.getvalue()

    @staticmethod
    def image(value=10, width=10, height=10, **kwargs):
        """A flat rgba image of one repeated opaque pixel."""
        pixels = bytes([value, value, value, 255]) * (width * height)
        return png(rows_of(pixels, width, height, 4), width, height, 6, **kwargs)

    @staticmethod
    def moved(value=13, at=(0, 0), width=10, height=10):
        """A flat rgba image of one repeated opaque pixel with a few pixels
        moved to another value."""
        pixels = bytearray(bytes([10, 10, 10, 255]) * (width * height))
        for x, y in at:
            pixels[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes([value, 10, 10, 255])
        return png(rows_of(bytes(pixels), width, height, 4), width, height, 6)

    def test_identical_images_change_nothing(self):
        """The same image twice is zero of everything, exits 0, and says where
        the biggest difference is by not having one."""
        status, out = self.compare(self.image(), self.image())
        self.assertEqual(status, 0)
        self.assertIn("100 pixels", out)
        self.assertIn("pixels over the threshold of 1/255: 0 of 100 (0.00%)", out)
        self.assertIn("largest per-channel difference: 0/255 at -", out)
        self.assertIn("within every budget", out)

    def test_a_known_difference_is_reported_and_exits_one(self):
        """Two of a hundred pixels 3/255 off is a 3, a 2 and a 2.00%, at the
        pixel the first of them moved."""
        status, out = self.compare(self.image(), self.moved(at=((4, 5), (9, 9))))
        self.assertEqual(status, 1)
        self.assertIn("pixels over the threshold of 1/255: 2 of 100 (2.00%)", out)
        self.assertIn("largest per-channel difference: 3/255 at (4,5)", out)
        self.assertIn("over budget", out)

    def test_the_threshold_decides_what_counts_as_changed(self):
        """A one-LSB difference is noise under the default threshold and a
        change under a threshold of zero, and it is in the largest difference
        either way -- which is the difference between two builds and a real
        change."""
        after = self.moved(value=11, at=((0, 0),))
        status, out = self.compare(self.image(), after)
        self.assertEqual(status, 0)
        self.assertIn("pixels over the threshold of 1/255: 0 of 100 (0.00%)", out)
        self.assertIn("largest per-channel difference: 1/255 at (0,0)", out)

        status, out = self.compare(self.image(), after, "--threshold", "0")
        self.assertEqual(status, 1)
        self.assertIn("pixels over the threshold of 0/255: 1 of 100 (1.00%)", out)

    def test_the_difference_budgets_decide_the_exit_code(self):
        """Out of the box nothing may change, so a change of any size is over a
        budget; from there each budget decides on its own, and landing on one
        still fits, since a budget is what is allowed."""
        before, after = self.image(), self.moved(at=((4, 5), (9, 9)))
        self.assertEqual(self.compare(before, after)[0], 1)

        # The difference budget, with the pixel one taken off the table: 3 fits
        # in 3 and not in 2.
        self.assertEqual(self.compare(before, after, "--max-diff", "2", "--max-percent", "100")[0], 1)
        self.assertEqual(self.compare(before, after, "--max-diff", "3", "--max-percent", "100")[0], 0)

        # The share budget: 2.00% of 100 pixels is over 1.99 and inside 2.
        self.assertEqual(self.compare(before, after, "--max-percent", "1.99")[0], 1)
        self.assertEqual(self.compare(before, after, "--max-percent", "2")[0], 0)

        # The count budget, which is the same budget said at this resolution.
        self.assertEqual(self.compare(before, after, "--max-pixels", "1")[0], 1)
        self.assertEqual(self.compare(before, after, "--max-pixels", "2")[0], 0)
        self.assertEqual(self.compare(before, after, "--max-pixels", "0")[0], 1)

    def test_the_budgets_are_printed(self):
        """The table holds what each budget is measured against, and the run
        says which ones it went over."""
        before, after = self.image(), self.moved(at=((0, 0),))
        status, out = self.compare(before, after, "--max-diff", "2", "--max-percent", "0.5")
        self.assertEqual(status, 1)
        row = [line for line in out.splitlines() if line.startswith("  largest difference")]
        self.assertEqual(len(row), 1)
        self.assertIn("3/255", row[0])
        self.assertIn("at most 2/255", row[0])
        self.assertTrue(row[0].endswith("over"))
        self.assertIn("share of pixels over the threshold", out)
        self.assertIn("over budget", out)
        self.assertIn("largest difference: 3/255 against 2/255", out)
        # A count of pixels is the same budget as a share, so the two are never
        # both in the table.
        status, out = self.compare(before, after, "--max-pixels", "5")
        self.assertEqual(status, 0)
        self.assertIn("count of pixels over the threshold", out)
        self.assertNotIn("share of pixels over the threshold", out)

    def test_an_alpha_difference_is_a_difference(self):
        """Alpha is a channel like any other, so a capture whose alpha moved is
        not a capture whose colour moved quietly."""
        pixels = bytearray(bytes([10, 10, 10, 255]) * 2)
        pixels[0:4] = bytes([10, 10, 10, 200])
        after = png(rows_of(bytes(pixels), 2, 1, 4), 2, 1, 6)
        before = self.image(width=2, height=1)
        status, out = self.compare(before, after, "--max-percent", "100")
        self.assertEqual(status, 0)
        self.assertIn("largest per-channel difference: 55/255 at (0,0)", out)
        self.assertIn("pixels over the threshold of 1/255: 1 of 2 (50.00%)", out)
        self.assertEqual(self.compare(before, after, "--max-diff", "10")[0], 1)

    def test_images_of_different_sizes_exit_two(self):
        """Two different sizes have no pixel in common to measure, which is a
        usage error rather than a difference."""
        status, out = self.compare(self.image(width=4, height=4),
                                   self.image(width=4, height=5))
        self.assertEqual(status, 2)
        self.assertIn("different sizes", out)
        self.assertIn("4x4", out)
        self.assertIn("4x5", out)

    def test_a_bad_file_exits_two(self):
        """A missing file and a file that is not a png are both usage errors,
        not regressions."""
        with tempfile.TemporaryDirectory() as directory:
            absent = str(pathlib.Path(directory) / "absent.png")
            junk = pathlib.Path(directory) / "junk.png"
            junk.write_bytes(b"this is not a png")
            for path in (absent, str(junk)):
                with self.subTest(path=path):
                    out = io.StringIO()
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                        status = png_compare.main([path, str(junk)])
                    self.assertEqual(status, 2)
                    self.assertIn("png_compare.py:", out.getvalue())

    def test_the_flags_are_checked(self):
        """A budget that could not be met by anything, or a percentage over a
        hundred, is said at the command line rather than compared against."""
        for args, what in ((["--max-diff", "256"], "0-255"),
                           (["--max-diff", "-1"], "0-255"),
                           (["--threshold", "-1"], "0 or more"),
                           (["--max-percent", "150"], "0 to 100"),
                           (["--max-pixels", "-3"], "0 or more")):
            with self.subTest(args=args):
                err = io.StringIO()
                with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as raised:
                    png_compare.parse_args(["before.png", "after.png", *args])
                self.assertEqual(raised.exception.code, 2)
                self.assertIn(what, err.getvalue())


if __name__ == "__main__":
    unittest.main()