#include <cassert>

#include "core/Defines.h"
#include "core/StaircaseMountEdge.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	StaircaseMountEdge
	------------------

	This Edge connects a StaircaseLocationVertex to a StaircaseVertex.  It is traversed immediately, assuming
	that an Agent is able to use the Staircase.  Its vertices may or may not be in the same Sector, but will
	be in the same Layer.
	*/

	StaircaseMountEdge::StaircaseMountEdge(shared_ptr<Staircase> staircase)
		: Edge(EdgeType::StaircaseMount)
		, mStaircase(staircase)
	{
	}

	StaircaseMountEdge::StaircaseMountEdge(uint32_t id, shared_ptr<Staircase> staircase)
		: Edge(id, EdgeType::StaircaseMount)
		, mStaircase(staircase)
	{
	}

	shared_ptr<Staircase> StaircaseMountEdge::getStaircase() const
	{
		return mStaircase;
	}

	shared_ptr<Edge> StaircaseMountEdge::copyWithoutVertices()
	{
		return make_shared<StaircaseMountEdge>(getId(), getStaircase());
	}

	string StaircaseMountEdge::getDescription() const
	{
		return format("StaircaseMount edge for {}", mStaircase->getDescription());
	}

	bool StaircaseMountEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		// TODO: see if any Agents are on the Staircase

		return true;
	}

	EdgeTraversalRequestResult StaircaseMountEdge::requestTraversal(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	float StaircaseMountEdge::getWeight(shared_ptr<const Vertex> /* targetVertex */, Agent const* /* agent */, bool /* edgeVisible */) const
	{
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
	}

} // core