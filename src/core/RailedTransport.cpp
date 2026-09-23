#include "core/RailedTransport.h"

namespace core
{
	RailedTransport::RailedTransport(float xOffset, float yOffset, float transportWidth,
		float transportHeight, float speed, std::vector<CellPosition> const& stops,
		bool looping)
		: Object((float)stops.at(0).x + xOffset, (float)stops.at(0).y + yOffset,
			transportWidth, transportHeight), mStops(stops), mLooping(looping), mSpeed(speed)
	{
	}

	bool RailedTransport::hasStop(uint32_t x, uint32_t y) const
	{
		return getStopIndex(x, y) != ~0u;
	}

	uint32_t RailedTransport::getNumStops() const { return (uint32_t)mStops.size(); }
	uint32_t RailedTransport::getStopDeckIndex(uint32_t index) const { return mStops.at(index).y; }

	uint32_t RailedTransport::getStopIndex(uint32_t x, uint32_t y) const
	{
		for (uint32_t i = 0; i < mStops.size(); ++i)
			if (mStops[i].x == x && mStops[i].y == y) return i;
		return ~0u;
	}

	std::vector<std::pair<std::string, std::string>> RailedTransport::getInternalsStrings() const
	{
		return { { "Coordination", "World traversal resource" },
			{ "Stops", std::to_string(mStops.size()) } };
	}
}
