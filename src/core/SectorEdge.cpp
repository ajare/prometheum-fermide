#include <cassert>

#include "core/Defines.h"
#include "core/SectorEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"

namespace core
{

	using namespace std;

	/*
	SectorEdge
	----------

	Implementation of Edge for the Vertices in a Sector.  This is a default Edge which
	will typically be used for moving between Vertices in a Location when no more appropriate
	Edge implementation is available.
	*/

	SectorEdge::SectorEdge()
		: Edge(EdgeType::Location)
	{
	}

	SectorEdge::SectorEdge(uint32_t id)
		: Edge(id, EdgeType::Location)
	{
	}

	shared_ptr<Edge> SectorEdge::copyWithoutVertices()
	{
		return make_shared<SectorEdge>(getId());
	}

	string SectorEdge::getDescription() const
	{
		return "Sector edge";
	}

	bool SectorEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return true;
	}

	EdgeTraversalRequestResult SectorEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	float SectorEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		CORE_VAR_UNUSED(edgeVisible);

		auto distance = getLength();

		if (distance == 0.0f)
		{
			return 0.0f;
		}

		// Time in seconds
		return max(distance / agent->getWalkSpeed(), CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME);
	}

} // core