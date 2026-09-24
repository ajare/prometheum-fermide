# Object atlas

`resources/textures/objects.png` is a 320x320 RGBA atlas. Its uniform cells are
64x160 pixels: the same 1x1 world-unit footprint used by sector rendering, not
64x64 screen pixels. Unoccupied cell space is transparent. Small objects do not
expand to fill their cell.

`objects.tileset.yaml` records each sprite's cell, local content rectangle and
whether its RGB can be tinted. Content UVs map to the renderer's existing physical
bounds; alpha padding never changes collision, selection or visual sizes.
The Agent and Marker sprites have grayscale RGB. Agents use the existing resolved
Agent colour, including tag overrides and selection, as the ImGui vertex tint.
Existing height scaling, horizontal centring and foot position are preserved.

Supported sprites:

- Door leaf (all four opening styles): source UV cropping preserves sliding
  rather than compressing the sprite. Fully open leaves emit no geometry.
- Enabled/disabled Buttons, at the existing Button bounds.
- Clear, frosted and tinted Windows: back-sector rendering precedes the overlay.
  Clear glass has a transparent centre; tinted glass is translucent.
- Agents and Markers, keeping the existing icon-fit dimensions.
- Platform lift car: the sprite follows the moving slab's actual bounds, not the
  shaft bounds. The editor shaft outline is retained.

Bulkhead Doors and Force Bridges remain entirely primitive-rendered. Other
transport geometry and editor overlays are unchanged. Headless tests without a
texture retain the primitive/font fallback.

The GUI loads and validates both the definition and PNG beside its executable,
under `textures/`. CMake deploys them even on builds without C++ changes. Missing
or invalid assets cause a controlled startup error, and the GL texture is released
before the context is destroyed.

The original generated art is in `resources/textures/source/objects-generated.png`.
Run `python3 scripts/pack_object_atlas.py` (requires Pillow) to reproduce the packed
PNG and YAML. This crops the irregular generated sheet, normalizes cells, removes
near-transparent noise and neutralizes tintable sprites. The generated art remains
a draft; the packer makes its sizing and alpha layout deterministic.

The `sector-tileset` CTest also checks object quads, inverted screen-Y bounds,
Agent tint, door UV cropping, fully-open leaf suppression and cleanup.
