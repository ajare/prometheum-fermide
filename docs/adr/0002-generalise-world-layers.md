# Generalise World from two layers to N ordered layers

The simulation currently hard-codes exactly two spatial depth planes: Fore (0) and Back (1). Corridors must be Fore, transits must be Back, and doors/windows bridge Fore↔Back. We will generalise this to a user-defined number of ordered layers (2–256). A layer is a spatial depth plane; layer 0 is front-most and layer N-1 is back-most. Locations may occupy any layer; a transit on layer L has its landing locations on layer L-1. Doors and windows connect only adjacent layers and are authored on the front layer of the pair. The renderer shows the selected layer L solid and whole. Layer L+1 is drawn solid through the apertures that layer L's Locations give it, and - while the wireframe overlay is on - additionally outlined over layer L, so its whole footprint can be read through the selection. The overlay adds outlines only: never a fill, Transit geometry, or the Agents inside the Layer behind. Nothing other than the selection and the one Layer behind it is drawn. This preserves the existing two-layer visual and traversal semantics while allowing deeper worlds.

## Considered options

- **Keep exactly two layers.** Rejected: it prevents scenarios where a world needs more than one depth of back-layer transit or stacked foreground spaces.
- **Allow arbitrary layer-to-layer thresholds (doors connecting any two layers).** Rejected: it complicates rendering occlusion and graph pairing without adding a compelling gameplay use case; adjacent-only thresholds keep the model local and predictable.
- **Render all layers as wireframe context.** Rejected: it clutters the canvas; showing only layer L+1 behind the visible layer L keeps the view readable and matches the current Fore/Back experience.

## Consequences

- `CORE_LAYER_FORE`, `CORE_LAYER_BACK`, and `CORE_NUM_LAYERS` will be removed; layer counts come from `mLayers.size()` and helpers such as `isFrontMostLayer`, `isBackMostLayer`, `layerBehind`, and `layerInFront`.
- Serialisation moves to `version: 4` with a top-level `layers` field and optional `layerNames`.
- Deleting a layer is a destructive, user-confirmed operation: it removes all sectors on that layer, removes transits on the deleted layer and the layer behind it (because their landing relationships break), compacts higher layers down by one, and removes agents on affected sectors.
- Graph construction must process adjacent layer pairs iteratively rather than assuming a single global Fore/Back lookup.
