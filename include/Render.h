#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/Background.h"
#include "core/Building.h"
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
// The colour a clear Window's Aperture pass fills the Sector behind it with.
//
// A Background is seen in its own colour: the glass shows what is actually
// behind it, so the pass is handed the Background's own colour rather than the
// generic back-layer tint. Any other back Sector carries no colour of its own
// and yields std::nullopt, leaving the caller's generic tint in place. Where a
// Window faces several Backgrounds at once, compositing them is a later ticket;
// a single Background behind the Window is all this answers.
//
inline std::optional<core::BackgroundColour> apertureFillColour(core::Sector const& backSector)
{
	if (backSector.getType() != core::SectorType::Background)
	{
		return std::nullopt;
	}

	return static_cast<core::Background const&>(backSector).getColour();
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
	// when the aperture belongs to the Transit alone, as a Stairwell deck does.
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
//   Stairwell  the doorway rectangle of each deck
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
			for (uint32_t deck = 0; deck < stairwell->getDecksHigh(); ++deck)
			{
				// A deck opens at the shaft's own column rather than at a landing's
				// cell, so the aperture carries no Location of its own.
				apertures.push_back({
					{ (float)stairwell->getCellX() + 1.0f
						- CORE_STAIRWELL_DOORWAY_WIDTH * 0.5f,
						(float)stairwell->getCellY() + (float)deck },
					{ (float)stairwell->getCellX() + 1.0f
						+ CORE_STAIRWELL_DOORWAY_WIDTH * 0.5f,
						(float)stairwell->getCellY() + (float)deck + CORE_STAIRWELL_DOORWAY_HEIGHT },
					deck < stairwell->getNumStops() ? stairwell->getStop(deck).sector : nullptr });
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

void renderGraph(std::shared_ptr<const core::Graph> graph, std::shared_ptr<const core::Building> building);

void renderBuilding(std::shared_ptr<const core::Building> building);
