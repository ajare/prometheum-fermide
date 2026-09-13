#pragma once

#include <string>
#include <vector>

#include "core/Sector.h"


namespace core
{

	class Location : public Sector
	{
		bool mIsCorridor;

	public:

		Location(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY,  uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, uint32_t capacity, bool isCorridor = false);

		[[nodiscard]] bool isCorridor() const { return mIsCorridor; }

		~Location() = default;

		// Overridden from Sector
		[[nodiscard]] std::string getDescription() const override;
	
		// Overridden from Sector
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;
	};

} // core
