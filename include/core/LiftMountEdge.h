#pragma once

#include "core/Edge.h"
#include "core/Lift.h"


namespace core
{
	class Agent;

	class LiftMountEdge : public Edge
	{
		std::shared_ptr<Lift> mLift;

	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		LiftMountEdge(uint32_t id, std::shared_ptr<Lift> lift);

		LiftMountEdge(std::shared_ptr<Lift> lift);

		std::shared_ptr<Lift> getLift() const;

		std::shared_ptr<Edge> copyWithoutVertices() override;

		std::string getDescription() const override;

		bool isTraversable(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const override;

		float getWeight(std::shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const override;

		TraversalResourceId getTraversalResourceId() const override;
	};

} // core
