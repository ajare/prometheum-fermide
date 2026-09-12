#pragma once

#include <vector>
#include <cstdint>

#include "core/CellDefinition.h"


namespace core
{
	class Building;

	class Layer
	{
		Building const* mwBuilding;

		uint32_t mCellsWide, mDecksHigh, mZ;

		std::vector<CellDefinition> mCells;

	private:

		void validateCellBounds(uint32_t x, uint32_t y) const;

	public:

		Layer(Building const* building, uint32_t cellsWide, uint32_t decksHigh, uint32_t z);

		uint32_t getCellsWide() const;

		uint32_t getDecksHigh() const;

		uint32_t getZ() const;

		CellDefinition const& getCellDefinition(uint32_t x, uint32_t y) const;

		CellDefinition& getCellDefinition(uint32_t x, uint32_t y);
	};

} // core
