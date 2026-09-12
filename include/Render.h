#pragma once

#include <memory>

#include "core/Building.h"
#include "core/Graph.h"


#define RENDER_VERTEX_SIZE						5
#define RENDER_INTER_LAYER_EDGE_SIZE			11

void renderGraph(std::shared_ptr<const core::Graph> graph, std::shared_ptr<const core::Building> building);

void renderBuilding(std::shared_ptr<const core::Building> building);
