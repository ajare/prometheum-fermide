#pragma once

#include <memory>
#include <vector>

#include "core/Edge.h"
#include "core/Vertex.h"


namespace core
{
	struct PathNode
	{
		std::shared_ptr<const Edge> edge;
		std::shared_ptr<const Vertex> targetVertex;
		float edgeWeight;
	};

	struct Path
	{
		std::vector<PathNode> nodes;
	};

} // core
