#include <format>
#include <cassert>

#include "core/Defines.h"
#include "core/Transit.h"


namespace core
{

	using namespace std;

	Transit::Transit(SectorType type, string const& name, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY, float xCellOffset, float yCellOffset, float width, float height, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, uint32_t capacity, vector<TransitStop> const& stops)
		: Sector(type, layerIndex, index, cellX, cellY, xCellOffset, yCellOffset, width, height, name, cellsWide, levelsHigh, topLevelHeight, capacity)
		, mStops(stops)
	{
	}

	TransitStop const& Transit::getStop(uint32_t index) const
	{
		ASSERT_CONTAINTER_INDEX(index, mStops);

		return mStops[index];
	}

	uint32_t Transit::getNumStops() const
	{
		return (uint32_t)mStops.size();
	}

} // core