#include <format>

#include "core/Defines.h"
#include "core/StaircaseEdge.h"

namespace core
{
	using namespace std;
	StaircaseEdge::StaircaseEdge(shared_ptr<Staircase> staircase)
		: Edge(EdgeType::Staircase), mStaircase(std::move(staircase)) {}
	StaircaseEdge::StaircaseEdge(uint32_t id, shared_ptr<Staircase> staircase)
		: Edge(id, EdgeType::Staircase), mStaircase(std::move(staircase)) {}
	string StaircaseEdge::getDescription() const { return format("Staircase edge for {}", mStaircase->getDescription()); }
	shared_ptr<Edge> StaircaseEdge::copyWithoutVertices() { return make_shared<StaircaseEdge>(getId(), mStaircase); }
	bool StaircaseEdge::isTraversable(shared_ptr<const Vertex>, shared_ptr<const Agent>) const { return true; }
	EdgeTraversalRequestResult StaircaseEdge::requestTraversal(shared_ptr<const Vertex>, shared_ptr<const Agent>) const { return EdgeTraversalRequestResult::OK; }
	float StaircaseEdge::getWeight(shared_ptr<const Vertex>, Agent const*, bool) const { return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME; }
}
