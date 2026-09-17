#include <cassert>
#include <algorithm>

#include "core/Defines.h"
#include "core/DoorSectorObject.h"
#include "core/SectorObjectVertex.h"
#include "core/DoorVertex.h"
#include "core/DoorEdge.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	DoorSectorObject
	---------------------

	Wrapper for a Door.  This creates and manages the Door instance.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	DoorSectorObject::DoorSectorObject(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, shared_ptr<const Sector> sectors[2], uint32_t* vertexIdentifer)
		: SectorObject(SectorObjectType::Door, sectors[0], cellX, cellY, cellsWide, 1, make_shared<Door>(cellX, cellY, cellsWide, sectors), vertexIdentifer)
	{
	}

	/***

	getDoor()
	---------

	Get the Door instance.
	*/
	shared_ptr<Door> DoorSectorObject::getDoor() const
	{
		return static_pointer_cast<Door>(_getObject());
	}

	vector<float> DoorSectorObject::calculateDoorQueueStopOffsets(Building const* building, shared_ptr<Vertex> vertex, uint32_t x, uint32_t y, uint32_t doorWidth) const
	{
		vector<float> stops;
		const float stopWidth = CORE_DOOR_QUEUE_STOP_WIDTH;

		auto sector = vertex->getSector();
		auto layer = building->getLayer(sector->getLayerIndex());

		int32_t ix0 = (int32_t)x;
		int32_t ix1 = (int32_t)(x + doorWidth);

		// Left extent
		for (; ix0 >= (int32_t)sector->getCellX0(); --ix0)
		{
			auto const& cellDef = layer->getCellDefinition((uint32_t)ix0, y);

			if (!cellDef.isTraversableOnFoot())
			{
				break;
			}
		}

		ix0++;

		// Right extent
		for (; ix1 <= (int32_t)sector->getCellX1(); ++ix1)
		{
			auto const& cellDef = layer->getCellDefinition((uint32_t)ix1, y);

			if (!cellDef.isTraversableOnFoot())
			{
				break;
			}
		}

		ix1--;

		// Generate stops
		stops.push_back(0.0f);

		float vx = vertex->getPosition().x;
		float xc = x + doorWidth / 2.0f;
		float stopX = xc - stopWidth;

		while (stopX >= ix0)
		{
			stops.push_back(stopX - vx);
			stopX -= stopWidth;
		}

		stopX = xc + stopWidth;

		while (stopX < (ix1 + 1))
		{
			stops.push_back(stopX - vx);
			stopX += stopWidth;
		}

		// Sort stops by increasing distance from centre of door
		sort(stops.begin(), stops.end(), [](auto a, auto b) { return fabs(a) < fabs(b); });

		return stops;
	}



	/***

	createVertex()
	-------------

	Creates a Vertex to be placed in front of the Door.

	Arguments:

	- object is actually a shared_ptr to this DoorSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	*/
	shared_ptr<Vertex> DoorSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* /* user */) const
	{
		ASSERT_PTR_EQ_THIS(object);

		auto doorCentre = getDoor()->getCellsWide() / 2.0f;

		float xOffset = (float)(getCellX() - sector->getCellX()) + doorCentre;
		float yOffset = (float)(getCellY() - sector->getCellY());

		auto vertex = make_shared<DoorVertex>(sector, dynamic_pointer_cast<DoorSectorObject>(object)->getDoor(), xOffset, yOffset);

		vertex->setObject(object->_getObject());
		return vertex;
	}

} // core