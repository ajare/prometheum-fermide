#include <cassert>

#include "core/Defines.h"
#include "core/Marker.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Marker
	-------

	Markers are essentially floating platforms within Locations.  They must be placed in a Location with height greater
	than 1, and on "floor" greater than zero.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	*/
	Marker::Marker(uint32_t cellX, uint32_t cellY, float xOffset)
		: Object((float)cellX, (float)cellY, 1.0f, 0.0f)
		, mCellX(cellX)
		, mCellY(cellY)
		, mOffset(xOffset)
	{
	}

	/***

	getCellX()
	----------

	Get global cell x-position.
	*/
	uint32_t Marker::getCellX() const
	{
		return mCellX;
	}

	/***

	getCellY()
	----------

	Get global cell y-position.
	*/
	uint32_t Marker::getCellY() const
	{
		return mCellY;
	}

	float Marker::getOffset() const
	{
		return mOffset;
	}

	std::string Marker::getDescription() const
	{
		return "Marker";
	}


} // core