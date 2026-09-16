#include <format>

#include "core/StaircaseVertex.h"

namespace core
{
	using namespace std;
	StaircaseVertex::StaircaseVertex(shared_ptr<Sector> sector, shared_ptr<Staircase> staircase,
		float xOffset, float yOffset)
		: Vertex(VertexType::Staircase, VertexSubType::Staircase, sector, xOffset, yOffset)
		, mStaircase(std::move(staircase)) {}
	StaircaseVertex::StaircaseVertex(uint32_t id, shared_ptr<Sector> sector,
		shared_ptr<Staircase> staircase, float xOffset, float yOffset)
		: Vertex(id, VertexType::Staircase, VertexSubType::Staircase, sector, xOffset, yOffset)
		, mStaircase(std::move(staircase)) {}
	string StaircaseVertex::getDescription() const
	{
		auto pos = getPosition();
		return format("StaircaseVertex at {},{}", pos.x, pos.y);
	}
	shared_ptr<Vertex> StaircaseVertex::copyWithoutEdges()
	{
		return make_shared<StaircaseVertex>(getId(), getSector(), mStaircase,
			getSectorOffset().x, getSectorOffset().y);
	}
}
