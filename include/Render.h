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

void renderGraph(std::shared_ptr<const core::Graph> graph, std::shared_ptr<const core::Building> building);

void renderBuilding(std::shared_ptr<const core::Building> building);
