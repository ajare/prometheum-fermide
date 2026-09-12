#include <cassert>

#include "core/Defines.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/SectorObjectVertex.h"
#include "core/ForceBridgeEdge.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	ForceBridgeSectorObject
	---------------------

	Wrapper for a ForceBridge.  This creates and manages the ForceBridge instance.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	- fromSide determines the side from which the bridge extends: see CORE_SIDE_LEFT / CORE_SIDE_RIGHT.
	*/
	ForceBridgeSectorObject::ForceBridgeSectorObject(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, int fromSide, shared_ptr<const Sector> sector, bool extensible, bool startExtended)
		: SectorObject(SectorObjectType::ForceBridge, sector, cellX, cellY, cellsWide, 1, make_shared<ForceBridge>(cellX, cellY, cellsWide, fromSide, extensible, startExtended), nullptr)
	{
	}

	/***

	getForceBridge()
	-----------

	Get the ForceBridge instance.
	*/
	shared_ptr<ForceBridge> ForceBridgeSectorObject::getForceBridge() const
	{
		return static_pointer_cast<ForceBridge>(_getObject());
	}

	/***

	createVertex()
	-------------

	Creates a Vertex to be placed on a given side of the ForceBridge.

	Arguments:

	- object is actually a shared_ptr to this ForceBridgeSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	- user holds the side.
	*/
	shared_ptr<Vertex> ForceBridgeSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* user) const
	{
		int side = *(static_cast<int*>(user));

		ASSERT_PTR_EQ_THIS(object);
		ASSERT_SIDE_OK(side);

		float xOffset;
		float yOffset = (float)(getCellY() - sector->getCellY());

		if (side == CORE_SIDE_LEFT)
		{
			xOffset = (float)(getCellX() - sector->getCellX()) - CORE_AGENT_MAX_WIDTH * 0.5f;
		}
		else
		{
			xOffset = (float)(getCellX() - sector->getCellX()) + getForceBridge()->getSize().x + CORE_AGENT_MAX_WIDTH * 0.5f;
		}

		auto vertex = make_shared<SectorObjectVertex>(
			VertexSubType::ForceBridge,
			sector,
			object,
			xOffset, yOffset
		);

		vertex->setObject(object->_getObject());
		return vertex;
	}

} // core