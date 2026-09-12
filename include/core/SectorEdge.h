#pragma once

#include "core/Edge.h"


namespace core
{
	class Agent;

	class SectorEdge : public Edge
	{
	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		SectorEdge(uint32_t id);
	
		SectorEdge();

		std::shared_ptr<Edge> copyWithoutVertices() override;

		std::string getDescription() const override;

		bool isTraversable(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		float getWeight(std::shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const override;
	};

} // core
