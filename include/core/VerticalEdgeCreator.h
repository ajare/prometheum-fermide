#pragma once

#include "core/Edge.h"


namespace core
{

	// Intended as a base class for Area and Shape, as subclasses of both of these need
	// the ability to create Edges for a Graph.
	class VerticalEdgeCreator
	{
	public:

		VerticalEdgeCreator() = default;

		virtual ~VerticalEdgeCreator() = default;

		[[nodiscard]] virtual std::shared_ptr<Edge> createCrossLevelEdge(std::shared_ptr<VerticalEdgeCreator> edgeCreator) const = 0;
	};

} // core
