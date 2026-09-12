import sys

from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont
import math

# Path to your TTF file
ttf_path = sys.argv[1]
output_png = sys.argv[2]

# Load font
font_size = 24
font = ImageFont.truetype(ttf_path, font_size)

# Load font cmap (character map)
tt = TTFont(ttf_path)

#cmap = tt["cmap"].getBestCmap()


# Get all unicode characters supported
chars = set()
tables = tt["cmap"].tables

for table in tables:
    print(len(table.cmap))
    #chars |= {chr(c) for c in table.cmap}

print(chars)
sys.exit(1)
# Grid settings
cols = 16
rows = math.ceil(len(chars) / cols)

cell_size = font_size + 20
img_width = cols * cell_size
img_height = rows * cell_size

# Create blank image
image = Image.new("RGBA", (img_width, img_height), "white")
draw = ImageDraw.Draw(image)

# Draw each glyph
for i, char in enumerate(chars):
    row = i // cols
    col = i % cols

    x = col * cell_size + 10
    y = row * cell_size + 10

    draw.text((x, y), char, font=font, fill="black")

# Save output
image.save(output_png)

print(f"Saved glyph sheet to {output_png}")