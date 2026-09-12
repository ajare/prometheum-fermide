#include <cassert>

#include "core/Defines.h"
#include "core/StaircaseEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	StaircaseEdge
	-------------

	Implementation of Edge for the Vertices at either end of a Staircase.
	*/

	using namespace std;

	StaircaseEdge::StaircaseEdge(shared_ptr<Staircase> staircase)
		: Edge(EdgeType::Staircase)
		, mStaircase(staircase)
	{
	}

	StaircaseEdge::StaircaseEdge(uint32_t id, shared_ptr<Staircase> staircase)
		: Edge(id, EdgeType::Staircase)
		, mStaircase(staircase)
	{
	}

	shared_ptr<Edge> StaircaseEdge::copyWithoutVertices()
	{
		return make_shared<StaircaseEdge>(getId(), mStaircase);
	}

	string StaircaseEdge::getDescription() const
	{
		return format("Staircase edge for {}", mStaircase->getDescription());
	}

	bool StaircaseEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return true;
	}

	EdgeTraversalRequestResult StaircaseEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	float StaircaseEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
	}

} // core