#pragma once

#include <cstdint>
#include <vector>

#include "core/Object.h"
#include "core/CellPosition.h"

namespace core
{
	// Shared render geometry for lift and shuttle vehicles. Scheduling and motion
	// are authoritative in the building-owned traversal coordinator.
	class RailedTransport : public Object
	{
	protected:
		std::vector<CellPosition> mStops;
		bool mLooping;
		float mSpeed;

	public:
		RailedTransport(float xOffset, float yOffset, float transportWidth,
			float transportHeight, float speed, std::vector<CellPosition> const& stops,
			bool looping);
		bool hasStop(uint32_t x, uint32_t y) const;
		uint32_t getNumStops() const;
		uint32_t getStopDeckIndex(uint32_t index) const;
		uint32_t getStopIndex(uint32_t x, uint32_t y) const;
		std::vector<std::pair<std::string, std::string>> getInternalsStrings() const override;
	};
}
