#include <cassert>

#include "core/Defines.h"
#include "core/ForceBridgeEdge.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	ForceBridgeEdge
	---------------

	Implementation of Edge for the Vertices on either side of a ForceBridge.
	*/

	ForceBridgeEdge::ForceBridgeEdge(shared_ptr<ForceBridge> forceBridge)
		: Edge(EdgeType::ForceBridge)
		, mForceBridge(forceBridge)
	{
	}

	ForceBridgeEdge::ForceBridgeEdge(uint32_t id, shared_ptr<ForceBridge> forceBridge)
		: Edge(id, EdgeType::ForceBridge)
		, mForceBridge(forceBridge)
	{
	}

	shared_ptr<Edge> ForceBridgeEdge::copyWithoutVertices()
	{
		return make_shared<ForceBridgeEdge>(getId(), mForceBridge);
	}

	string ForceBridgeEdge::getDescription() const
	{
		return format("ForceBridge edge for {}", mForceBridge->getDescription());
	}

	bool ForceBridgeEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return mForceBridge->isExtended();
	}

	EdgeTraversalRequestResult ForceBridgeEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return mForceBridge->extend() ? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	float ForceBridgeEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		// Weight is time to cross the Edge, plus possibly the time waiting for the ForceBridge to extend.
		auto distance = getLength();
		float traverseTime = distance == 0.0f ? 0.0f : agent->getWalkSpeed() / distance;

		// If Edge isn't visible, then assume we have to wait for the ForceBridge.
		if (!edgeVisible || mForceBridge->isExtended())
		{
			// TODO: if not visible, could hedge by assuming a percentage chance of it
			//       being extended, and multiply the extend time by that.
			traverseTime += mForceBridge->getExtendRetractTime();
		}

		return max(traverseTime, CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME);
	}

} // core