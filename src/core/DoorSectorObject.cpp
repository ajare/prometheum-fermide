#include <cassert>
#include <algorithm>

#include "core/Defines.h"
#include "core/DoorSectorObject.h"
#include "core/SectorObjectVertex.h"
#include "core/DoorVertex.h"
#include "core/DoorVertexController.h"
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
		: SectorObject(SectorObjectType::Door, sectors[CORE_LAYER_FORE], cellX, cellY, cellsWide, 1, make_shared<Door>(cellX, cellY, cellsWide, sectors), vertexIdentifer)
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


	shared_ptr<VertexController> DoorSectorObject::createVertexController(Building const* building, vector<shared_ptr<Vertex>> const& vertices, map<shared_ptr<Controller>, shared_ptr<Vertex>> const& controllerVertexLookup) const
	{
		auto door = static_pointer_cast<Door>(_getObject());

		// Migrated doors are governed exclusively by their TraversalResource.
		if (door->getTraversalResourceId())
		{
			return nullptr;
		}

		auto vcPos = vertices[CORE_LAYER_FORE]->getPosition();
		auto vcSize = getSize();

		auto vcPos0 = Vector2(vcPos.x - vcSize.x / 2, vcPos.y);
		auto vcPos1 = Vector2(vcPos.x - vcSize.x / 2, vcPos.y);
		auto vcSize0 = Vector2(vcSize.x, CORE_AGENT_MAX_HEIGHT);
		auto vcSize1 = Vector2(vcSize.x, CORE_AGENT_MAX_HEIGHT);

		// If this Door is controlled (eg by a Button) then expand the Control
		// Area to encompass the Button, so it can be pressed on the way, if
		// the Agent is on the same side.
		shared_ptr<Controller> foreController, backController;
		shared_ptr<Vertex> foreControllerVertex, backControllerVertex;

		if (hasController("ForeController"))
		{
			foreController = getController("ForeController");
			foreControllerVertex = controllerVertexLookup.at(foreController);
			auto fcPos = foreControllerVertex->getPosition();
			
			vcPos0.x = min(vcPos0.x, fcPos.x);
			vcSize0.x = (max(vcPos0.x + vcSize.x, fcPos.x) + CORE_VERTEXCONTROLLER_CONTROLLER_PADDING) - vcPos0.x;
		}
		if (hasController("BackController"))
		{
			backController = getController("BackController");
			backControllerVertex = controllerVertexLookup.at(backController);
			auto bcPos = backControllerVertex->getPosition();

			vcPos1.x = min(vcPos1.x, bcPos.x);
			vcSize1.x = (max(vcPos1.x + vcSize.x, bcPos.x) + CORE_VERTEXCONTROLLER_CONTROLLER_PADDING) - vcPos1.x;
		}
		 
		Shape controlAreas[2] = {
			Shape(vcPos0.x, vcPos0.y, vcSize0.x, vcSize0.y),
			Shape(vcPos1.x, vcPos1.y, vcSize1.x, vcSize1.y)
		};

		// Exit Areas are the same for both Vertices
		auto exitX = vcPos.x - vcSize.x / 2 + CORE_AGENT_MAX_WIDTH * 0.5f;
		auto exitW = vcSize.x - CORE_AGENT_MAX_WIDTH;
		auto vcExitShape = Shape(exitX, vcPos.y, exitW, CORE_AGENT_MAX_HEIGHT);

		Shape exitAreas[2] = {
			vcExitShape,
			vcExitShape
		};

		shared_ptr<Vertex> vertPtrs[2] =
		{
			vertices[0],
			vertices[1]
		};

		shared_ptr<Controller> controllerPtrs[2] = {
			foreController,
			backController
		};

		shared_ptr<Vertex> controllerVertPtrs[2] = {
			foreControllerVertex,
			backControllerVertex
		};

		auto vertexController = make_shared<DoorVertexController>(
			this, 
			controlAreas, 
			exitAreas, 
			building, 
			vertPtrs, 
			getCellX(), 
			getCellY(), 
			door->getCellsWide(),
			controllerPtrs,
			controllerVertPtrs
		);

		for (int i = 0; i < CORE_NUM_LAYERS; ++i)
		{
			vertices[i]->_setController(vertexController);
			vertexController->setVertexForLayer(i, vertices[i]);
		}

		// Set SensorSource
		door->setSensorSource(vertexController);

		return vertexController;
	}

	/***

	createVertex()
	-------------

	Creates a Vertex to be placed in front of the Door.

	Arguments:

	- object is actually a shared_ptr to this DoorSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	*/
	shared_ptr<Vertex> DoorSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* user) const
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