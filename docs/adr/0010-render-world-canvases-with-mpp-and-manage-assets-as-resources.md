# Render World canvases with MPP and manage application assets as Willpower resources

Status: accepted

Dear ImGui remains the editor's immediate-mode user-interface layer, but World canvases are rendered by Massive Poly Pusher (MPP) into an offscreen render texture that ImGui presents as an image. A CPU-side `WorldDrawList` records ordered canvas primitives independently of either rendering API. The MPP renderer consumes that stream using native triangle and line batches, MPP text, clipping, and Willpower-managed atlas textures. Thick lines and outlines remain native line primitives and use the supported OpenGL line-width range; they are not expanded into triangle geometry.

One editor rendering service owns the MPP render system, MPP resource manager, and Willpower application resource manager, and destroys them before the OpenGL context. `resources/Resources.yaml` is the application asset manifest. Sector and object atlases are Willpower `Image` and `ImageSet` resources. Worlds, Agent tag registries, Agent behaviour registries, and individual Lua modules have application `Resource` subclasses; dependencies in the manifest connect bundled Worlds to their registries and behaviour registries to their Lua modules. Interactive user documents may be registered programmatically instead of rewriting the bundled manifest.

## Considered options

- **Continue drawing World canvases directly with `ImDrawList`.** Rejected because it keeps simulation visualization tied to the UI backend and prevents a coherent MPP scene and resource lifecycle.
- **Expand thick lines and outlines into triangles.** Rejected because lines are a distinct primitive in the render plan and MPP supports native line batches. Platform line-width limits are handled by clamping to the graphics driver's advertised range.
- **Introduce project-specific tile-set resource classes.** Rejected because Willpower `ImageSet` already provides named atlas rectangles and UV coordinates. Tintability and repetition are rendering semantics rather than resource kinds.
- **Keep atlas metadata and registry ownership in parallel global caches.** Rejected because duplicate authorities permit the rendered or edited object to diverge from the resource-manager object.
- **Move the whole editor UI to MPP.** Rejected because Dear ImGui remains appropriate for menus, panels, controls, docking, and presentation of the offscreen World texture.

## Consequences

World rendering can be tested at the stable CPU command-stream boundary without depending on ImGui's internal vertex tessellation. Draw-order barriers and clip rectangles are explicit. MPP performs the GPU drawing and owns the offscreen target, while ImGui only composites that target into the viewport. Application assets fail during rendering-service startup when required manifests, render infrastructure, or atlas resources are unavailable.

Registry resources own the loaded registry instances used to resolve a World resource. In accordance with ADR 0008, an unavailable or refused Agent behaviour registry remains recoverable World dependency state rather than malformed World data; tag-registry incompatibility remains a World-load failure. Lua remains sandboxed and registry-local as specified by ADR 0008—the resource layer adds managed script assets and lifecycle without changing script capabilities or execution semantics.
