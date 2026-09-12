#include "core/Area.h"


namespace core
{

	using namespace std;

	/*
	Area
	----

	This is a base class used by classes like Sector.
	*/

	Area::Area(uint32_t cellX, uint32_t cellY, float xCellOffset, float yCellOffset, float width, float height)
		: mCellX(cellX)
		, mCellY(cellY)
		, mCellOffset(xCellOffset, yCellOffset)
		, mSize(width, height)
	{
	}

	uint32_t Area::getCellX() const
	{
		return mCellX;
	}

	uint32_t Area::getCellY() const
	{
		return mCellY;
	}

	Vector2 const& Area::getCellOffset() const
	{
		return mCellOffset;
	}

	Vector2 Area::getPosition() const
	{
		Vector2 pos{ (float)getCellX(), (float)getCellY() };
		return pos + getCellOffset();
	}

	Vector2 const& Area::getSize() const
	{
		return mSize;
	}

	void Area::getBounds(Vector2& minExtent, Vector2& maxExtent) const
	{
		minExtent = getPosition();
		maxExtent = minExtent + getSize();
	}

	void Area::getPhysicalBounds(Vector2& minExtent, Vector2& maxExtent) const
	{
		return getBounds(minExtent, maxExtent);
	}

	bool Area::pointInBounds(float x, float y) const
	{
		Vector2 minExtent, maxExtent;

		getBounds(minExtent, maxExtent);

		if (x < minExtent.x)
		{
			return false;
		}
		if (y < minExtent.y)
		{
			return false;
		}
		if (x > maxExtent.x)
		{
			return false;
		}
		if (y > maxExtent.y)
		{
			return false;
		}

		return true;
	}

} // core