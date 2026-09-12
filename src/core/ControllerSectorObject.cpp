#include <cassert>

#include "core/Defines.h"
#include "core/ControllerSectorObject.h"
#include "core/ControllerVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	ControllerSectorObject
	---------------------

	Wrapper for an Interactable.  This is intended to be subclassed.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	ControllerSectorObject::ControllerSectorObject(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh, shared_ptr<const Sector> sector, shared_ptr<Object> object, uint32_t* vertexIdentifer)
		: SectorObject(SectorObjectType::Controller, sector, cellX, cellY, cellsWide, decksHigh, object, vertexIdentifer)
		, mController(dynamic_pointer_cast<Controller>(object))
	{
	}

	/***

	getInteractable()
	-----------

	Get the Interactable instance.
	*/
	shared_ptr<Controller> ControllerSectorObject::getController() const
	{
		return mController;
	}

} // core