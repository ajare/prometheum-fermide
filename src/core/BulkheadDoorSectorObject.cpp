#include <cassert>

#include "core/Defines.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/SectorObjectVertex.h"
#include "core/BulkheadDoorVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	BulkheadDoorSectorObject
	---------------------

	Wrapper for a BulkheadDoor.  This creates and manages the BulkheadDoor instance.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	BulkheadDoorSectorObject::BulkheadDoorSectorObject(uint32_t cellX, uint32_t cellY, shared_ptr<const Sector> sectors[2])
		: SectorObject(SectorObjectType::BulkheadDoor, sectors[CORE_SIDE_LEFT], cellX, cellY, 2, 1, make_shared<BulkheadDoor>(cellX, cellY, sectors), nullptr)
	{
	}

	/***

	getDoor()
	---------

	Get the BulkheadDoor instance.
	*/
	shared_ptr<BulkheadDoor> BulkheadDoorSectorObject::getDoor() const
	{
		return static_pointer_cast<BulkheadDoor>(_getObject());
	}


	/***

	createVertex()
	-------------

	Creates a Vertex to be placed on the given side of the BulkheadDoor.

	Arguments:

	- object is actually a shared_ptr to this BulkheadDoorSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	- user holds the side.
	*/
	shared_ptr<Vertex> BulkheadDoorSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* user) const
	{
		ASSERT_PTR_EQ_THIS(object);

		float xOffset, yOffset;

		// Ensure that Vertices are placed between the door and any button which might open it.
		if (sector == getDoor()->getSideSector(CORE_SIDE_LEFT))
		{
			xOffset = sector->getCellsWide() - (CORE_BULKHEAD_DOOR_BUTTON_DIST - 0.2f);
			yOffset = (float)(getCellY() - sector->getCellY());
		}
		else
		{
			xOffset = CORE_BULKHEAD_DOOR_BUTTON_DIST - 0.2f;
			yOffset = (float)(getCellY() - sector->getCellY());
		}

		auto vertex = make_shared<BulkheadDoorVertex>(sector, getDoor(), xOffset, yOffset);
		
		vertex->setObject(object->_getObject());
		return vertex;
	}

} // core