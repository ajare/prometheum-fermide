#include "core/Defines.h"
#include "core/StaircaseEdge.h"
#include "core/StaircaseTransit.h"

namespace core
{
	using namespace std;

	StaircaseTransit::StaircaseTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY,
		uint32_t cellsWide, int riseSide, float speed, vector<TransitStop> const& stops)
		: Transit(SectorType::Staircase, "Staircase", layerIndex, index,
			cellX, cellY, 0.0f, 0.0f, (float)cellsWide,
			1.0f + CORE_CORRIDOR_HEIGHT, cellsWide, 2, 1.0f, ~0u, stops)
		, VerticalEdgeCreator()
		, mStaircase(make_shared<Staircase>(cellX, cellY, cellsWide, riseSide, speed))
	{
	}

	string StaircaseTransit::getDescription() const { return mStaircase->getDescription(); }

	bool StaircaseTransit::sectorSupportsObjectType(SectorObjectType) const { return false; }

	shared_ptr<Edge> StaircaseTransit::createCrossLevelEdge(
		[[maybe_unused]] shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);
		return make_shared<StaircaseEdge>(mStaircase);
	}

	void StaircaseTransit::updateImpl(float frameTime)
	{
		mStaircase->update(frameTime);
	}
}
