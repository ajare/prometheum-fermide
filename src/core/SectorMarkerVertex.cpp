#include "core/Defines.h"
#include "core/SectorMarkerVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	SectorMarkerVertex
	--------------------

	This Vertex is essentially a marker Vertex used in a corridor.  It may be used for connecting corridors to LadderTransits,
	or for putting a waypoint next to a gap in the floor (eg on a Walkway).
	*/

	SectorMarkerVertex::SectorMarkerVertex(uint32_t id, shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset)
		: Vertex(id, VertexType::Location, VertexSubType::Marker, sector, xLocationOffset, yLocationOffset)
	{
	}

	SectorMarkerVertex::SectorMarkerVertex(shared_ptr<Sector> sector, float xLocationOffset, float yLocationOffset)
		: Vertex(VertexType::Location, VertexSubType::Marker, sector, xLocationOffset, yLocationOffset)
	{
	}

	string SectorMarkerVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("SectorMarkerVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> SectorMarkerVertex::copyWithoutEdges()
	{
		return make_shared<SectorMarkerVertex>(
			getId(),
			getSector(),
			getSectorOffset().x,
			getSectorOffset().y
		);
	}

} // core