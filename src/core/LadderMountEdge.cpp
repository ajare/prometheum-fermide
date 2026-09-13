#include <cassert>
#include <limits>

#include "core/Defines.h"
#include "core/LadderMountEdge.h"
#include "core/Agent.h"
#include "core/Vertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	LadderMountEdge
	---------------

	This Edge connects a regular SectorObjectVertex to a LadderVertex.  It is traversed immediately, assuming
	that an Agent is able to use the Ladder.  Its vertices may or may not be in the same Sector, but will
	be in the same Layer.
	*/

	LadderMountEdge::LadderMountEdge(shared_ptr<Ladder> ladder)
		: Edge(EdgeType::LadderMount)
		, mLadder(ladder)
	{
	}

	LadderMountEdge::LadderMountEdge(uint32_t id, shared_ptr<Ladder> ladder)
		: Edge(id, EdgeType::LadderMount)
		, mLadder(ladder)
	{
	}

	shared_ptr<Ladder> LadderMountEdge::getLadder() const
	{
		return mLadder;
	}

	shared_ptr<Edge> LadderMountEdge::copyWithoutVertices()
	{
		return make_shared<LadderMountEdge>(getId(), getLadder());
	}

	string LadderMountEdge::getDescription() const
	{
		return format("LadderMount edge for {}", mLadder->getDescription());
	}

	bool LadderMountEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		// TODO: see if any Agents are on the Ladder

		return true;
	}

	EdgeTraversalRequestResult LadderMountEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	float LadderMountEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		if (!mLadder->isExtended())
		{
			auto source = getOtherVertex(targetVertex);
			auto sourceSector = source && source->getSector()
				? SectorId{ (uint64_t)source->getSector()->getIndex() + 1 } : SectorId{};
			if (source && source->getType() != VertexType::Ladder
				&& !mLadder->canPrepareFrom(sourceSector))
				return numeric_limits<float>::infinity();
		}
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
	}

	TraversalResourceId LadderMountEdge::getTraversalResourceId() const
	{
		return mLadder->getTraversalResourceId();
	}

} // core