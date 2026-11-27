#!/usr/bin/env python3
"""Compares two PNG images and says how far apart they are: the biggest
per-channel difference, how many pixels differ at all, and where the biggest
difference is.

    tools/png_compare.py <before.png> <after.png> [--threshold 1] [--max-diff 255]
                         [--max-percent 0] [--max-pixels N]

Two screenshots of the same frame taken from two builds differ in the last bit
here and there whatever the code did, because a shader compiled twice is not
the same arithmetic twice. That is what --threshold is for: a pixel counts as
changed only when one of its channels differs by more than the threshold, so
the default of 1 calls a one-LSB wobble the same colour and calls anything
bigger a change. A caller that would rather count the noise can move it with
--threshold 0, which makes every pixel that is not bit-identical a change.

Everything else is reported rather than judged: the largest per-channel
difference over every pixel and every channel (0-255), where that difference
sits, and how many pixels are over the threshold, out of the total and as a
percentage. The judgement is the caller's, and it is three budgets: --max-diff
on the largest difference, --max-percent or --max-pixels on how many pixels may
move. "At most 2/255 per channel and well under 1% of pixels" is `--max-diff 2
--max-percent 1`, and the exit code says whether it held.

Out of the box only one thing is allowed, which is nothing changing at all:
--max-percent defaults to 0, so a single pixel over the threshold is over the
budget and the tool is a plain "did this change?" check. --max-diff defaults to
255, which is the whole range of an 8-bit channel and so no two channels can
reach it -- that budget is off until it is lowered. --max-pixels is the same
budget said as a count rather than a share, and takes --max-percent's place
when it is given: a run that budgets a number of pixels is not judged twice, on
a count and on the percentage of it that count happens to be.

The two images have to be the same size: a pair of different sizes has no pixel
they share to measure, so that is a usage error and not a difference.

The PNGs are decoded here, out of zlib and struct, so the tool runs wherever a
python3 does -- there is no pillow and no numpy to lean on, and a comparison
that quietly needs one is a comparison that does not run on the build machine.
What that reader takes is the 8-bit PNGs a screenshot comes in as: grayscale,
rgb, palette and rgba, the palette with or without a tRNS, all of them with or
without Adam7 interlacing (the seven passes are put back together in the same
order the rest of this reads them). Every one of them is expanded to rgba and
compared channel by channel, alpha included, so a capture whose alpha moved is
a difference like any other rather than something to notice afterwards. What it
does not take is a bit depth of 16 -- refused by name, rather than read as the
eight bits that happen to sit at the front of the sample -- or the
grayscale-with-alpha colour type, which nothing here writes.

Decoding is plain python over every byte of every scanline, which is slow next
to a native decoder: a 1080p screenshot takes a few seconds to read. That is
the price of a tool with no dependencies, paid once per pair, and it is only
the wrong trade for a thousand pairs.

Exits 0 when nothing went over its budget, 1 when something did, 2 on a usage or
parse error -- a missing file, a file that is not a PNG, a bit depth or colour
type the reader does not handle, a chunk that fails its length or its crc, or
two images of different sizes.
"""

import argparse
import pathlib
import struct
import sys
import zlib

# A channel difference above which a pixel counts as changed: one LSB of an
# 8-bit channel is the wobble a shader gives away for free, so it is noise by
# default and anything past it is the comparison's business.
DEFAULT_THRESHOLD = 1
# The largest difference two 8-bit channels can have, so this is the whole
# range of a channel and a budget set here is one nothing can ever reach.
FULL_SCALE = 255

# The png signature, and the colour types the reader takes, each with the word
# for it and the channels a pixel of it holds before it is widened to rgba.
SIGNATURE = b"\x89PNG\r\n\x1a\n"
COLOURS = {
    0: ("8-bit grayscale", 1),
    2: ("8-bit rgb", 3),
    3: ("8-bit palette", 1),
    6: ("8-bit rgba", 4),
}

# The colour types that are not in COLOURS, named for the refusal message.
REFUSED = {
    1: "greyscale with a palette, deprecated",
    4: "grayscale with alpha",
}

# Adam7: the passes in the order they are stored in, and the pixel each one
# starts at and the step it takes across and down.
ADAM7 = (
    (0, 0, 8, 8),
    (4, 0, 8, 8),
    (0, 4, 4, 8),
    (2, 0, 4, 4),
    (0, 2, 2, 4),
    (1, 0, 2, 2),
    (0, 1, 1, 2),
)


class UsageError(Exception):
    """A missing or unreadable file, or one this reader cannot read."""


def paeth(left, up, upper_left):
    """The Paeth predictor: whichever of the pixel to the left, the pixel above
    and the one above that a byte is closest to, which is how the last of the
    filters says "this byte is nearly the same as something nearby"."""
    guess = left + up - upper_left
    near_left = abs(guess - left)
    near_up = abs(guess - up)
    near_corner = abs(guess - upper_left)
    if near_left <= near_up and near_left <= near_corner:
        return left
    return up if near_up <= near_corner else upper_left


def chunks(data, path):
    """Every chunk of the png in `data`, in order, as (type, body): the
    signature, the chunk lengths and the chunk CRCs all checked, since a file
    that had been truncated or mangled and decoded anyway would be worse than
    one that says so."""
    if not data.startswith(SIGNATURE):
        raise UsageError(f"{path}: not a png: the signature is wrong")
    at = len(SIGNATURE)
    while at < len(data):
        if len(data) - at < 12:
            raise UsageError(f"{path}: truncated: a chunk header at byte {at} is cut short")
        (length,) = struct.unpack(">I", data[at:at + 4])
        kind = data[at + 4:at + 8].decode("latin-1")
        body = data[at + 8:at + 8 + length]
        if len(body) != length:
            raise UsageError(f"{path}: truncated: chunk {kind} claims {length} bytes, "
                             f"{len(body)} are here")
        (crc,) = struct.unpack(">I", data[at + 8 + length:at + 12 + length])
        if zlib.crc32(data[at + 4:at + 8 + length]) != crc:
            raise UsageError(f"{path}: chunk {kind} fails its crc")
        yield kind, body
        at += 12 + length


def header_of(body, path):
    """The IHDR chunk as the fields the reader needs, refusing the bit depths
    and colour types it would otherwise read wrong: a 16-bit sample has eight
    bits in front of the byte this compares, and grayscale-with-alpha is one
    more colour type than COLOURS has a name for."""
    if len(body) != 13:
        raise UsageError(f"{path}: IHDR is {len(body)} bytes, 13 are needed")
    width, height, depth, colour, compression, filtering, interlace = struct.unpack(">IIBBBBB", body)
    if not width or not height:
        raise UsageError(f"{path}: a {width}x{height} image has no pixels to compare")
    if depth != 8:
        raise UsageError(f"{path}: bit depth {depth}, only 8 is read")
    if colour not in COLOURS:
        raise UsageError(f"{path}: colour type {colour} ({REFUSED.get(colour, 'unknown')}) is not read")
    if compression != 0 or filtering != 0:
        raise UsageError(f"{path}: unknown compression {compression} or filter method {filtering}")
    if interlace > 1:
        raise UsageError(f"{path}: unknown interlace method {interlace}")
    return {"width": width, "height": height, "depth": depth, "colour": colour,
            "name": COLOURS[colour][0], "channels": COLOURS[colour][1],
            "interlaced": bool(interlace)}


def unfilter(raw, width, height, channels):
    """The scanlines back out of what zlib gave us. A png stores each row as a
    filter byte and then as bytes that only mean something next to the row above
    and the pixel to the left, which is what this puts back: None (the bytes as
    they are), Sub (the pixel to the left), Up (the row above), Average (the
    two of them, halved and rounded down) and Paeth (whichever of them
    paeth() picks)."""
    stride = width * channels
    if len(raw) < height * (stride + 1):
        raise UsageError(f"the image data is {len(raw)} bytes, {height * (stride + 1)} are needed")
    out, previous, at = bytearray(), bytearray(stride), 0
    for _ in range(height):
        kind = raw[at]
        if kind > 4:
            raise UsageError(f"unknown row filter {kind}")
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += stride + 1
        if kind:
            for index in range(stride):
                left = line[index - channels] if index >= channels else 0
                up = previous[index]
                upper_left = previous[index - channels] if index >= channels else 0
                if kind == 1:
                    line[index] = (line[index] + left) & 0xFF
                elif kind == 2:
                    line[index] = (line[index] + up) & 0xFF
                elif kind == 3:
                    line[index] = (line[index] + (left + up) // 2) & 0xFF
                else:
                    line[index] = (line[index] + paeth(left, up, upper_left)) & 0xFF
        out += line
        previous = line
    return bytes(out)


def adam7(raw, width, height, channels):
    """An interlaced image's seven passes put back into one row order: a pass is
    unfiltered on its own, since a pass has its own previous row, and its pixels
    are then scattered into the image where they belong. The passes that are
    empty at this size hold no data at all, which is why the bytes are read out
    of the image as the passes are walked rather than counted up front."""
    out = bytearray(width * height * channels)
    at = 0
    for start_x, start_y, step_x, step_y in ADAM7:
        columns = len(range(start_x, width, step_x))
        rows = len(range(start_y, height, step_y))
        if not columns or not rows:
            continue
        stride = columns * channels + 1
        pixels = unfilter(raw[at:at + rows * stride], columns, rows, channels)
        at += rows * stride
        for row, y in enumerate(range(start_y, height, step_y)):
            line = pixels[row * columns * channels:(row + 1) * columns * channels]
            for column, x in enumerate(range(start_x, width, step_x)):
                out[(y * width + x) * channels:(y * width + x + 1) * channels] = \
                    line[column * channels:(column + 1) * channels]
    return bytes(out)


def scanlines(header, raw):
    """The image a row at a time, as (row, width, channels, bytes): enough to
    widen a pixel without holding a second copy of the whole image for the sake
    of one row of it."""
    width, height, channels = header["width"], header["height"], header["channels"]
    if not header["interlaced"]:
        pixels = unfilter(raw, width, height, channels)
        for row in range(height):
            yield row, width, channels, pixels[row * width * channels:(row + 1) * width * channels]
        return
    whole = adam7(raw, width, height, channels)
    for row in range(height):
        yield row, width, channels, whole[row * width * channels:(row + 1) * width * channels]


def rgba(header, raw):
    """Every pixel of the image as four bytes, r g b a: a grayscale or palette
    pixel widened to colour, everything given the alpha it already had or an
    opaque one, so that the comparison is four bytes against four bytes with
    nothing left of the two files' colour types to think about."""
    out = bytearray(header["width"] * header["height"] * 4)
    palette, alpha = header["palette"], header["alpha"]
    at = 0
    for _, width, channels, line in scanlines(header, raw):
        for index in range(width):
            pixel = line[index * channels:(index + 1) * channels]
            if header["colour"] == 3:
                entry = pixel[0]
                if 3 * (entry + 1) > len(palette):
                    raise UsageError(f"a pixel uses palette entry {entry}, of {len(palette) // 3}")
                colour = palette[entry * 3:entry * 3 + 3]
                out[at:at + 4] = colour + (bytes([alpha[entry]]) if entry < len(alpha) else b"\xff")
            elif channels == 1:
                out[at:at + 4] = bytes([pixel[0]] * 3) + b"\xff"
            elif channels == 4:
                out[at:at + 4] = pixel
            else:
                out[at:at + 4] = pixel + b"\xff"
            at += 4
    return bytes(out)


def read_png(path):
    """The image in `path` as (its IHDR fields, its pixels as rgba bytes)."""
    try:
        data = pathlib.Path(path).read_bytes()
    except OSError as error:
        raise UsageError(f"{path}: {error.strerror or error}")

    header, palette, alpha, packed, ended = None, b"", b"", [], False
    for kind, body in chunks(data, path):
        if header is None:
            if kind != "IHDR":
                raise UsageError(f"{path}: not a png: it starts with {kind}, not IHDR")
            header = header_of(body, path)
        elif kind == "PLTE":
            palette = body
        elif kind == "tRNS":
            alpha = body
        elif kind == "IDAT":
            packed.append(body)
        elif kind == "IEND":
            ended = True
    if not packed:
        raise UsageError(f"{path}: no image data: there is no IDAT")
    if not ended:
        raise UsageError(f"{path}: truncated: there is no IEND")
    if header["colour"] == 3 and not palette:
        raise UsageError(f"{path}: a palette image with no PLTE")
    header.update({"palette": palette, "alpha": alpha})
    try:
        raw = zlib.decompress(b"".join(packed))
    except zlib.error as error:
        raise UsageError(f"{path}: the image data does not decompress: {error}")
    try:
        return header, rgba(header, raw)
    except UsageError as error:
        # The decoding errors say what is wrong with the data and not which of
        # the two files it was, since only this call knows that.
        raise UsageError(f"{path}: {error}")


def pixel_distance(first, second, at):
    """How far apart two pixels are: the largest of their four channel
    differences, which is the number a threshold is compared against -- a pixel
    that moved a lot in one channel has moved, however still the other three
    are."""
    return max(abs(first[at + channel] - second[at + channel]) for channel in range(4))


def compare(first, second, header, threshold):
    """One pass over the two images together: the largest per-channel
    difference anywhere in them and where it is, and how many pixels are over
    the threshold. The two are measured differently on purpose -- the largest
    difference is a number to read, the pixels over the threshold is a number
    to judge -- so the biggest wobble and the amount of wobbling are never
    taken for one another."""
    width, height = header["width"], header["height"]
    total = width * height
    worst, worst_at, changed = 0, None, 0
    for pixel in range(total):
        distance = pixel_distance(first, second, pixel * 4)
        if distance > threshold:
            changed += 1
        if distance > worst:
            worst, worst_at = distance, pixel
    return {"max_diff": worst, "worst": worst_at, "changed": changed,
            "total": total, "percent": changed / total * 100.0, "width": width, "height": height}


def over(value, budget):
    """Whether a measured number is past its budget: a budget is what is
    allowed, so landing exactly on it is still inside."""
    return value > budget


def budgets(args, result):
    """Every budget the run has, as (what it holds, the number measured, the
    number allowed, the unit it is written with, whether it is over). Only one
    of the two pixel budgets is ever in: a run that budgets a count of pixels
    is not also judged on the share of them, since one is the other said at a
    resolution."""
    rows = [("largest difference", result["max_diff"], args.max_diff, "/255")]
    if args.max_pixels is None:
        rows.append(("share of pixels over the threshold", result["percent"], args.max_percent, "%"))
    else:
        rows.append(("count of pixels over the threshold", result["changed"], args.max_pixels, ""))
    return [(*row, over(row[1], row[2])) for row in rows]


def cell(value, unit):
    """A measured number or a budget as the table holds it: a difference is
    written against its full scale, a percentage to two decimals, and a count
    of pixels as it is."""
    return f"{value:.2f}{unit}" if isinstance(value, float) else f"{value}{unit}"


def measured(result, value, unit):
    """A measured number as it is read. A share of pixels carries the count of
    them beside it, since a percentage of a few million pixels rounds away to
    nothing and the count is the number a reader wants anyway."""
    return f"{cell(value, unit)} ({result['changed']} px)" if unit == "%" else cell(value, unit)


def table(rows, headers):
    """The rows as a table, every column padded to its widest cell and the
    last one (the verdict) left as it is."""
    widths = [max(len(row[column]) for row in [headers, *rows]) for column in range(len(headers) - 1)]
    lines = []
    for row in [headers, *rows]:
        padded = "  ".join(cell.ljust(widths[index]) for index, cell in enumerate(row[:-1]))
        lines.append(f"  {padded}  {row[-1]}".rstrip())
    return lines


def show(text, out=sys.stdout):
    """Print a line."""
    print(text, file=out)


def report(before, after, result, rows, args, out):
    """What the comparison found, as a person reads it: what the two images
    were, the numbers that came out of them, and then every budget they are
    measured against."""
    show(f"before  {args.before}  ({before['name']})", out)
    show(f"after   {args.after}  ({after['name']})", out)
    show("", out)

    at = result["worst"]
    where = "-" if at is None else f"({at % result['width']},{at // result['width']})"
    show(f"{result['width']}x{result['height']}, {result['total']} pixels, every one compared as "
         f"rgba, alpha included", out)
    show(f"pixels over the threshold of {args.threshold}/255: {result['changed']} of "
         f"{result['total']} ({result['percent']:.2f}%)", out)
    show(f"largest per-channel difference: {result['max_diff']}/255 at {where} "
         f"(x,y from the top left)", out)
    show("", out)

    table_rows = [[label, measured(result, value, unit), f"at most {cell(budget, unit)}",
                   "over" if failed else "-"]
                  for label, value, budget, unit, failed in rows]
    for line in table(table_rows, ["budget", "measured", "allowed", ""]):
        show(line, out)

    failed = [row for row in rows if row[4]]
    if failed:
        show("", out)
        show("over budget", out)
        for label, value, budget, unit, _ in failed:
            show(f"   {label}: {measured(result, value, unit)} against {cell(budget, unit)}", out)
    else:
        show("", out)
        show("within every budget", out)


def parse_args(argv):
    """The command line: two images, how much of a per-channel difference
    counts as a change, and the budgets a change has to fit inside."""
    parser = argparse.ArgumentParser(
        prog="tools/png_compare.py",
        description="Compares two png images and says how far apart they are.",
        epilog="Exits 0 within every budget, 1 over one, 2 on a usage or parse error.")
    parser.add_argument("before", metavar="<before.png>", help="the image to compare against")
    parser.add_argument("after", metavar="<after.png>", help="the image to compare")
    parser.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD, metavar="N",
                        help="a per-channel difference above which a pixel counts as changed"
                             f" (default {DEFAULT_THRESHOLD}, so a one-LSB wobble is noise)")
    parser.add_argument("--max-diff", type=int, default=FULL_SCALE, metavar="N",
                        help="budget on the largest per-channel difference, 0-255"
                             f" (default {FULL_SCALE}, the whole range of a channel, so off)")
    parser.add_argument("--max-percent", type=float, default=0.0, metavar="PERCENT",
                        help="budget on the share of pixels over the threshold, 0-100"
                             " (default 0, so no pixel may change at all)")
    parser.add_argument("--max-pixels", type=int, default=None, metavar="N",
                        help="budget on the number of pixels over the threshold, which takes the "
                             "place of --max-percent (a resolution of its own is what makes a count "
                             "of pixels mean something)")
    args = parser.parse_args(argv)
    if args.threshold < 0:
        parser.error(f"--threshold {args.threshold}: a channel difference, 0 or more")
    if not 0 <= args.max_diff <= FULL_SCALE:
        parser.error(f"--max-diff {args.max_diff}: a channel difference is 0-255")
    if not 0.0 <= args.max_percent <= 100.0:
        parser.error(f"--max-percent {args.max_percent}: a percentage, 0 to 100")
    if args.max_pixels is not None and args.max_pixels < 0:
        parser.error(f"--max-pixels {args.max_pixels}: a number of pixels, 0 or more")
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        before, before_pixels = read_png(args.before)
        after, after_pixels = read_png(args.after)
        if (before["width"], before["height"]) != (after["width"], after["height"]):
            raise UsageError(f"the images are different sizes: {before['width']}x{before['height']}"
                             f" and {after['width']}x{after['height']}")
    except UsageError as error:
        print(f"png_compare.py: {error}", file=sys.stderr)
        return 2

    result = compare(before_pixels, after_pixels, before, args.threshold)
    rows = budgets(args, result)
    report(before, after, result, rows, args, sys.stdout)
    return 1 if any(row[4] for row in rows) else 0


if __name__ == "__main__":
    sys.exit(main())