#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The co-op screens' own art (Kaniel 2026-10-17: drawn for the port with Nano Banana, flat gold on a flat dark
ground), from the pictures to what the port embeds. A developer tool, run after new art comes; it needs Pillow.

  coop_art.py cut --sigils S --badges B --medallions M --card C --dividers D --marks K --frame F --backdrop G
      cuts the generated sheets into the pieces in gpu/art/coop/ (PNG, the flat ground made clear);
  coop_art.py cut-icon --settings S
      cuts one icon (Kaniel 2026-10-29: the team screen's settings button, silver on a flat ground) into its piece;
  coop_art.py pack
      packs the pieces into gpu/art/coop_art.bin (the overlay's atlas: the team screen and the team loading screen)
      and puts the badges and the portrait frame into the party HUD's strip (scripts/party_hud_icons.bin,
      hud_patch.py: the party list's icons and the ring round each portrait).

coop_art.bin: 'BBCA', the atlas's width and height, the piece count (little-endian u16 each), then x, y, width and
height of each piece in PIECES' order (u16 each), then the atlas's RGBA pixels, zlib-compressed
(shim/bbport_coop_art.h reads it)."""
import argparse
from pathlib import Path
import struct
import zlib

PORT = Path(__file__).resolve().parent.parent
PIECE_DIR = PORT / 'gpu/art/coop'
ATLAS = PORT / 'gpu/art/coop_art.bin'
HUD_STRIP = PORT / 'scripts/party_hud_icons.bin'
# The overlay's pieces, in the order shim/bbport_coop_art.h's Piece lists them, and the scale each is kept at in the
# atlas (one scale for pieces drawn over each other: the two medallions' rings stay the same size).
PIECES = (('sigil0', 0.64), ('sigil1', 0.64), ('sigil2', 0.64), ('sigil3', 0.64),
          ('medallion', 0.6), ('medallion-ready', 0.6), ('card', 0.7), ('divider', 0.52), ('title-ornament', 0.7),
          ('mark-ready', 0.42), ('mark-leader', 0.42), ('mark-invite', 0.42), ('backdrop', 1.0), ('settings', 1.0))
# An icon's piece, at most this many pixels a side (the settings button is about 60 at 1080p, 120 at 4K).
ICON_SIDE = 256
ATLAS_SIDE = 2048
# The party HUD strip (party_icons_atlas.py): 128-pixel frames none, hunter, elsewhere, fallen, the downed drop with
# its ring red to 0%, 10%, ... 100%, then the portraits' frame.
ICON, DOWNED_STEPS = 128, 11
HUD_COUNT = 4 + DOWNED_STEPS + 1
# The downed ring on the badge's gold band, in a frame's 128 units from its centre.
DOWNED_RING = (52.0, 7.0)
DOWNED_RED = (208, 40, 30)


def picture(path):
    """A picture file (Pillow's, read as bytes)."""
    from PIL.Image import open as open_image
    return open_image(path)


def key_ground(image, ground=None, clear=12.0, solid=48.0):
    """The picture with its flat ground made clear: each pixel's cover from its colour's distance to the ground
    (the ground's colour from the border when not given), its colour taken back out of the blend."""
    from PIL import Image
    image = image.convert('RGB')
    if ground is None:
        w, h = image.size
        border = [image.getpixel((x, y)) for x in range(0, w, max(1, w // 32)) for y in (0, h - 1)] + \
                 [image.getpixel((x, y)) for y in range(0, h, max(1, h // 32)) for x in (0, w - 1)]
        ground = tuple(sorted(p[c] for p in border)[len(border) // 2] for c in range(3))
    out = Image.new('RGBA', image.size)
    src, dst = image.load(), out.load()
    for y in range(image.size[1]):
        for x in range(image.size[0]):
            r, g, b = src[x, y]
            d = ((r - ground[0]) ** 2 + (g - ground[1]) ** 2 + (b - ground[2]) ** 2) ** 0.5
            a = min(1.0, max(0.0, (d - clear) / (solid - clear)))
            if a <= 0.0:
                dst[x, y] = (0, 0, 0, 0)
                continue
            un = [min(255, max(0, round((v - k * (1 - a)) / a))) for v, k in zip((r, g, b), ground)]
            dst[x, y] = (*un, round(a * 255))
    return out


def clear_outside(image, tolerance=14.0):
    """The picture with the ground round it made clear (flooded from its border), what it encloses kept: a card's
    inside stays."""
    image = image.convert('RGB')
    w, h = image.size
    src = image.load()
    ground = src[1, 1]
    near = lambda p: sum((a - b) ** 2 for a, b in zip(p, ground)) ** 0.5 <= tolerance
    seen = bytearray(w * h)
    stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:
        x, y = stack.pop()
        if x < 0 or y < 0 or x >= w or y >= h or seen[y * w + x] or not near(src[x, y]):
            continue
        seen[y * w + x] = 1
        stack += ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))
    out = image.convert('RGBA')
    dst = out.load()
    for y in range(h):
        for x in range(w):
            if seen[y * w + x]:
                dst[x, y] = (0, 0, 0, 0)
    return out


def trim(image, pad=4):
    """The picture cut to what shows in it, `pad` clear pixels round it."""
    box = image.getchannel('A').point(lambda a: 255 if a > 64 else 0).getbbox()
    if box is None:
        raise ValueError('nothing shows in the picture')
    x0, y0, x1, y1 = box
    w, h = image.size
    return image.crop((max(0, x0 - pad), max(0, y0 - pad), min(w, x1 + pad), min(h, y1 + pad)))


def square(image):
    """The picture centred in a clear square."""
    from PIL import Image
    side = max(image.size)
    out = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    out.paste(image, ((side - image.size[0]) // 2, (side - image.size[1]) // 2))
    return out


def split(image, count):
    """A sheet of `count` pieces side by side, cut into equal columns."""
    w, h = image.size
    return [image.crop((w * i // count, 0, w * (i + 1) // count, h)) for i in range(count)]


def cut(args):
    from PIL import Image, ImageChops
    PIECE_DIR.mkdir(parents=True, exist_ok=True)
    pieces = {}
    for i, part in enumerate(split(picture(args.sigils), 4)):
        pieces[f'sigil{i}'] = square(trim(key_ground(part)))
    for name, part in zip(('badge-hunter', 'badge-elsewhere', 'badge-fallen', 'badge-downed'),
                          split(picture(args.badges), 4)):
        pieces[name] = square(trim(key_ground(part)))
    for name, part in zip(('medallion', 'medallion-ready'), split(picture(args.medallions), 2)):
        pieces[name] = square(trim(key_ground(part)))
    # The card: cut to its gold border (the ground round it is not quite flat).
    card = picture(args.card).convert('RGB')
    r, _, b = card.split()
    bright = Image.eval(r, lambda v: 255 if v > 120 else 0)
    warm = Image.eval(ImageChops.subtract(r, b), lambda v: 255 if v > 30 else 0)  # gold, not grey
    border = ImageChops.multiply(bright, warm)
    pieces['card'] = clear_outside(card).crop(border.getbbox())
    sheet = picture(args.dividers)
    w, h = sheet.size
    pieces['divider'] = trim(key_ground(sheet.crop((0, 0, w, h * 9 // 20))))
    pieces['title-ornament'] = trim(key_ground(sheet.crop((0, h * 9 // 20, w, h))))
    for name, part in zip(('mark-ready', 'mark-leader', 'mark-invite'), split(picture(args.marks), 3)):
        pieces[name] = square(trim(key_ground(part)))
    pieces['portrait-frame'] = square(trim(key_ground(picture(args.frame)), pad=2))
    # The backdrop: the picture inside its dark margin, opaque.
    backdrop = picture(args.backdrop).convert('RGB')
    bw, bh = backdrop.size
    pieces['backdrop'] = backdrop.crop((round(bw * 0.052), round(bh * 0.089), round(bw * 0.948), bh)).convert('RGBA')
    for name, image in pieces.items():
        image.save(PIECE_DIR / f'{name}.png', optimize=True)
        print(f'{name}: {image.size}')


def cut_icon(args):
    PIECE_DIR.mkdir(parents=True, exist_ok=True)
    image = fit(square(trim(key_ground(picture(args.settings)))), ICON_SIDE)
    image.save(PIECE_DIR / 'settings.png', optimize=True)
    print(f'settings: {image.size}')


def fit(image, longest):
    """The picture no more than `longest` pixels on its longer side."""
    w, h = image.size
    return scaled(image, min(1.0, longest / max(w, h)))


def scaled(image, scale):
    from PIL import Image
    w, h = image.size
    return image.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS) if scale != 1 else image


def shelf_pack(sizes, side):
    """(x, y) for each (w, h), rows filled left to right, tallest first; a 2-pixel gap between pieces."""
    order = sorted(range(len(sizes)), key=lambda i: -sizes[i][1])
    places, x, y, row = [None] * len(sizes), 0, 0, 0
    for i in order:
        w, h = sizes[i]
        if x + w > side:
            x, y, row = 0, y + row + 2, 0
        if w > side or y + h > side:
            raise ValueError('the pieces do not fit the atlas')
        places[i] = (x, y)
        x, row = x + w + 2, max(row, h)
    return places


def encode_atlas(images, side=ATLAS_SIDE):
    from PIL import Image
    places = shelf_pack([im.size for im in images], side)
    atlas = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    for image, place in zip(images, places):
        atlas.paste(image, place)
    head = b'BBCA' + struct.pack('<HHH', side, side, len(images))
    for image, (x, y) in zip(images, places):
        head += struct.pack('<HHHH', x, y, *image.size)
    return head + zlib.compress(atlas.tobytes(), 9)


def decode_atlas(data):
    """(width, height, [(x, y, w, h)], rgba) of an atlas file (the overlay reads it the same way)."""
    if data[:4] != b'BBCA':
        raise ValueError('not a co-op art atlas')
    width, height, count = struct.unpack_from('<HHH', data, 4)
    rects = [struct.unpack_from('<HHHH', data, 10 + 8 * i) for i in range(count)]
    rgba = zlib.decompress(data[10 + 8 * count:])
    if len(rgba) != width * height * 4:
        raise ValueError('the atlas has the wrong number of pixels')
    return width, height, rects, rgba


def downed_badge(badge, share):
    """The downed badge with its ring red to `share` (0..1), clockwise from the top."""
    from PIL import Image, ImageDraw
    scale = 4
    ring = Image.new('L', (ICON * scale, ICON * scale), 0)
    if share > 0:
        radius, width = DOWNED_RING[0] * scale, DOWNED_RING[1] * scale
        c = ICON * scale / 2
        ImageDraw.Draw(ring).arc((c - radius - width / 2, c - radius - width / 2, c + radius + width / 2,
                                  c + radius + width / 2), -90, -90 + 360 * share, fill=255, width=round(width))
    ring = ring.resize((ICON, ICON), Image.LANCZOS)
    red = Image.new('RGBA', (ICON, ICON), (*DOWNED_RED, 255))
    red.putalpha(ring)
    return Image.alpha_composite(badge, red)


def hud_strip():
    """The party HUD's strip from the badges and the portrait frame."""
    from PIL import Image
    load = lambda name: fit(picture(PIECE_DIR / f'{name}.png').convert('RGBA'), ICON).resize((ICON, ICON),
                                                                                                  Image.LANCZOS)
    frames = [Image.new('RGBA', (ICON, ICON), (0, 0, 0, 0)), load('badge-hunter'), load('badge-elsewhere'),
              load('badge-fallen')]
    downed = load('badge-downed')
    frames += [downed_badge(downed, step / (DOWNED_STEPS - 1)) for step in range(DOWNED_STEPS)]
    frames.append(load('portrait-frame'))
    strip = Image.new('RGBA', (ICON * HUD_COUNT, ICON), (0, 0, 0, 0))
    for i, frame in enumerate(frames):
        strip.paste(frame, (ICON * i, 0))
    return b'BBIC' + struct.pack('<HHH', ICON * HUD_COUNT, ICON, HUD_COUNT) + zlib.compress(strip.tobytes(), 9)


def pack(_args):
    images = [scaled(picture(PIECE_DIR / f'{name}.png').convert('RGBA'), scale) for name, scale in PIECES]
    ATLAS.write_bytes(encode_atlas(images))
    print(f'{ATLAS}: {ATLAS.stat().st_size} bytes')
    HUD_STRIP.write_bytes(hud_strip())
    print(f'{HUD_STRIP}: {HUD_STRIP.stat().st_size} bytes')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    cut_parser = commands.add_parser('cut')
    for name in ('sigils', 'badges', 'medallions', 'card', 'dividers', 'marks', 'frame', 'backdrop'):
        cut_parser.add_argument(f'--{name}', required=True)
    commands.add_parser('cut-icon').add_argument('--settings', required=True)
    commands.add_parser('pack')
    args = parser.parse_args()
    {'cut': cut, 'cut-icon': cut_icon, 'pack': pack}[args.command](args)


if __name__ == '__main__':
    main()
