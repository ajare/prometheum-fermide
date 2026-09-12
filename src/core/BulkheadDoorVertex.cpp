#include "core/Defines.h"
#include "core/BulkheadDoorVertex.h"
#include "core/Location.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	BulkheadDoorVertex
	------------------

	This Vertex is used for pathing so that an Agent can move close enough to a Bulkhead Door to use it.
	*/

	BulkheadDoorVertex::BulkheadDoorVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<BulkheadDoor> door, float xLocationOffset, float yLocationOffset)
		: Vertex(id, VertexType::Location, VertexSubType::BulkheadDoor, sector,	xLocationOffset, yLocationOffset)
		, mBulkheadDoor(door)
	{
	}

	BulkheadDoorVertex::BulkheadDoorVertex(shared_ptr<Sector> sector, shared_ptr<BulkheadDoor> door, float xLocationOffset, float yLocationOffset)
		: Vertex(VertexType::Location, VertexSubType::BulkheadDoor, sector,	xLocationOffset, yLocationOffset)
		, mBulkheadDoor(door)
	{
	}

	shared_ptr<BulkheadDoor> BulkheadDoorVertex::getBulkheadDoor() const
	{
		return mBulkheadDoor;
	}

	string BulkheadDoorVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("BulkheadDoorVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> BulkheadDoorVertex::copyWithoutEdges()
	{
		return make_shared<BulkheadDoorVertex>(
			getId(), 
			getSector(), 
			getBulkheadDoor(), 
			getSectorOffset().x, 
			getSectorOffset().y
		);
	}

} // core