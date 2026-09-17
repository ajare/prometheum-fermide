#include <cassert>

#include "core/Defines.h"
#include "core/LadderTransit.h"
#include "core/LadderEdge.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	LadderTransit::LadderTransit(uint32_t index, uint32_t cellX, uint32_t cellY, uint32_t decksHigh, vector<TransitStop> const& stops, bool extensible, bool startExtended)
		: Transit(SectorType::Ladder, "Ladder", layerBehind(0), index,
			cellX, cellY,
			0.0f, 0.0f,
			1.0f, (float)decksHigh,
			1, decksHigh, 
			CORE_CORRIDOR_HEIGHT,
			(uint32_t)((float)((decksHigh - 1.0f) + CORE_CORRIDOR_HEIGHT) / CORE_AGENT_MAX_HEIGHT),
			stops)
		, VerticalEdgeCreator()
	{
		mLadder = make_shared<Ladder>(cellX, cellY, decksHigh, extensible, startExtended);
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

	shared_ptr<Edge> LadderTransit::createCrossDeckEdge([[maybe_unused]] shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);

		return make_shared<LadderEdge>(mLadder);
	}

	void LadderTransit::updateImpl(float frameTime)
	{
		mLadder->update(frameTime);
	}

} // core