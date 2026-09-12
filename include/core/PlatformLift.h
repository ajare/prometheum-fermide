#pragma once

#include <cstdint>
#include <memory>

#include "core/Lift.h"


namespace core
{

	class PlatformLift : public Lift
	{
		// Overridden from RailedTransport
		ControllableActionStatus updateCallToStop(ControllableAction const& action, float frameTime) override;

	protected:

		// Overridden from RailedTransport
		void arriveAtStop(uint32_t index) override;

	public:

		PlatformLift(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, std::vector<uint32_t> const& stopOffsets);

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		// Overridden from Lift
		void getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const override;
	};

} // core
