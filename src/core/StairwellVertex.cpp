#include "core/Defines.h"
#include "core/StairwellVertex.h"
#include "core/Location.h"
#include "core/SectorType.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	StairwellVertex
	---------------

	This Vertex is used at each end of a Stairwell, and also within to provide navigation in a zig-zag line.  It connects
	to other StairwellVertices via a StairwellEdge, and StairwellLocationVertices via StairwellMountEdge.
	*/

	StairwellVertex::StairwellVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<Stairwell> stairwell, float xOffset, float yOffset, uint32_t levelOffset)
		: Vertex(id, isLocationLike(sector->getType()) ? VertexType::Location : VertexType::Stairwell, VertexSubType::Stairwell, sector, xOffset, yOffset)
		, mStairwell(stairwell)
		, mLevelOffset(levelOffset)
	{
	}

	StairwellVertex::StairwellVertex(shared_ptr<Sector> sector, shared_ptr<Stairwell> stairwell, float xOffset, float yOffset, uint32_t levelOffset)
		: Vertex(isLocationLike(sector->getType()) ? VertexType::Location : VertexType::Stairwell, VertexSubType::Stairwell, sector, xOffset, yOffset)
		, mStairwell(stairwell)
		, mLevelOffset(levelOffset)
	{
	}

	shared_ptr<Stairwell> StairwellVertex::getStairwell() const
	{
		return mStairwell;
	}

	string StairwellVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("StairwellVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> StairwellVertex::copyWithoutEdges()
	{
		return make_shared<StairwellVertex>(
			getId(),
			getSector(),
			getStairwell(),
			getSectorOffset().x,
			getSectorOffset().y,
			mLevelOffset
		);
	}

} // core