#include "core/Defines.h"
#include "core/StaircaseLocationVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	StaircaseLocationVertex
	-----------------------

	This Vertex is found in a corridor or room, and connects to a StaircaseVertex via a StaircaseMountEdge.
	*/

	StaircaseLocationVertex::StaircaseLocationVertex(uint32_t id, shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset)
		: Vertex(id, VertexType::Location, VertexSubType::Marker, sector, xLocationOffset, yLocationOffset)
	{
	}

	StaircaseLocationVertex::StaircaseLocationVertex(shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset)
		: Vertex(VertexType::Location, VertexSubType::Marker, sector, xLocationOffset, yLocationOffset)
	{
	}

	string StaircaseLocationVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("StaircaseLocationVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> StaircaseLocationVertex::copyWithoutEdges()
	{
		return make_shared<StaircaseLocationVertex>(
			getId(),
			getSector(),
			getSectorOffset().x,
			getSectorOffset().y
		);
	}

} // core