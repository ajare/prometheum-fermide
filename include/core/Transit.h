#pragma once

#include <string>

#include "core/Sector.h"


namespace core
{

	struct TransitStop
	{
		std::shared_ptr<const Sector> sector;
		int sectorOffsetX, sectorOffsetY;
	};

	class Transit : public Sector
	{
		std::vector<TransitStop> mStops;

	public:

		Transit(SectorType type, std::string const& name, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY, float xCellOffset, float yCellOffset, float width, float height, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, uint32_t capacity, std::vector<TransitStop> const& stops);

		~Transit() = default;

		TransitStop const& getStop(uint32_t index) const;

		uint32_t getNumStops() const;
	};

} // core
