#include "core/Defines.h"
#include "core/DoorVertex.h"
#include "core/Location.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	DoorVertex
	----------

	This Vertex is used for pathing so that an Agent can move to a Door, before passing through it.
	*/

	DoorVertex::DoorVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<Door> door, float xLocationOffset, float yLocationOffset)
		: Vertex(id, VertexType::Location, VertexSubType::Door, sector,	xLocationOffset, yLocationOffset)
		, mDoor(door)
	{
	}

	DoorVertex::DoorVertex(shared_ptr<Sector> sector, shared_ptr<Door> door, float xLocationOffset, float yLocationOffset)
		: Vertex(VertexType::Location, VertexSubType::Door, sector,	xLocationOffset, yLocationOffset)
		, mDoor(door)
	{
	}

	shared_ptr<Door> DoorVertex::getDoor() const
	{
		return mDoor;
	}

	string DoorVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("DoorVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> DoorVertex::copyWithoutEdges()
	{
		return make_shared<DoorVertex>(
			getId(), 
			getSector(), 
			getDoor(), 
			getSectorOffset().x, 
			getSectorOffset().y
		);
	}

} // core