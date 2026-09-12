#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/Lift.h"


namespace core
{

	class CarLift : public Lift
	{
	public:

		CarLift(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, std::vector<uint32_t> const& stopOffsets);

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Lift
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};

} // core
