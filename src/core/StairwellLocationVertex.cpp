#include "core/Defines.h"
#include "core/StairwellLocationVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	StairwellLocationVertex
	-----------------------

	This Vertex is found in a corridor or room, and connects to a StairwellVertex via a StairwellMountEdge.
	*/

	StairwellLocationVertex::StairwellLocationVertex(uint32_t id, shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset)
		: Vertex(id, VertexType::Location, VertexSubType::Marker, sector, xLocationOffset, yLocationOffset)
	{
	}

	StairwellLocationVertex::StairwellLocationVertex(shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset)
		: Vertex(VertexType::Location, VertexSubType::Marker, sector, xLocationOffset, yLocationOffset)
	{
	}

	string StairwellLocationVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("StairwellLocationVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> StairwellLocationVertex::copyWithoutEdges()
	{
		return make_shared<StairwellLocationVertex>(
			getId(),
			getSector(),
			getSectorOffset().x,
			getSectorOffset().y
		);
	}

} // core