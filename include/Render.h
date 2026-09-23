#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "imgui/imgui.h"

#include "core/Background.h"
#include "core/World.h"
#include "core/Facade.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Graph.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/Location.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SectorType.h"
#include "core/ShuttleTransit.h"
#include "core/StaircaseTransit.h"
#include "core/StairwellTransit.h"
#include "core/Vector2.h"


#define RENDER_VERTEX_SIZE						5
#define RENDER_INTER_LAYER_EDGE_SIZE			11

//
// Marker icon geometry.
//
// A Marker's icon floats MarkerFloorLift world units above the floor its Vertex
// sits on, which clears the floor line and lets the editor's hit box cover the
// icon plus the gap down to the floor. The Vertex itself stays on the ground;
// only the icon lifts.
//
inline constexpr float MarkerIconSize{ 22.0f };	// longest edge of the drawn icon, in pixels
inline constexpr float MarkerFloorLift{ 0.1f };	// how far the icon floats above the floor, in world units

//
// How a Sector is drawn by one pass of the viewport.
//
// The selected Layer is drawn solid and whole. The Layer directly behind it is
// drawn by two passes: solid, clipped to the apertures the selected Layer gives
// it, and - while the wireframe overlay is on - outlined over the selection.
// Every other Layer is hidden: nothing in front of the selection, and nothing
// more than one Layer behind it.
//
enum class LayerRenderStyle
{
	Hidden,		// The Layer is not drawn at all.
	Solid,		// The selected Layer, drawn whole.
	Wireframe,	// The Layer directly behind the selected Layer, outlined over the selection.
	Aperture	// The Layer behind, drawn solid through an aperture in the selected Layer.
};

//
// One pass of the viewport: the Layer it draws and the style it draws it with.
//
struct RenderPass
{
	uint32_t layer;
	LayerRenderStyle style;
};

//
// The passes the viewport runs for one selected Layer, in draw order:
//
//   1. the selected Layer, drawn solid and whole;
//   2. the Transits of the Layer directly behind, drawn solid through the
//      apertures the selected Layer's Locations give them;
//   3. while the wireframe overlay is on, the whole Layer directly behind,
//      outlined over the selection.
//
// Pass 2 runs whether or not the overlay is on. The overlay contributes the
// Layer behind's outlines; it is not what makes that Layer visible.
//
inline std::vector<RenderPass> renderPasses(uint32_t viewLayer, uint32_t layerCount,
	bool wireframeOverlay)
{
	std::vector<RenderPass> passes;

	if (viewLayer >= layerCount)
	{
		return passes;
	}

	passes.push_back({ viewLayer, LayerRenderStyle::Solid });

	if (viewLayer + 1 < layerCount)
	{
		auto const behind = core::layerBehind(viewLayer);

		passes.push_back({ behind, LayerRenderStyle::Aperture });

		if (wireframeOverlay)
		{
			passes.push_back({ behind, LayerRenderStyle::Wireframe });
		}
	}

	return passes;
}

// True while the Layer reaches the screen at all: the selected Layer, or the Layer
// directly behind it.
inline bool isLayerDrawn(uint32_t layer, uint32_t viewLayer, uint32_t layerCount)
{
	if (layer >= layerCount || viewLayer >= layerCount)
	{
		return false;
	}

	return layer == viewLayer || layer == viewLayer + 1;
}

// Filled rather than outlined. Both the selected Layer and the Layer seen through
// one of its apertures are drawn solid; only the wireframe overlay is not.
inline bool isDrawnSolid(LayerRenderStyle style)
{
	return style == LayerRenderStyle::Solid || style == LayerRenderStyle::Aperture;
}

//
// The tint a Sector takes when its lights are off.
//
// A flat-colour Sector bypasses this entirely (see bypassesLightsOffTint());
// every other Sector wears it in place of its Layer's colour.
//
inline constexpr core::BackgroundColour LightsOffTint{ 48, 48, 48 };

//
// Whether a Sector is rendered as its own flat, opaque colour.
//
// A Background has always been drawn this way: its colour is its whole surface.
// A Facade follows the same rendering rule (ADR 0003) - the user-authored
// colour is the surface, the generic Layer fill is skipped so nothing paints
// twice, and the lights-off tint never applies.
//
inline bool rendersAsFlatColour(core::SectorType type)
{
	return type == core::SectorType::Background || type == core::SectorType::Facade;
}

//
// The flat surface colour a Sector carries of its own, if it carries one.
//
inline std::optional<core::BackgroundColour> flatSurfaceColour(core::Sector const& sector)
{
	if (sector.getType() == core::SectorType::Background)
	{
		return static_cast<core::Background const&>(sector).getColour();
	}

	if (sector.getType() == core::SectorType::Facade)
	{
		return static_cast<core::Facade const&>(sector).getColour();
	}

	return std::nullopt;
}

//
// Whether the lights-off tint skips a Sector type.
//
// A flat-colour Sector's colour is user-authored; tinting it when the lights
// are off would silently override the picker. A Background has no lights to
// switch at all. A Facade does: its light switch keeps meaning exactly what it
// meant for the agents and objects inside - only the fill refuses to follow it
// (ADR 0003).
//
inline bool bypassesLightsOffTint(core::SectorType type)
{
	return rendersAsFlatColour(type);
}

//
// The colour a clear Window's Aperture pass fills the Sector behind it with.
//
// A Background is seen in its own colour: the glass shows what is actually
// behind it, so the pass is handed the Background's own colour rather than the
// generic back-layer tint. A Facade behind glass does the same - it renders as
// a solid colour exactly like a Background, so its user-authored colour is
// what the glass shows. Any other back Sector carries no colour of its own and
// yields std::nullopt, leaving the caller's generic tint in place. Where a
// Window faces several Backgrounds at once, backgroundApertureRegions() below
// answers this once per Background in the composite.
//
inline std::optional<core::BackgroundColour> apertureFillColour(core::Sector const& backSector)
{
	return flatSurfaceColour(backSector);
}

//
// One piece of a multi-Background aperture composite: the Background the glass
// shows through one stretch of the aperture, and the world-space (cell-unit)
// rectangle that Background is clipped to while being drawn.
//
struct BackgroundApertureRegion
{
	std::shared_ptr<const core::Background> background;
	core::Vector2 min;
	core::Vector2 max;
};

//
// The regions through which one aperture (a clear Window's rect on the Layer
// in front) sees the Layer behind it, derived from that Layer's cell grid.
//
// The Window's single back Sector is not consulted: #36 made it
// non-authoritative for a span of several Backgrounds - it names only the
// Sector behind the Window's first cell. The cell grid is what the aperture
// actually looks into, so each Background occupying cells under the aperture
// becomes one region, clipped to the intersection of the aperture rect and
// that Background's own rect. Two Backgrounds side by side therefore meet
// exactly on the cell boundary between them: neither bleeds past it, and no
// seam line is drawn across it.
//
// Regions are ordered by the grid's left-to-right, top-to-bottom sweep of
// first appearance, so the composite is deterministic. Cells holding no
// Background - empty space, or a non-Background Sector - contribute no
// region. An aperture which sees no Background at all (an ordinary Window
// into a Room, or an empty span) yields no regions, and the caller keeps
// its single-sector path. Because the rects are world-space, the composite
// is pinned to the world: scrolling the viewport moves the seam with the
// Backgrounds, never with the screen.
//
inline std::vector<BackgroundApertureRegion> backgroundApertureRegions(
	core::World const& world,
	uint32_t backLayerIndex,
	core::Vector2 const& apertureMin,
	core::Vector2 const& apertureMax)
{
	std::vector<BackgroundApertureRegion> regions;

	if (backLayerIndex >= world.getLayerCount())
	{
		return regions;
	}

	int const x0 = std::max(static_cast<int>(std::floor(apertureMin.x)), 0);
	int const y0 = std::max(static_cast<int>(std::floor(apertureMin.y)), 0);
	int const x1 = std::min(static_cast<int>(std::ceil(apertureMax.x)) - 1,
		static_cast<int>(world.getCellsWide()) - 1);
	int const y1 = std::min(static_cast<int>(std::ceil(apertureMax.y)) - 1,
		static_cast<int>(world.getLevelsHigh()) - 1);

	for (int y = y0; y <= y1; ++y)
	{
		for (int x = x0; x <= x1; ++x)
		{
			auto const sector = world.getSectorAtPosition(backLayerIndex,
				static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);

			if (!sector || sector->getType() != core::SectorType::Background)
			{
				continue;
			}

			auto const background = std::static_pointer_cast<const core::Background>(sector);

			// A Background is rectangular, so it enters the aperture once; the
			// grid may revisit it from every cell it covers under the aperture.
			bool alreadySeen = false;
			for (auto const& region : regions)
			{
				if (region.background == background)
				{
					alreadySeen = true;
					break;
				}
			}
			if (alreadySeen)
			{
				continue;
			}

			core::Vector2 bgMin, bgMax;
			background->getBounds(bgMin, bgMax);

			BackgroundApertureRegion region;
			region.background = background;
			region.min = { std::max(bgMin.x, apertureMin.x), std::max(bgMin.y, apertureMin.y) };
			region.max = { std::min(bgMax.x, apertureMax.x), std::min(bgMax.y, apertureMax.y) };

			if (region.max.x - region.min.x <= 0.0f || region.max.y - region.min.y <= 0.0f)
			{
				continue;
			}

			regions.push_back(region);
		}
	}

	return regions;
}

//
// Whether a Background is filled in this style.
//
// The selected Layer fills it with its own raw colour, and so does the view
// through an aperture - the latter already clipped to the Window by the caller.
//
inline bool shouldFillBackground(LayerRenderStyle style)
{
	return isDrawnSolid(style);
}

//
// Whether a Background contributes its outline in this style.
//
// This is the narrowing ticket #35 records. The umbrella spec (#27) said "no
// per-sector border - the selection highlight is the only time an individual
// Background's outline is drawn", which collides with ADR 0002: the wireframe
// overlay exists to convey the shape of the Layer behind, so a Background that
// contributed nothing there would leave no cue at all that the Layer behind has
// extent whenever it is not behind an aperture.
//
// The rule keeps its shape and loses its blanket:
//
//   * the Solid pass still draws no border, so adjacent same-colour Backgrounds
//     meet seamlessly;
//   * the overlay keeps the outline;
//   * selection is untouched - shouldHighlightSelectedSector() still fires for a
//     selected Background.
//
// One pass never both fills and outlines a Background: a border drawn over its
// own fill is exactly the seam the Solid pass must not show.
//
inline bool shouldOutlineBackground(LayerRenderStyle style)
{
	return style == LayerRenderStyle::Wireframe;
}

//
// The selection highlight is an editor overlay drawn over the selected Sector on
// the selected Layer. It is deliberately type-agnostic: a Background takes it
// exactly as a Location does, which is why nothing in this rule names one.
//
inline bool shouldHighlightSelectedSector(LayerRenderStyle style, uint32_t sectorLayer,
	uint32_t viewLayer, bool sectorSelectionMode)
{
	return sectorSelectionMode && style == LayerRenderStyle::Solid && sectorLayer == viewLayer;
}

//
// One vertical stretch of a Sector level's side wall that the viewport draws.
//
// World units, y0 the lower end and y1 the upper, so the pair reads the same
// way up as the Sector's own level spans rather than in flipped screen space.
//
struct WallSpan
{
	float y0;
	float y1;

	[[nodiscard]] bool empty() const { return y1 - y0 <= 0.0f; }
};

//
// The world-y of one Sector level's floor: the Sector's own base plus every level
// under it. Level heights differ between Locations - a Corridor level is
// CORE_CORRIDOR_HEIGHT, a Room level stands taller - so a level's height is not
// its row, and the two cannot be used interchangeably here.
//
inline float levelFloorY(core::Sector const& sector, uint32_t levelIndex)
{
	core::Vector2 lo, hi;
	sector.getBounds(lo, hi);

	float y = lo.y;

	for (uint32_t d = 0; d < levelIndex && d < sector.getLevelsHigh(); ++d)
	{
		y += sector.getLevelHeight(d);
	}

	return y;
}

//
// The Location, if any, sharing one Sector's side boundary on a given level.
//
// Looked up geometrically - the cell just outside the Sector's own footprint on
// the level's global row - because that cell is the boundary the wall would
// stand in.
//
inline std::shared_ptr<const core::Sector> boundarySector(
	core::World const& world,
	core::Sector const& sector,
	uint32_t levelIndex,
	int side)
{
	int const boundaryX = side == CORE_SIDE_LEFT
		? static_cast<int>(sector.getCellX()) - 1
		: static_cast<int>(sector.getCellX()) + static_cast<int>(sector.getCellsWide());
	int const globalY = static_cast<int>(sector.getCellY()) + static_cast<int>(levelIndex);

	if (boundaryX < 0 || boundaryX >= static_cast<int>(world.getCellsWide())
		|| globalY < 0 || globalY >= static_cast<int>(world.getLevelsHigh()))
	{
		return nullptr;
	}

	return world.getSectorAtPosition(sector.getLayerIndex(),
		static_cast<float>(boundaryX) + 0.5f, static_cast<float>(globalY) + 0.5f);
}

//
// The stretch of a boundary the viewed Layer leaves open, found from the far
// side of it.
//
// The wireframe overlay x-rays the Layer directly behind the selection. A
// behind-Layer wall drawn straight across an opening the selected Layer has
// made reads as a wall across that opening, and the connection the player just
// authored disappears. The caller subtracts this stretch from the behind wall;
// only the intersection goes, the rest of the wall still stands.
//
// Returns false when the Sector is not behind the viewed Layer, when the Layer
// in front has no Location pair straddling this boundary, or when that pair is
// not open.
//
inline bool frontLayerOpening(std::shared_ptr<const core::World> const& world,
	int viewLayer, core::Sector const& sector, uint32_t levelIndex, int side,
	float& openFrom, float& openTo)
{
	openFrom = openTo = 0.0f;

	if (!world || viewLayer < 0 || sector.getLayerIndex() <= static_cast<uint32_t>(viewLayer))
	{
		return false;
	}

	// The boundary line this Sector's wall stands on, and the two cells of the
	// viewed Layer which straddle it.
	int const boundaryX = side == CORE_SIDE_LEFT
		? static_cast<int>(sector.getCellX())
		: static_cast<int>(sector.getCellX()) + static_cast<int>(sector.getCellsWide());
	int const globalY = static_cast<int>(sector.getCellY()) + static_cast<int>(levelIndex);

	if (boundaryX <= 0 || boundaryX >= static_cast<int>(world->getCellsWide())
		|| globalY < 0 || globalY >= static_cast<int>(world->getLevelsHigh()))
	{
		return false;
	}

	auto const left = world->getSectorAtPosition(static_cast<uint32_t>(viewLayer),
		static_cast<float>(boundaryX) - 0.5f, static_cast<float>(globalY) + 0.5f);
	auto const right = world->getSectorAtPosition(static_cast<uint32_t>(viewLayer),
		static_cast<float>(boundaryX) + 0.5f, static_cast<float>(globalY) + 0.5f);

	if (!left || !right)
	{
		return false;
	}

	auto const leftLevel = globalY - static_cast<int>(left->getCellY());
	auto const rightLevel = globalY - static_cast<int>(right->getCellY());

	if (leftLevel < 0 || leftLevel >= static_cast<int>(left->getLevelsHigh())
		|| rightLevel < 0 || rightLevel >= static_cast<int>(right->getLevelsHigh()))
	{
		return false;
	}

	if (left->getEndType(static_cast<uint32_t>(leftLevel), CORE_SIDE_RIGHT) != core::SectorEndType::None
		|| right->getEndType(static_cast<uint32_t>(rightLevel), CORE_SIDE_LEFT) != core::SectorEndType::None)
	{
		return false;
	}

	auto const leftFloor = levelFloorY(*left, static_cast<uint32_t>(leftLevel));
	auto const rightFloor = levelFloorY(*right, static_cast<uint32_t>(rightLevel));

	openFrom = std::max(leftFloor, rightFloor);
	openTo = std::min(leftFloor + left->getLevelHeight(static_cast<uint32_t>(leftLevel)),
		rightFloor + right->getLevelHeight(static_cast<uint32_t>(rightLevel)));

	return openTo > openFrom;
}

//
// The part of one span left after another is taken out of it.
//
inline std::vector<WallSpan> subtractSpan(WallSpan const& span, float from, float to)
{
	std::vector<WallSpan> spans;

	if (span.empty() || to <= span.y0 || from >= span.y1)
	{
		if (!span.empty()) spans.push_back(span);
		return spans;
	}

	if (auto const below = WallSpan{ span.y0, std::max(std::min(from, span.y1), span.y0) }; !below.empty())
	{
		spans.push_back(below);
	}

	if (auto const above = WallSpan{ std::min(std::max(to, span.y0), span.y1), span.y1 }; !above.empty())
	{
		spans.push_back(above);
	}

	return spans;
}

//
// The stretches of one Location level's side wall the viewport should draw.
//
// A closed (Wall) end draws its whole level; a BulkheadDoor end draws no plain
// wall line, exactly as before.
//
// An open end removes only the boundary it actually shares: the vertical
// overlap between this Sector's level span and the neighbouring Sector's level
// span at the same global row. A Room level standing 1.0 tall beside a
// 0.7-tall Corridor therefore keeps the 0.3 of wall above the Corridor's
// ceiling - the opening is the intersection, never the whole level. Two levels
// of equal height overlap completely, so their shared wall vanishes entirely
// and the two Locations read as connected.
//
// With no World to ask, or no Sector on the boundary, an open end removes
// its whole level: with nothing to intersect there is no shared boundary to
// leave standing, and this is how an open end has always rendered.
//
// `viewLayer` is the Layer the viewport is showing. A Sector drawn from behind
// it - the wireframe overlay - also loses the stretch that the viewed Layer's
// own opening covers, so a behind-Layer wall cannot masquerade as the wall the
// player just removed. Pass a negative viewLayer to skip that second cut.
//
inline std::vector<WallSpan> wallSpansToDraw(std::shared_ptr<const core::World> const& world,
	core::Sector const& sector, uint32_t levelIndex, int side, int viewLayer = -1)
{
	std::vector<WallSpan> spans;

	if (levelIndex >= sector.getLevelsHigh())
	{
		return spans;
	}

	auto const y0 = levelFloorY(sector, levelIndex);
	auto const y1 = y0 + sector.getLevelHeight(levelIndex);

	auto const endType = sector.getEndType(levelIndex, side);

	if (endType != core::SectorEndType::None)
	{
		if (endType == core::SectorEndType::Wall)
		{
			spans.push_back({ y0, y1 });
		}
	}
	else
	{
		auto const neighbour = world
			? boundarySector(*world, sector, levelIndex, side)
			: nullptr;

		if (!neighbour)
		{
			return spans;
		}

		auto const neighbourLevel = static_cast<int>(levelIndex) + static_cast<int>(sector.getCellY())
			- static_cast<int>(neighbour->getCellY());
		if (neighbourLevel < 0 || neighbourLevel >= static_cast<int>(neighbour->getLevelsHigh()))
		{
			return spans;
		}

		auto const neighbourFloor = levelFloorY(*neighbour, static_cast<uint32_t>(neighbourLevel));
		auto const neighbourTop = neighbourFloor
			+ neighbour->getLevelHeight(static_cast<uint32_t>(neighbourLevel));

		auto const openFrom = std::max(y0, neighbourFloor);
		auto const openTo = std::min(y1, neighbourTop);

		// Levels on the same global row always meet, but a neighbour that somehow
		// misses this one leaves no opening to cut, and the wall stands whole.
		if (openTo <= openFrom)
		{
			spans.push_back({ y0, y1 });
		}
		else
		{
			if (auto const below = WallSpan{ y0, openFrom }; !below.empty())
			{
				spans.push_back(below);
			}

			if (auto const above = WallSpan{ openTo, y1 }; !above.empty())
			{
				spans.push_back(above);
			}
		}
	}

	float openingFrom{ 0.0f }, openingTo{ 0.0f };
	if (!frontLayerOpening(world, viewLayer, sector, levelIndex, side, openingFrom, openingTo))
	{
		return spans;
	}

	std::vector<WallSpan> trimmed;
	for (auto const& span : spans)
	{
		for (auto const& kept : subtractSpan(span, openingFrom, openingTo))
		{
			trimmed.push_back(kept);
		}
	}

	return trimmed;
}

// Transit geometry is drawn by the selected Layer's passes. The wireframe overlay
// contributes outlines, never the Transit's own filled geometry, which would
// otherwise paint over the selected Layer.
inline bool shouldRenderLadderGeometry(LayerRenderStyle style)
{
	return isDrawnSolid(style);
}

//
// The world-space rectangle through which one Transit on the Layer behind the
// selection may be drawn.
//
// A Transit never fills the selected Layer. Its solid body is visible only where
// the selected Layer's Locations open onto it, which is the clipping the
// two-layer Fore/Back renderer already applied to Back-layer Transits. The
// wireframe overlay outlines the Transit's whole footprint over the selection;
// clipping governs the fill, not the overlay.
//
struct TransitAperture
{
	core::Vector2 min;
	core::Vector2 max;

	// The Location on the selected Layer that this aperture opens through. Null
	// when the aperture belongs to the Transit alone, as a Stairwell level does.
	std::shared_ptr<const core::Sector> location;
};

// True while a Transit drawn in this style must be clipped to its apertures. Only
// the aperture pass clips: the selected Layer draws its own Transits whole, and
// the wireframe overlay outlines the whole Layer behind over the selection,
// which is the point of an x-ray overlay.
inline constexpr bool shouldClipTransitToApertures(LayerRenderStyle style)
{
	return style == LayerRenderStyle::Aperture;
}

inline bool shouldRenderStairwellGeometry(LayerRenderStyle style)
{
	return isDrawnSolid(style);
}

inline bool shouldRenderForeContentAfterTransit(core::SectorType transitType)
{
	return transitType == core::SectorType::Ladder;
}

inline bool shouldRenderLadderGeometryAfterSectorContents()
{
	return false;
}

inline bool shouldRenderStaircaseAfterSector(core::SectorType sectorType)
{
	// The selected Layer's Transit pass runs after Locations. Both Corridors and
	// Rooms are Location sectors and may expose part of a Staircase.
	return sectorType == core::SectorType::Location;
}

// The apertures that one Transit on the Layer directly behind `viewLayer` has on
// `viewLayer`. Each Transit type exposes its own aperture geometry:
//
//   Ladder     the bounds of each landing Location
//   Lift       the doorway rectangle at each landing
//   Shuttle    the rectangle of each Door on the selected Layer which opens
//              onto it, so the aperture is the doorway the player can see
//   Stairwell  the doorway rectangle of each level
//   Staircase  the bounds of every Location on the selected Layer
//
// `viewLocations` is the selected Layer's Sectors, already culled to the
// viewport. A Transit that does not sit directly behind `viewLayer`, or that has
// no landing among those Locations, exposes no aperture and so is not drawn at
// all.
inline std::vector<TransitAperture> transitApertures(
	std::shared_ptr<const core::Sector> const& transit,
	uint32_t viewLayer,
	std::vector<std::shared_ptr<const core::Sector>> const& viewLocations)
{
	std::vector<TransitAperture> apertures;

	if (!transit || transit->getLayerIndex() != core::layerBehind(viewLayer))
	{
		return apertures;
	}

	auto landsOnViewLayer = [viewLayer](std::shared_ptr<const core::Sector> const& sector)
	{
		return sector && sector->getLayerIndex() == viewLayer;
	};

	auto addLocationAperture = [&](std::shared_ptr<const core::Sector> const& location)
	{
		if (!landsOnViewLayer(location))
		{
			return;
		}

		TransitAperture aperture;
		aperture.location = location;
		location->getBounds(aperture.min, aperture.max);
		apertures.push_back(aperture);
	};

	auto addDoorwayAperture = [&](std::shared_ptr<const core::Sector> const& location,
		float centerX, float y, float width, float height)
	{
		if (!landsOnViewLayer(location))
		{
			return;
		}

		apertures.push_back({
			{ centerX - width * 0.5f, y },
			{ centerX + width * 0.5f, y + height },
			location });
	};

	switch (transit->getType())
	{
	case core::SectorType::Ladder:
		if (auto const* ladder = dynamic_cast<core::LadderTransit const*>(transit.get()))
		{
			for (uint32_t stop = 0; stop < ladder->getNumStops(); ++stop)
			{
				addLocationAperture(ladder->getStop(stop).sector);
			}
		}
		break;

	case core::SectorType::Lift:
		if (auto const* lift = dynamic_cast<core::LiftTransit const*>(transit.get()))
		{
			for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
			{
				auto const& landing = lift->getStop(stop);
				if (!landsOnViewLayer(landing.sector))
				{
					continue;
				}

				auto const cellX = (float)((int)landing.sector->getCellX() + landing.sectorOffsetX);
				auto const cellY = (float)((int)landing.sector->getCellY() + landing.sectorOffsetY);
				auto const width = (float)lift->getCellsWide()
					- CORE_LIFT_DOORWAY_BORDER * 2.0f;

				addDoorwayAperture(landing.sector, cellX + (float)lift->getCellsWide() * 0.5f,
					cellY, width, CORE_LIFT_DOORWAY_HEIGHT);
			}
		}
		break;

	case core::SectorType::Shuttle:
		// A Shuttle is seen through the thresholds it actually owns: the Doors
		// authored on the selected Layer which open onto it. Deriving the aperture
		// from the stop's origin cell instead put it one cell in front of the
		// carriage's first door and missed every other door on that carriage, so
		// the solid pass painted carriage over floor with no doorway in front of
		// it. A carriage may have any number of doors, and each is its own
		// aperture.
		for (auto const& location : viewLocations)
		{
			if (!landsOnViewLayer(location)
				|| !std::dynamic_pointer_cast<const core::Location>(location))
			{
				continue;
			}

			for (uint32_t index = 0; index < location->getNumObjects(); ++index)
			{
				auto const object = location->getObject(index);
				if (!object || object->getObjectType() != core::SectorObjectType::Door)
				{
					continue;
				}

				auto const door = std::static_pointer_cast<const core::DoorSectorObject>(
					object)->getDoor();
				if (!door || door->getBackSector() != transit)
				{
					continue;
				}

				core::Vector2 lo, hi;
				door->getFullShape(lo, hi);
				apertures.push_back({ { lo.x, lo.y }, { hi.x, hi.y }, location });
			}
		}
		break;

	case core::SectorType::Stairwell:
		if (auto const* stairwell = dynamic_cast<core::StairwellTransit const*>(transit.get()))
		{
			for (uint32_t level = 0; level < stairwell->getLevelsHigh(); ++level)
			{
				// A level opens at the shaft's own column rather than at a landing's
				// cell, so the aperture carries no Location of its own.
				apertures.push_back({
					{ (float)stairwell->getCellX() + 1.0f
						- CORE_STAIRWELL_DOORWAY_WIDTH * 0.5f,
						(float)stairwell->getCellY() + (float)level },
					{ (float)stairwell->getCellX() + 1.0f
						+ CORE_STAIRWELL_DOORWAY_WIDTH * 0.5f,
						(float)stairwell->getCellY() + (float)level + CORE_STAIRWELL_DOORWAY_HEIGHT },
					level < stairwell->getNumStops() ? stairwell->getStop(level).sector : nullptr });
			}
		}
		break;

	case core::SectorType::Staircase:
		for (auto const& location : viewLocations)
		{
			if (!std::dynamic_pointer_cast<const core::Location>(location)
				|| !shouldRenderStaircaseAfterSector(location->getType()))
			{
				continue;
			}

			addLocationAperture(location);
		}
		break;

	default:
		break;
	}

	return apertures;
}

// Occupants follow the same rule as their Sector's geometry. The wireframe overlay
// may outline its Sectors, but must not expose the Agents inside them.
inline bool shouldRenderSectorAgents(core::SectorType /* sectorType */, LayerRenderStyle style)
{
	return isDrawnSolid(style);
}

void renderGraph(std::shared_ptr<const core::Graph> graph, std::shared_ptr<const core::World> world);

// Ordinary Agents use their effective inherited Colour (or the editor
// fallback); selection always wins with its fixed gold highlight.
ImU32 agentRenderColour(core::Agent const& agent, bool selected);
void renderAgent(core::Agent const* agent, ImDrawList* drawList);

void renderWorld(std::shared_ptr<const core::World> world);

//
// Publishes the World rendered by the viewport.
//
// renderWorld() calls this on entry. The Sector-level passes read the
// World back from here because their call chain carries no World pointer,
// which is the same route #37 opened for the multi-Background aperture
// composite. An open wall needs it too: the stretch of wall a removed end
// takes away is the overlap with the Sector on the far side of that boundary.
//
void setRenderWorld(std::shared_ptr<const core::World> world);

//
// The Sectors of one Layer that the current viewport sees: culled from the
// visible world origin (-xOffset, -yOffset) over the viewport's size. Every
// render pass culls through here so the passes cannot disagree (#58). Declared
// here so the headless viewport-culling check can exercise the real bounds.
//
std::vector<std::shared_ptr<const core::Sector>> viewportSectors(
	std::shared_ptr<const core::World> const& world, uint32_t layer);

//
// Draws every Sector one Layer contributes for a single viewport pass, culled
// to the current viewport. Declared here so the headless viewport-culling
// check (#58) can exercise the real pass against a live ImDrawList.
//
void renderSectors(std::shared_ptr<const core::World> world, uint32_t layer,
	LayerRenderStyle style, ImDrawList* drawList);

//
// Draws one Sector for a single viewport pass: its surface fill, its
// thresholds and objects, and its Agents, in the order the viewport paints
// them. Declared here so the headless draw-order check (#49) can exercise
// the real renderSector() draw-call order rather than a model of it.
//
void renderSector(std::shared_ptr<const core::Sector> sector, uint32_t layer,
	LayerRenderStyle style, bool renderEdges, ImColor colour, ImDrawList* drawList);
