#include <format>

#include "core/Defines.h"
#include "core/StaircaseMountEdge.h"

namespace core
{
	using namespace std;
	StaircaseMountEdge::StaircaseMountEdge(shared_ptr<Staircase> staircase)
		: Edge(EdgeType::StaircaseMount), mStaircase(std::move(staircase)) {}
	StaircaseMountEdge::StaircaseMountEdge(uint32_t id, shared_ptr<Staircase> staircase)
		: Edge(id, EdgeType::StaircaseMount), mStaircase(std::move(staircase)) {}
	string StaircaseMountEdge::getDescription() const { return format("Staircase mount edge for {}", mStaircase->getDescription()); }
	shared_ptr<Edge> StaircaseMountEdge::copyWithoutVertices() { return make_shared<StaircaseMountEdge>(getId(), mStaircase); }
	bool StaircaseMountEdge::isTraversable(shared_ptr<const Vertex>, shared_ptr<const Agent>) const { return true; }
	EdgeTraversalRequestResult StaircaseMountEdge::requestTraversal(shared_ptr<const Vertex>, shared_ptr<const Agent>) const { return EdgeTraversalRequestResult::OK; }
	float StaircaseMountEdge::getWeight(shared_ptr<const Vertex>, Agent const*, bool) const { return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME; }
}
