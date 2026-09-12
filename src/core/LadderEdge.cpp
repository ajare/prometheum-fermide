#include <cassert>

#include "core/Defines.h"
#include "core/LadderEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	LadderEdge
	----------

	Implementation of Edge for the Vertices at either end of a Ladder.
	*/

	using namespace std;

	LadderEdge::LadderEdge(shared_ptr<Ladder> ladder)
		: Edge(EdgeType::Ladder)
		, mLadder(ladder)
	{
	}

	LadderEdge::LadderEdge(uint32_t id, shared_ptr<Ladder> ladder)
		: Edge(id, EdgeType::Ladder)
		, mLadder(ladder)
	{
	}

	shared_ptr<Edge> LadderEdge::copyWithoutVertices()
	{
		return make_shared<LadderEdge>(getId(), mLadder);
	}

	string LadderEdge::getDescription() const
	{
		return format("Ladder edge for {}", mLadder->getDescription());
	}

	bool LadderEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		// TODO: this will depend on whether there are any Agents in the way.
		return true;
	}

	EdgeTraversalRequestResult LadderEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		// TODO: this will depend on whether there are any Agents in the way.
		return EdgeTraversalRequestResult::OK;
	}

	float LadderEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		auto distance = getLength();
		float traverseTime = distance == 0.0f ? 0.0f : agent->getClimbSpeed() / distance;

		// If Edge isn't visible, then assume we have to wait for the Ladder to extend.
		if (!edgeVisible || !mLadder->isExtended())
		{
			// TODO: if not visible, could hedge by assuming a percentage chance of it
			//       being extended, and multiply the extend time by that. 
			traverseTime += mLadder->getExtendRetractTime();
		}

		return max(traverseTime, CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME);
	}

} // core