#pragma once

#include <memory>

#include "core/Building.h"
#include "core/Graph.h"


#define RENDER_VERTEX_SIZE						5
#define RENDER_INTER_LAYER_EDGE_SIZE			11

inline bool shouldRenderLadderGeometry(int layer, bool visibleLayer)
{
	// When Fore is visible, Ladder transits are rendered separately through
	// their Location clip rectangles. Do not redraw the complete Back geometry.
	return visibleLayer || !core::isBackMostLayer((uint32_t)layer);
}

inline bool shouldRenderStairwellGeometry(int layer, bool visibleLayer)
{
	return visibleLayer || !core::isBackMostLayer((uint32_t)layer);
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
	// The Fore-layer transit pass runs after Locations. Both Corridors and Rooms
	// are Location sectors and may expose part of a Staircase.
	return sectorType == core::SectorType::Location;
}

inline bool shouldRenderSectorAgents(core::SectorType sectorType, int layer, bool visibleLayer)
{
	if (sectorType == core::SectorType::Ladder
		|| sectorType == core::SectorType::Stairwell
		|| sectorType == core::SectorType::Staircase)
		return visibleLayer || !core::isBackMostLayer((uint32_t)layer);
	if (sectorType == core::SectorType::Shuttle)
		return visibleLayer || !core::isBackMostLayer((uint32_t)layer);
	return true;
}

void renderGraph(std::shared_ptr<const core::Graph> graph, std::shared_ptr<const core::Building> building);

void renderBuilding(std::shared_ptr<const core::Building> building);
