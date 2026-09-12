#include "core/Layer.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	Layer::Layer(Building const* building, uint32_t cellsWide, uint32_t decksHigh, uint32_t z)
		: mwBuilding(building)
		, mCellsWide(cellsWide)
		, mDecksHigh(decksHigh)
		, mZ(z)
	{
		mCells.resize(cellsWide * decksHigh);
	}

	uint32_t Layer::getCellsWide() const
	{
		return mCellsWide;
	}

	uint32_t Layer::getDecksHigh() const
	{
		return mDecksHigh;
	}

	uint32_t Layer::getZ() const
	{
		return mZ;
	}

	void Layer::validateCellBounds(uint32_t x, uint32_t y) const
	{
		// Bounds checks
		if (x >= mCellsWide)
		{
			throw BuildingException(mwBuilding, format("Layer::validateCellBounds({}, {}) - x={} is out of bounds", x, y, x));
		}

		if (y >= mDecksHigh)
		{
			throw BuildingException(mwBuilding, format("Layer::validateCellBounds({}, {}) - y={} is out of bounds", x, y, y));
		}
	}

	CellDefinition const& Layer::getCellDefinition(uint32_t x, uint32_t y) const
	{
		validateCellBounds(x, y);
		return mCells[y * mCellsWide + x];
	}

	CellDefinition& Layer::getCellDefinition(uint32_t x, uint32_t y)
	{
		validateCellBounds(x, y);
		return mCells[y * mCellsWide + x];

	}

} // core