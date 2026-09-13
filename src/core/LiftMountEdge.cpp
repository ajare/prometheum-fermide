#include <cassert>

#include "core/Defines.h"
#include "core/LiftMountEdge.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	LiftMountEdge
	-------------

	This Edge connects a regular SectorObjectVertex to a LiftVertex.  It is traversed immediately, assuming
	that an Agent is able to use the Lift.  Its vertices may or may not be in the same Sector, but will
	be in the same Layer.
	*/

	LiftMountEdge::LiftMountEdge(shared_ptr<Lift> lift)
		: Edge(EdgeType::LiftMount)
		, mLift(lift)
	{
	}

	LiftMountEdge::LiftMountEdge(uint32_t id, shared_ptr<Lift> lift)
		: Edge(id, EdgeType::LiftMount)
		, mLift(lift)
	{
	}

	shared_ptr<Lift> LiftMountEdge::getLift() const
	{
		return mLift;
	}

	shared_ptr<Edge> LiftMountEdge::copyWithoutVertices()
	{
		return make_shared<LiftMountEdge>(getId(), getLift());
	}

	string LiftMountEdge::getDescription() const
	{
		return format("LiftMount edge for {}", mLift->getDescription());
	}

	bool LiftMountEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		// TODO: see if any Agents are on the Lift

		return true;
	}

	EdgeTraversalRequestResult LiftMountEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	float LiftMountEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
	}

	TraversalResourceId LiftMountEdge::getTraversalResourceId() const
	{
		return mLift->getTraversalResourceId();
	}

} // core