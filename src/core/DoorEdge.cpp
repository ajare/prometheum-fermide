#include <cassert>

#include "core/Defines.h"
#include "core/DoorEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	DoorEdge
	--------

	Implementation of Edge for the Vertices on either side of a Door.  These Vertices
	will be on different Layers - Fore and Back respectively.
	*/

	using namespace std;

	DoorEdge::DoorEdge(shared_ptr<Door> door)
		: Edge(EdgeType::Door)
		, mDoor(door)
	{
	}

	DoorEdge::DoorEdge(uint32_t id, shared_ptr<Door> door)
		: Edge(id, EdgeType::Door)
		, mDoor(door)
	{
	}

	shared_ptr<Edge> DoorEdge::copyWithoutVertices()
	{
		return make_shared<DoorEdge>(getId(), mDoor);
	}

	string DoorEdge::getDescription() const
	{
		return format("Door edge for {}", mDoor->getDescription());
	}

	bool DoorEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return mDoor->isOpen();
	}

	EdgeTraversalRequestResult DoorEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return mDoor->open() ? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	float DoorEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		// Time in seconds.  As we are crossing Layers, the distance between Vertices is essentially zero.

		float preparation = edgeVisible && mDoor->isOpen()
			? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : mDoor->getOpenCloseTime();
		if (!agent) return preparation;
		auto targetSector = targetVertex && targetVertex->getSector()
			? SectorId{ (uint64_t)targetVertex->getSector()->getIndex() + 1 } : SectorId{};
		auto sourceSector = getVertex(0) && SectorId{ (uint64_t)getVertex(0)->getSector()->getIndex() + 1 } != targetSector
			? SectorId{ (uint64_t)getVertex(0)->getSector()->getIndex() + 1 }
			: getVertex(1) ? SectorId{ (uint64_t)getVertex(1)->getSector()->getIndex() + 1 } : SectorId{};
		return preparation + agent->estimateTraversalDelay(getTraversalResourceId(), sourceSector);
	}

	TraversalResourceId DoorEdge::getTraversalResourceId() const
	{
		return mDoor->getTraversalResourceId();
	}

} // core