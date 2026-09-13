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
		(void)edgeVisible;
		auto rideTime = getLength() / CORE_LIFT_SPEED;
		auto ride = rideTime > CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
			? rideTime : CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
		if (!agent) return ride;
		auto targetSector = targetVertex && targetVertex->getSector()
			? SectorId{ (uint64_t)targetVertex->getSector()->getIndex() + 1 } : SectorId{};
		auto sourceSector = getVertex(0) && SectorId{ (uint64_t)getVertex(0)->getSector()->getIndex() + 1 } != targetSector
			? SectorId{ (uint64_t)getVertex(0)->getSector()->getIndex() + 1 }
			: getVertex(1) ? SectorId{ (uint64_t)getVertex(1)->getSector()->getIndex() + 1 } : SectorId{};
		return ride + agent->estimateTraversalDelay(getTraversalResourceId(), sourceSector);
	}

	TraversalResourceId LiftEdge::getTraversalResourceId() const
	{
		return mLift->getTraversalResourceId();
	}

} // core