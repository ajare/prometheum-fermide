#include "core/DependentPathControllable.h"
#include "core/Controller.h"


namespace core
{
	using namespace std;

	DependentPathControllable::DependentPathControllable()
		: Controllable()
		, DependentPathObject()
	{
	}

	/***

	getDependingController()
	--------------------------

	Return the buttons and other objects which can open/close/etc this Door, which an Agent might need to use
	to pass through it.

	Arguments:

	- index here is the Layer index, or the Side index.
	*/
	shared_ptr<Controller> DependentPathControllable::getDependingController(uint32_t index) const
	{
		if (index >= getNumControllers())
		{
			return nullptr;
		}

		return getController(index);
	}

} // core
