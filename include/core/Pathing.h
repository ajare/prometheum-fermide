#pragma once

#include <unordered_map>
#include <memory>

#include "core/Path.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/Edge.h"
#include "core/Vertex.h"
#include "core/PriorityQueue.h"


namespace core
{
	namespace pathing
	{

		std::shared_ptr<Path> findPath(Agent const* agent, Graph const* graph, std::shared_ptr<const Vertex> source, std::shared_ptr<const Vertex> target);

		std::shared_ptr<const Vertex> findNextVertexForVertexInPath(std::shared_ptr<Path> path, Vertex const* vertex, uint32_t index = 0);
	
	} // pathing
} // core
