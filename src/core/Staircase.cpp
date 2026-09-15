#include <cassert>

#include "core/Defines.h"
#include "core/Staircase.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Staircase
	---------

	Staircases connect Locations on different Decks.  They live on the Back Layer, and must connect to a Fore
	Location on each Deck.  They can connect to multiple floors within the same Location, as long as the exit
	is on ground or a Walkway.

	Staircases are two cells wide, in order that the steps are not too steep.
	
	They may have multiple Agents on them at any time, going in either direction.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- decksHigh is the number of decks that the Staircase spans.  So a Staircase joining Decks 0 and 1 will have a decksHigh of 2
	*/
	Staircase::Staircase(uint32_t cellX, uint32_t cellY, uint32_t decksHigh, int mountSide)
		: Object((float)cellX, (float)cellY, 2.0f, (float)decksHigh)
		, mDecksHigh(decksHigh)
		, mMountSide(mountSide)
	{
	}

	/***

	getDecksHigh()
	--------------

	Get number of decks that the Staircase joins.
	*/
	uint32_t Staircase::getDecksHigh() const
	{
		return mDecksHigh;
	}

	/***

	getMountSide()
	--------------

	Gets the side from which you mount the Staircase.
	*/
	int Staircase::getMountSide() const
	{
		return mMountSide;
	}

	array<Vector2, 4> Staircase::getDeckPath(uint32_t deckOffset) const
	{
		float const lowerX = mMountSide == CORE_SIDE_LEFT ? 1.666f : 0.334f;
		float const upperX = mMountSide == CORE_SIDE_LEFT ? 0.334f : 1.666f;
		float const y = (float)deckOffset;
		return { Vector2{ 1.0f, y }, Vector2{ lowerX, y + 0.25f },
			Vector2{ upperX, y + 0.75f }, Vector2{ 1.0f, y + 1.0f } };
	}

	/***

	getDescription()
	----------------

	Get a description.
	*/
	string Staircase::getDescription() const
	{
		return format("Staircase - {} decks", mDecksHigh);
	}

	/***

	getCurrentShape()
	-----------------

	Get the current, physical extents of the Staircase.
	*/
	void Staircase::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}


} // core