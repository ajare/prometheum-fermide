#pragma once

#include <cstdint>
#include <memory>

#include "core/RailedTransport.h"


namespace core
{

	class Shuttle : public RailedTransport
	{
		uint32_t mNumCars;

		uint32_t mCarWidth;

	private:

		// Overridden from RailedTransport
		uint32_t getStopRefIndex(uint32_t index) const override;

	public:

		Shuttle(uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, float transportWidth, float transportHeight, uint32_t numCars, uint32_t carWidth, std::vector<uint32_t> stopOffsets);

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;

		uint32_t getNumCars() const;

		uint32_t getCarWidth() const;
	};

} // core
