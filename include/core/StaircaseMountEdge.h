#pragma once

#include "core/Edge.h"
#include "core/Staircase.h"


namespace core
{
	class Agent;

	class StaircaseMountEdge : public Edge
	{
		std::shared_ptr<Staircase> mStaircase;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		StaircaseMountEdge(uint32_t id, std::shared_ptr<Staircase> staircase);

		StaircaseMountEdge(std::shared_ptr<Staircase> staircase);

		std::shared_ptr<Staircase> getStaircase() const;

		std::shared_ptr<Edge> copyWithoutVertices() override;

		std::string getDescription() const override;

		bool isTraversable(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		float getWeight(std::shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const override;
	};

} // core
