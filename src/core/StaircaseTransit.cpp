#include <cassert>

#include "core/Defines.h"
#include "core/StaircaseTransit.h"
#include "core/StaircaseEdge.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	StaircaseTransit::StaircaseTransit(uint32_t index, uint32_t cellX, uint32_t cellY, uint32_t decksHigh, int mountSide, vector<TransitStop> const& stops)
		: Transit(SectorType::Staircase, "Staircase", CORE_LAYER_BACK, index,
			cellX, cellY,
			0.0f, 0.0f,
			2.0f, (float)((decksHigh - 1.0f) + CORE_CORRIDOR_HEIGHT),
			2, decksHigh,
			1.0f,
			~0u,
			stops)
		, VerticalEdgeCreator()
		, mMountSide(mountSide)
	{
		mStaircase = make_shared<Staircase>(cellX, cellY, decksHigh, mountSide);
	}

	shared_ptr<Staircase> StaircaseTransit::getStaircase() const
	{
		return mStaircase;
	}

	int StaircaseTransit::getMountSide() const
	{
		return mMountSide;
	}

	string StaircaseTransit::getDescription() const
	{
		return mStaircase->getDescription();
	}

	bool StaircaseTransit::sectorSupportsObjectType(SectorObjectType type) const
	{
		return type == SectorObjectType::Door ||
			type == SectorObjectType::InteractionPoint ||
			type == SectorObjectType::Window;
	}

	shared_ptr<Edge> StaircaseTransit::createCrossDeckEdge([[maybe_unused]] shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);

		return make_shared<StaircaseEdge>(mStaircase);
	}

} // core