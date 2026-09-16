#include <cmath>
#include <format>
#include <limits>

#include "core/Defines.h"
#include "core/StaircaseEdge.h"
#include "core/Vertex.h"

namespace core
{
	using namespace std;

	StaircaseEdge::StaircaseEdge(shared_ptr<Staircase> staircase)
		: Edge(EdgeType::Staircase), mStaircase(std::move(staircase)) {}

	StaircaseEdge::StaircaseEdge(uint32_t id, shared_ptr<Staircase> staircase)
		: Edge(id, EdgeType::Staircase), mStaircase(std::move(staircase)) {}

	string StaircaseEdge::getDescription() const
	{
		return format("Staircase edge for {}", mStaircase->getDescription());
	}

	shared_ptr<Edge> StaircaseEdge::copyWithoutVertices()
	{
		return make_shared<StaircaseEdge>(getId(), mStaircase);
	}

	bool StaircaseEdge::isTraversable(shared_ptr<const Vertex> targetVertex,
		shared_ptr<const Agent>) const
	{
		if (!mStaircase->isEscalator()) return true;
		auto sourceVertex = getOtherVertex(targetVertex);
		bool const movingUp = targetVertex->getPosition().y > sourceVertex->getPosition().y;
		return movingUp == (mStaircase->getSpeed() > 0.0f);
	}

	EdgeTraversalRequestResult StaircaseEdge::requestTraversal(
		shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	float StaircaseEdge::getWeight(shared_ptr<const Vertex> targetVertex,
		Agent const*, bool) const
	{
		if (!mStaircase->isEscalator()) return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
		auto sourceVertex = getOtherVertex(targetVertex);
		bool const movingUp = targetVertex->getPosition().y > sourceVertex->getPosition().y;
		if (movingUp != (mStaircase->getSpeed() > 0.0f))
			return numeric_limits<float>::infinity();
		return getLength() / abs(mStaircase->getSpeed());
	}

	float StaircaseEdge::getTraversalSpeed(Agent const*) const
	{
		return abs(mStaircase->getSpeed());
	}
}
