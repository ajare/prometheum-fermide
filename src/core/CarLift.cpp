#include <format>

#include "core/Defines.h"
#include "core/CarLift.h"


namespace core
{

	using namespace std;

	/***

	CarLift
	-------

	CarLift is a subclass of Lift which is used to connect Decks, within a LiftTransit.

	Construction arguments:

	- cellX and cellY are global, not relative to the Sector that it's in.
	*/
	CarLift::CarLift(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, vector<uint32_t> const& stopOffsets)
		: Lift(cellX, cellY, CORE_LIFT_CAR_BORDER, 0.0f, (float)(cellsWide - CORE_LIFT_CAR_BORDER * 2), CORE_LIFT_CAR_HEIGHT, CORE_LIFT_SPEED, stopOffsets)
	{
	}

	string CarLift::getDescription() const
	{
		return "Lift";
	}

	/***

	getCurrentShape()
	-----------------

	Get the current, physical extents of the Lift.
	*/
	void CarLift::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}

} // core