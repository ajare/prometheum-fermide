#pragma once

#include "core/Lift.h"

namespace core
{
	class PlatformLift : public Lift
	{
	public:
		PlatformLift(uint32_t cellX, uint32_t cellY, uint32_t cellsWide,
			std::vector<uint32_t> const& stopOffsets);
		std::string getDescription() const override;
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};
}
