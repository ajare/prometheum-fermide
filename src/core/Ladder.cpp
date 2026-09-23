#include <algorithm>
#include <cassert>

#include "core/Defines.h"
#include "core/Ladder.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Ladder
	------

	Ladders are flexible objects which connect two different Locations on different Decks, but may also connect
	different floors of the same Location.  When connecting different Locations, those Locations must be on the
	Fore Layer, and the Ladder must be wrapped by a LadderTransit and placed on the Back Layer.  When connecting
	two floors within the same Location, the Location can be on either Layer, but must obviously be more than
	one floor high.

	Ladders can be extended or retracted through world-owned interaction points, typically represented by
	a physical Button at the base and an interaction point at the top.

	Ladders may span multiple decks, and pass behind Locations.  The requirement is that for the Locations that
	they connect, the Cells they connect on have a floor (either the ground, or a Walkway).

	Ladders may have multiple Agents on them, but they must all be travelling in the same direction.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- decksHigh is the number of decks that the Ladder spans.  So a Ladder joining Decks 0 and 1 will have a decksHigh of 2
	*/
	Ladder::Ladder(uint32_t cellX, uint32_t cellY, uint32_t decksHigh, bool extensible, bool startExtended)
		: ExtensibleObject(GENERATE_LADDER_DIMS(cellX, cellY, decksHigh), extensible, startExtended)
		, mDecksHigh(decksHigh)
	{
	}

	/***

	getDecksHigh()
	--------------

	Get number of decks that the Ladder joins.
	*/
	uint32_t Ladder::getDecksHigh() const
	{
		return mDecksHigh;
	}

	float Ladder::getUsableLength() const
	{
		return (float)(mDecksHigh - 1) + CORE_LADDER_HEIGHT_AT_TOP
			- CORE_LADDER_HEIGHT_OFF_GROUND;
	}

	/***

	getDescription()
	----------------

	Get a description.
	*/
	string Ladder::getDescription() const
	{
		return format("Ladder - {} decks", mDecksHigh);
	}

	/***

	getMaxRetractedPercentage()
	---------------------------

	When a Ladder retracts, it will not do so completely (like a ForceBridge), otherwise it would disappear.
	Ladders always retract upwards / extend downwards.
	*/
	float Ladder::getMaxRetractedPercentage() const
	{
		// Retract enough such that the height above the lowest Deck is the same
		// as when fully extended.
		auto height = (float)getDecksHigh();

		auto retractedHeight = min(CORE_LADDER_HEIGHT_AT_TOP - CORE_LADDER_HEIGHT_OFF_GROUND, CORE_LADDER_HEIGHT_AT_TOP);
		return retractedHeight / height;
	}

	/***

	getExtendRetractTime()
	------------------

	Get the time taken for the Ladder to fully extend or retract.  The time for extending
	and retracting will always be the same.
	*/
	float Ladder::getExtendRetractTime() const
	{
		return CORE_LADDER_EXTEND_RETRACT_TIME * getDecksHigh();
	}

	/***

	getCurrentShape()
	-----------------

	Get the current, physical extents of the Ladder.  If the Ladder is partially
	extended, then it will take this into consideration.
	*/
	void Ladder::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);

		auto const fullLength = maxExtent.y - minExtent.y;
		auto const visibleLength = min(fullLength,
			max(CORE_LADDER_MIN_RETRACTED_LENGTH, fullLength * getExtendedPercentage()));
		minExtent.y = maxExtent.y - visibleLength;
	}

} // core