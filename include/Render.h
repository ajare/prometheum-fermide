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
	return visibleLayer || layer != CORE_LAYER_BACK;
}

inline bool shouldRenderStaircaseGeometry(int layer, bool visibleLayer)
{
	return visibleLayer || layer != CORE_LAYER_BACK;
}

inline bool shouldRenderForeContentAfterTransit(core::SectorType transitType)
{
	return transitType == core::SectorType::Ladder;
}

inline bool shouldRenderLadderGeometryAfterSectorContents()
{
	return false;
}

inline bool shouldRenderSectorAgents(core::SectorType sectorType, int layer, bool visibleLayer)
{
	if (sectorType == core::SectorType::Ladder)
		return shouldRenderLadderGeometry(layer, visibleLayer);
	if (sectorType == core::SectorType::Shuttle)
		return visibleLayer || layer != CORE_LAYER_BACK;
	return true;
}

void renderGraph(std::shared_ptr<const core::Graph> graph, std::shared_ptr<const core::Building> building);

void renderBuilding(std::shared_ptr<const core::Building> building);
