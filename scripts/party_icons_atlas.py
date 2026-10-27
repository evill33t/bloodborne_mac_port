#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the party death icons (gpu/art/party_icons.svg) into the atlas the overlay embeds (gpu/art/party_icons.bin)
(the strip the party HUD's list shows, scripts/party_hud_icons.bin, is coop_art.py's since 2026-10-17; --hud-strip
draws the old one from the SVG):
'BBIC', width, height and icon count (little-endian u16 each), then the RGBA pixels, zlib-compressed. A developer
tool, run after editing the drawing: it needs a Chromium browser (Edge or Chrome) to draw the SVG and Pillow."""
import argparse
import io
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

PORT = Path(__file__).resolve().parent.parent
SVG = PORT / 'gpu/art/party_icons.svg'
ATLAS = PORT / 'gpu/art/party_icons.bin'
HUD_STRIP = PORT / 'scripts/party_hud_icons.bin'
ICON, COUNT = 128, 7
# The HUD strip's frames (hud_patch.HUD_ICON_FRAMES): none, hunter, elsewhere, fallen, then the downed drop with its
# ring filled red to 0%, 10%, ... 100%; last the portraits' frame (h-frame, hud_patch.FRAME_PIXELS).
DOWNED_STEPS = 11
HUD_COUNT = 4 + DOWNED_STEPS + 1
BROWSERS = (r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
            r'C:\Program Files\Google\Chrome\Application\chrome.exe', 'chromium', 'google-chrome')


def encode(width, height, rgba, count=COUNT):
    if len(rgba) != width * height * 4 or width % ICON or width // ICON != count:
        raise ValueError('the atlas is a row of 128-pixel RGBA icons')
    return b'BBIC' + struct.pack('<HHH', width, height, count) + zlib.compress(bytes(rgba), 9)


def hud_strip_svg(svg_text):
    """The HUD strip as an SVG: the atlas drawing's symbols, placed as hud_patch.HUD_ICON_FRAMES lists them, then
    the portrait frame."""
    defs = svg_text[svg_text.index('<defs>'):svg_text.index('</defs>') + len('</defs>')]
    uses = ['<use href="#h-hunter" x="128" width="128" height="128"/>',
            '<use href="#h-lantern" x="256" width="128" height="128"/>',
            '<use href="#h-dead" x="384" width="128" height="128"/>']
    circumference = 2 * 3.14159265 * 52
    for step in range(DOWNED_STEPS):
        x = 128 * (4 + step)
        share = circumference * step / (DOWNED_STEPS - 1)
        uses.append(f'<use href="#h-down" x="{x}" width="128" height="128"/>')
        if share:
            uses.append(f'<circle cx="{x + 64}" cy="64" r="52" fill="none" stroke="#d0281e" stroke-width="7" '
                        f'stroke-dasharray="{share:.1f} 400" transform="rotate(-90 {x + 64} 64)"/>')
    uses.append(f'<use href="#h-frame" x="{128 * (4 + DOWNED_STEPS)}" width="128" height="128"/>')
    width = ICON * HUD_COUNT
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} 128" width="{width}" height="128">'
            + defs + ''.join(uses) + '</svg>')


def decode(data):
    """(width, height, count, rgba) of an atlas file (the overlay reads it the same way)."""
    if data[:4] != b'BBIC':
        raise ValueError('not a party icon atlas')
    width, height, count = struct.unpack_from('<HHH', data, 4)
    rgba = zlib.decompress(data[10:])
    if len(rgba) != width * height * 4:
        raise ValueError('the atlas has the wrong number of pixels')
    return width, height, count, rgba


def draw(browser, svg_text, png, count):
    with tempfile.TemporaryDirectory() as folder:
        page = Path(folder) / 'atlas.html'
        page.write_text(f'<html><body style="margin:0;background:transparent">{svg_text}</body></html>',
                        encoding='utf-8')
        subprocess.run([browser, '--headless=new', '--disable-gpu', '--hide-scrollbars',
                        '--default-background-color=00000000', '--force-device-scale-factor=1',
                        f'--window-size={ICON * count},{ICON}', f'--screenshot={png}', page.as_uri()],
                       check=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', help='a Chromium browser (default: Edge, then Chrome)')
    parser.add_argument('--hud-strip', action='store_true', help="also the HUD strip from the SVG (coop_art.py's now)")
    args = parser.parse_args()
    browser = args.browser or next((b for b in BROWSERS if Path(b).is_file() or shutil.which(b)), None)
    if not browser:
        raise SystemExit('No Chromium browser found: pass --browser')
    from PIL.PngImagePlugin import PngImageFile
    svg_text = SVG.read_text(encoding='utf-8')
    targets = [(ATLAS, svg_text, COUNT)] + ([(HUD_STRIP, hud_strip_svg(svg_text), HUD_COUNT)] if args.hud_strip else [])
    for target, text, count in targets:
        with tempfile.TemporaryDirectory() as folder:
            png = Path(folder) / 'atlas.png'
            draw(browser, text, png, count)
            image = PngImageFile(io.BytesIO(png.read_bytes())).convert('RGBA')
            if image.size != (ICON * count, ICON):
                raise SystemExit(f'the browser drew {image.size}, not {(ICON * count, ICON)}')
            target.write_bytes(encode(*image.size, image.tobytes(), count))
        print(f'{target}: {target.stat().st_size} bytes')


if __name__ == '__main__':
    main()
