#include <cassert>

#include "core/Defines.h"
#include "core/LiftButton.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	LiftButton
	----------

	Specialisation of Button to call a Lift to its Deck.

	Construction arguments:

	- cellX and cellY are global, not relative to the Sector that it's in.
	- xOffset and yOffset are offsets to cellX and cellY to precisely place the Button
	*/
	LiftButton::LiftButton(string const& name, uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, shared_ptr<InteractableTarget> target)
		: Button(name, cellX, cellY, xOffset, yOffset, InteractableType::LiftButton, target)
		, mDeckIndex(cellY)
	{
	}

	/***

	use()
	-----

	Call the Lift to this Deck.
	*/
	InteractionResult LiftButton::use()
	{
		return mTarget->interact(this, InteractionType::CallLiftToDeck, &mDeckIndex);
	}

} // core