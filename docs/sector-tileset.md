# Sector textures

`resources/textures/sectors.tileset.yaml` defines the pixel regions used from
`sector-atlas-draft.png`. Version 1 uses explicit `[x, y, width, height]` rectangles
because the generated draft is not a uniform tile grid. Image dimensions and all
regions are validated before the renderer receives the texture.

The renderer repeats a surface every 1x1 world unit (64x160 screen pixels), cropping
partial cells. Shared boundary strips compose centre, end, corner and open-wall
variants rather than baking every combination into separate sprites. Floors and
ceilings appear only at sector bounds. Side strips use the existing wall-span
calculation, including partial-height openings into Corridors. Open spans receive
no strip. Room interiors therefore support arbitrary height without intermediate
floors. The current draft supplies small interior samples; it is not a finished
seamless art set.

Room, Corridor, Ladder, Lift, Shuttle, Stairwell and Staircase surfaces have
separate entries. Backgrounds and Facades intentionally retain their authored
opaque colours and no boundary strips. Vehicles, stair/ladder mechanisms,
thresholds, other objects, Agents and editor overlays retain their existing
rendering. Aperture clipping and render order are unchanged; wireframe mode
remains primitive-based.

CMake copies both assets beside the GUI executable under `textures/`, including
on builds with no C++ changes. Startup resolves that directory relative to the
executable, not the working directory. Invalid or missing assets produce a
controlled startup error. The GL texture is deleted before the GL context.
Headless rendering without an installed texture retains the primitive fallback.

Run `ctest --test-dir build-linux -R 'sector-tileset|headless-smoke|graphics-startup-failure' --output-on-failure`
after building the GUI, headless and `pf-sector-tileset-checks` targets.
