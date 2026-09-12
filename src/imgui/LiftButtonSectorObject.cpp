#include <cassert>

#include "core/Defines.h"
#include "core/LiftButtonSectorObject.h"
#include "core/LiftButton.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	LiftButtonSectorObject
	----------------------

	Specialisation of ButtonSectorObject for Lift Buttons

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	LiftButtonSectorObject::LiftButtonSectorObject(string const& name, uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, ButtonAnchorType anchorType, shared_ptr<InteractableTarget> target, shared_ptr<const Sector> sector)
		: ButtonSectorObject(name, cellX, cellY, 1, 1, anchorType, target, sector)
	{
		auto button = make_shared<LiftButton>(name, cellX, cellY, xOffset, yOffset, target);
		mObject = button;
		mInteractable = button;
	}

} // core