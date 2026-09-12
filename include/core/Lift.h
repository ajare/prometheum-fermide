#pragma once

#include <cstdint>
#include <memory>

#include "core/RailedTransport.h"


namespace core
{

	class Lift : public RailedTransport
	{
		// Overridden from RailedTransport
		uint32_t getStopRefIndex(uint32_t index) const override;

	public:

		Lift(uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, float transportWidth, float transportHeight, float speed, std::vector<uint32_t> stopOffsets);
	
	};

} // core
