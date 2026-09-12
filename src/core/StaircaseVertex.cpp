#include "core/Defines.h"
#include "core/StaircaseVertex.h"
#include "core/Location.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	StaircaseVertex
	---------------

	This Vertex is used at each end of a Staircase, and also within to provide navigation in a zig-zag line.  It connects
	to other StaircaseVertices via a StaircaseEdge, and StaircaseLocationVertices via StaircaseMountEdge.
	*/

	StaircaseVertex::StaircaseVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<Staircase> staircase, float xOffset, float yOffset, uint32_t deckOffset)
		: Vertex(id, sector->getType() == SectorType::Location ? VertexType::Location : VertexType::Staircase, VertexSubType::Staircase, sector, xOffset, yOffset)
		, mStaircase(staircase)
		, mDeckOffset(deckOffset)
	{
	}

	StaircaseVertex::StaircaseVertex(shared_ptr<Sector> sector, shared_ptr<Staircase> staircase, float xOffset, float yOffset, uint32_t deckOffset)
		: Vertex(sector->getType() == SectorType::Location ? VertexType::Location : VertexType::Staircase, VertexSubType::Staircase, sector, xOffset, yOffset)
		, mStaircase(staircase)
		, mDeckOffset(deckOffset)
	{
	}

	shared_ptr<Staircase> StaircaseVertex::getStaircase() const
	{
		return mStaircase;
	}

	string StaircaseVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("StaircaseVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> StaircaseVertex::copyWithoutEdges()
	{
		return make_shared<StaircaseVertex>(
			getId(),
			getSector(),
			getStaircase(),
			getSectorOffset().x,
			getSectorOffset().y,
			mDeckOffset
		);
	}

} // core