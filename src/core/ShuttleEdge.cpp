#include <cassert>

#include "core/Defines.h"
#include "core/ShuttleEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	ShuttleEdge
	-----------

	Implementation of Edge for the Vertices at either end of a Shuttle.
	*/

	using namespace std;

	ShuttleEdge::ShuttleEdge(shared_ptr<Shuttle> shuttle)
		: Edge(EdgeType::Shuttle)
		, mShuttle(shuttle)
	{
	}

	ShuttleEdge::ShuttleEdge(uint32_t id, shared_ptr<Shuttle> shuttle)
		: Edge(id, EdgeType::Shuttle)
		, mShuttle(shuttle)
	{
	}

	shared_ptr<Edge> ShuttleEdge::copyWithoutVertices()
	{
		return make_shared<ShuttleEdge>(getId(), mShuttle);
	}

	string ShuttleEdge::getDescription() const
	{
		return format("Shuttle edge for {}", mShuttle->getDescription());
	}

	bool ShuttleEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return true;
	}

	EdgeTraversalRequestResult ShuttleEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	float ShuttleEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		auto resource = getTraversalResourceId();
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
			+ (agent && resource ? agent->estimateTraversalDelay(resource,
				SectorId{ (uint64_t)targetVertex->getSector()->getIndex() + 1 }) : 0.0f);
	}

	TraversalResourceId ShuttleEdge::getTraversalResourceId() const
	{
		return mShuttle ? mShuttle->getTraversalResourceId() : TraversalResourceId{};
	}

} // core