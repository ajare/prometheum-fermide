#include <cassert>

#include "core/Defines.h"
#include "core/Walkway.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Walkway
	-------

	Walkways are essentially floating platforms within Locations.  They must be placed in a Location with height greater
	than 1, and on "floor" greater than zero.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	*/
	Walkway::Walkway(uint32_t cellX, uint32_t cellY)
		: Object((float)cellX, (float)cellY, 1.0f, 0.0f)
		, mCellX(cellX)
		, mCellY(cellY)
	{
	}

	/***

	getCellX()
	----------

	Get global cell x-position.
	*/
	uint32_t Walkway::getCellX() const
	{
		return mCellX;
	}

	/***

	getCellY()
	----------

	Get global cell y-position.
	*/
	uint32_t Walkway::getCellY() const
	{
		return mCellY;
	}

	std::string Walkway::getDescription() const
	{
		return "Walkway";
	}

	ControllableActionStatus Walkway::useImpl(Controller* controller, ControllableActionCallback callback)
	{
		return ControllableActionStatus::Unhandled;
	}

} // core