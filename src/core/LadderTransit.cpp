#include <cassert>

#include "core/Defines.h"
#include "core/LadderTransit.h"
#include "core/LadderEdge.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	LadderTransit::LadderTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY, uint32_t levelsHigh, vector<TransitStop> const& stops, bool extensible, bool startExtended)
		: Transit(SectorType::Ladder, "Ladder", layerIndex, index,
			cellX, cellY,
			0.0f, 0.0f,
			1.0f, (float)levelsHigh,
			1, levelsHigh,
			CORE_CORRIDOR_HEIGHT,
			(uint32_t)((float)((levelsHigh - 1.0f) + CORE_CORRIDOR_HEIGHT) / CORE_AGENT_MAX_HEIGHT),
			stops)
		, VerticalEdgeCreator()
	{
		mLadder = make_shared<Ladder>(cellX, cellY, levelsHigh, extensible, startExtended);
	}

	shared_ptr<Ladder> LadderTransit::getLadder() const
	{
		return mLadder;
	}

	string LadderTransit::getDescription() const
	{
		return mLadder->getDescription();
	}

	bool LadderTransit::sectorSupportsObjectType(SectorObjectType type) const
	{
		return type == SectorObjectType::Door ||
			type == SectorObjectType::Window;
	}

	shared_ptr<Edge> LadderTransit::createCrossLevelEdge([[maybe_unused]] shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);

		return make_shared<LadderEdge>(mLadder);
	}

	void LadderTransit::updateImpl(float frameTime)
	{
		mLadder->update(frameTime);
	}

} // core