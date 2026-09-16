#pragma once

#include "core/Edge.h"
#include "core/Staircase.h"

namespace core
{
	class StaircaseEdge : public Edge
	{
		std::shared_ptr<Staircase> mStaircase;
	public:
		StaircaseEdge(std::shared_ptr<Staircase> staircase);
		StaircaseEdge(uint32_t id, std::shared_ptr<Staircase> staircase);
		[[nodiscard]] std::string getDescription() const override;
		[[nodiscard]] std::shared_ptr<Edge> copyWithoutVertices() override;
		[[nodiscard]] bool isTraversable(std::shared_ptr<const Vertex>, std::shared_ptr<const Agent>) const override;
		EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex>, std::shared_ptr<const Agent>) const override;
		[[nodiscard]] float getWeight(std::shared_ptr<const Vertex>, Agent const*, bool) const override;
	};
}
