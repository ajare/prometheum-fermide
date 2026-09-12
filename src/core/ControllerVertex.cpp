#include "core/Defines.h"
#include "core/ControllerVertex.h"
#include "core/Controller.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	ControllerVertex
	------------------

	This Vertex is used for pathing so that an Agent can move close enough to an Interactable to use it.
	*/

	ControllerVertex::ControllerVertex(uint32_t id, VertexType type, VertexSubType subType, shared_ptr<Sector> sector, shared_ptr<Controller> controller, float xLocationOffset, float yLocationOffset)
		: Vertex(id, type, subType, sector,	xLocationOffset, yLocationOffset)
		, mController(controller)
	{
	}

	ControllerVertex::ControllerVertex(VertexType type, VertexSubType subType, shared_ptr<Sector> sector, shared_ptr<Controller> controller, float xLocationOffset, float yLocationOffset)
		: Vertex(type, subType, sector,	xLocationOffset, yLocationOffset)
		, mController(controller)
	{
	}

	shared_ptr<Controller> ControllerVertex::getController() const
	{
		return mController;
	}

	string ControllerVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("ControllerVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> ControllerVertex::copyWithoutEdges()
	{
		return make_shared<ControllerVertex>(
			getId(),
			getType(),
			getSubType(),
			getSector(), 
			getController(),
			getSectorOffset().x,
			getSectorOffset().y
		);
	}

} // core