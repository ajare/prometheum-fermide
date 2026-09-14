#include <cassert>

#include "core/Defines.h"
#include "core/GapEdge.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	GapEdge
	-------

	Implementation of Edge for the Vertices on either side of a space in the air.  This can
	happen when there a Walkways on either side of a Cell, but nothing between them, or when
	a Location leads to another Location at a higher level but there is no Walkway for it to
	connect to.

	Gaps are essentially not crossable, but are included as Edges with a very high weight in
	case that some means of crossing them (jumping?) might be added.  If an Agent receives a
	path with a large weight, it needs to decide whether to ignore it completely and give up
	on where it is going, or use that path in some vain hope that something will change by the
	time it gets to the gap.
	*/

	GapEdge::GapEdge()
		: Edge(EdgeType::Gap)
	{
	}

	GapEdge::GapEdge(uint32_t id)
		: Edge(id, EdgeType::Gap)
	{
	}

	shared_ptr<Edge> GapEdge::copyWithoutVertices()
	{
		return make_shared<GapEdge>(getId());
	}

	string GapEdge::getDescription() const
	{
		return "Gap edge";
	}

	bool GapEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return false;
	}

	EdgeTraversalRequestResult GapEdge::requestTraversal(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return EdgeTraversalRequestResult::Failed;
	}

	float GapEdge::getWeight(shared_ptr<const Vertex> /* targetVertex */, Agent const* /* agent */, bool /* edgeVisible */) const
	{
		return CORE_GRAPH_EDGE_UNTRAVERSABLE;
	}

} // core