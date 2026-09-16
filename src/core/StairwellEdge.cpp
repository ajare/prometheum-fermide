#include <cassert>

#include "core/Defines.h"
#include "core/StairwellEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	StairwellEdge
	-------------

	Implementation of Edge for the Vertices at either end of a Stairwell.
	*/

	using namespace std;

	StairwellEdge::StairwellEdge(shared_ptr<Stairwell> stairwell)
		: Edge(EdgeType::Stairwell)
		, mStairwell(stairwell)
	{
	}

	StairwellEdge::StairwellEdge(uint32_t id, shared_ptr<Stairwell> stairwell)
		: Edge(id, EdgeType::Stairwell)
		, mStairwell(stairwell)
	{
	}

	shared_ptr<Edge> StairwellEdge::copyWithoutVertices()
	{
		return make_shared<StairwellEdge>(getId(), mStairwell);
	}

	string StairwellEdge::getDescription() const
	{
		return format("Stairwell edge for {}", mStairwell->getDescription());
	}

	bool StairwellEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return true;
	}

	EdgeTraversalRequestResult StairwellEdge::requestTraversal(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	float StairwellEdge::getWeight(shared_ptr<const Vertex> /* targetVertex */, Agent const* /* agent */, bool /* edgeVisible */) const
	{
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
	}

	TraversalResourceId StairwellEdge::getTraversalResourceId() const
	{
		return mStairwell->getTraversalResourceId();
	}

} // core