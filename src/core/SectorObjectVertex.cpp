#include "core/Defines.h"
#include "core/SectorObjectVertex.h"
#include "core/SectorObject.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	
	SectorObjectVertex
	------------

	This Vertex is used in Locations, and references a SectorObject.  For instance, it will be typically used for
	connected to a Ladder within a room, as one end of the LadderMountEdge.
	*/

	SectorObjectVertex::SectorObjectVertex(uint32_t id, VertexSubType subType, shared_ptr<Sector> sector, shared_ptr<SectorObject> object, float xLocationOffset, float yLocationOffset)
		: Vertex(id, VertexType::Location, subType, sector,	xLocationOffset, yLocationOffset)
		, mObject(object)
	{
	}

	SectorObjectVertex::SectorObjectVertex(VertexSubType subType, shared_ptr<Sector> sector, shared_ptr<SectorObject> object, float xLocationOffset, float yLocationOffset)
		: Vertex(VertexType::Location, subType, sector,	xLocationOffset, yLocationOffset)
		, mObject(object)
	{
	}

	shared_ptr<SectorObject> SectorObjectVertex::getObject() const
	{
		return mObject;
	}

	string SectorObjectVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("SectorObjectVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> SectorObjectVertex::copyWithoutEdges()
	{
		return make_shared<SectorObjectVertex>(
			getId(), 
			getSubType(), 
			getSector(), 
			mObject, 
			getSectorOffset().x, 
			getSectorOffset().y
		);
	}

} // core