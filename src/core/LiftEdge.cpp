#include <cassert>

#include "core/Defines.h"
#include "core/LiftEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	LiftEdge
	--------

	Implementation of Edge for the Vertices at either end of a Lift.
	*/

	using namespace std;

	LiftEdge::LiftEdge(shared_ptr<Lift> lift)
		: Edge(EdgeType::Lift)
		, mLift(lift)
	{
	}

	LiftEdge::LiftEdge(uint32_t id, shared_ptr<Lift> lift)
		: Edge(id, EdgeType::Lift)
		, mLift(lift)
	{
	}

	shared_ptr<Edge> LiftEdge::copyWithoutVertices()
	{
		return make_shared<LiftEdge>(getId(), mLift);
	}

	string LiftEdge::getDescription() const
	{
		return format("Lift edge for {}", mLift->getDescription());
	}

	bool LiftEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return true;
	}

	EdgeTraversalRequestResult LiftEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	float LiftEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
	}

	TraversalResourceId LiftEdge::getTraversalResourceId() const
	{
		return mLift->getTraversalResourceId();
	}

} // core