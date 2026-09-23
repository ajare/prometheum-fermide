#include <cassert>

#include "core/Defines.h"
#include "core/StairwellTransit.h"
#include "core/StairwellEdge.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	StairwellTransit::StairwellTransit(uint32_t index, uint32_t layerIndex, uint32_t cellX, uint32_t cellY, uint32_t levelsHigh, int mountSide, vector<TransitStop> const& stops)
		: Transit(SectorType::Stairwell, "Stairwell", layerIndex, index,
			cellX, cellY,
			0.0f, 0.0f,
			2.0f, (float)((levelsHigh - 1.0f) + CORE_CORRIDOR_HEIGHT),
			2, levelsHigh,
			1.0f,
			~0u,
			stops)
		, VerticalEdgeCreator()
		, mMountSide(mountSide)
	{
		mStairwell = make_shared<Stairwell>(cellX, cellY, levelsHigh, mountSide);
	}

	shared_ptr<Stairwell> StairwellTransit::getStairwell() const
	{
		return mStairwell;
	}

	int StairwellTransit::getMountSide() const
	{
		return mMountSide;
	}

	string StairwellTransit::getDescription() const
	{
		return mStairwell->getDescription();
	}

	bool StairwellTransit::sectorSupportsObjectType(SectorObjectType type) const
	{
		return type == SectorObjectType::Door ||
			type == SectorObjectType::InteractionPoint ||
			type == SectorObjectType::Window;
	}

	shared_ptr<Edge> StairwellTransit::createCrossLevelEdge([[maybe_unused]] shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);

		return make_shared<StairwellEdge>(mStairwell);
	}

} // core