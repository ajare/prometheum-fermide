#!/usr/bin/env python3
"""Pack generated artwork into uniform 1x1-world-unit RGBA cells (Pillow)."""
from pathlib import Path
from PIL import Image, ImageOps

root = Path(__file__).resolve().parents[1] / 'resources/textures'
source = Image.open(root / 'source/objects-generated.png').convert('RGBA')
# name, source crop, content rect within a 64x160 cell, neutral/tintable
sprites = [
    ('door', (489, 43, 615, 189), (6, 80, 52, 80), False),
    ('button-enabled', (1179, 84, 1261, 165), (29, 112, 6, 16), False),
    ('button-disabled', (960, 85, 1039, 164), (29, 112, 6, 16), False),
    ('window-clear', (1338, 78, 1543, 173), (6, 80, 52, 48), False),
    ('window-frosted', (1338, 78, 1543, 173), (6, 80, 52, 48), False),
    ('window-tinted', (1338, 78, 1543, 173), (6, 80, 52, 48), False),
    ('agent', (62, 251, 163, 405), (19, 88, 26, 72), True),
    ('marker', (1594, 469, 1735, 620), (24, 122, 16, 22), True),
    ('platform-lift', (14, 837, 208, 864), (0, 150, 64, 10), False),
]
atlas = Image.new('RGBA', (320, 320))
lines = ['# Pixel coordinates use top-left origin. Each cell is one world unit.',
         '# Content rectangles exclude alpha padding; renderer maps content to physical bounds.',
         'version: 1', 'image: objects.png', 'size: [320, 320]',
         'tile_size: [64, 160]', 'sprites:']
for index, (name, crop, content, neutral) in enumerate(sprites):
    sprite = source.crop(crop)
    if name == 'door':
        # A closed leaf must fully occlude the sector surface beneath it.
        sprite.putalpha(255)
    if neutral:
        alpha = sprite.getchannel('A')
        # White/grayscale RGB allows arbitrary ImGui vertex-colour overrides.
        gray = ImageOps.grayscale(sprite)
        sprite = Image.merge('RGBA', (gray, gray, gray, alpha))
    if name == 'window-clear':
        # Clear centre must reveal the actual back Sector, not baked blue glass.
        from PIL import ImageDraw
        ImageDraw.Draw(sprite).rectangle((23, 22, 179, 72), fill=(0, 0, 0, 0))
    elif name == 'window-tinted':
        alpha = sprite.getchannel('A').point(lambda a: round(a * 0.55))
        sprite.putalpha(alpha)
    x, y, w, h = content
    sprite = sprite.resize((w, h), Image.Resampling.LANCZOS)
    # Strip nearly invisible generation noise and prevent transparent RGB fringes.
    pixels = [(r, g, b, a) if a >= 12 else (0, 0, 0, 0) for r, g, b, a in sprite.getdata()]
    sprite.putdata(pixels)
    cell_x, cell_y = (index % 5) * 64, (index // 5) * 160
    atlas.paste(sprite, (cell_x + x, cell_y + y))
    lines += [f'  {name}:', f'    cell: [{index % 5}, {index // 5}]',
              f'    content: [{x}, {y}, {w}, {h}]', f'    tintable: {str(neutral).lower()}']
atlas.save(root / 'objects.png')
(root / 'objects.tileset.yaml').write_text('\n'.join(lines) + '\n')
