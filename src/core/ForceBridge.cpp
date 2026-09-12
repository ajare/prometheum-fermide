#include <cassert>

#include "core/Defines.h"
#include "core/ForceBridge.h"
#include "core/SectorObjectVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	ForceBridge
	-----------

	A ForceBridge is basically an extensible/retractable Walkway.  They are generally controlled by a Button, or
	similar object, and not extended/retracted directly.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	- fromSide determines the side from which the bridge extends: see CORE_SIDE_LEFT / CORE_SIDE_RIGHT.
	*/
	ForceBridge::ForceBridge(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int fromSide, bool extensible, bool startExtended)
		: ExtensibleObject((float)cellX, (float)cellY, (float)cellsWide, 0.0f, extensible, startExtended)
		, mFromSide(fromSide)
	{
	}

	/***

	getFromSide()
	-------------

	Get the side from which the ForceBridge extends: see CORE_SIDE_LEFT / CORE_SIDE_RIGHT.
	*/
	int ForceBridge::getFromSide() const
	{
		return mFromSide;
	}

	/***

	getDescription()
	----------------

	Get a description.
	*/
	string ForceBridge::getDescription() const
	{
		return "ForceBridge";
	}

	/***

	getExtendRetractTime()
	------------------

	Get the time taken for the ForceBridge to fully extend or retract.  The time for extending
	and retracting will always be the same.
	*/
	float ForceBridge::getExtendRetractTime() const
	{
		return CORE_FORCEBRIDGE_EXTEND_RETRACT_TIME * getSize().x;
	}

	/***

	getCurrentShape()
	-----------------

	Get the current, physical extents of the ForceBridge.  If the ForceBridge is partially
	extended, then it will take this into consideration.
	*/
	void ForceBridge::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);

		if (getFromSide() == CORE_SIDE_LEFT)
		{
			maxExtent.x = minExtent.x + getSize().x * getExtendedPercentage();
		}
		else
		{
			minExtent.x = maxExtent.x - getSize().x * getExtendedPercentage();
		}
	}

} // core