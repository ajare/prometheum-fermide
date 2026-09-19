#include "core/Defines.h"
#include "core/LadderVertex.h"
#include "core/Location.h"
#include "core/SectorType.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	LadderVertex
	------------

	This Vertex is used at each end of a Ladder.  For each Ladder, there will be a pair, connected by a LadderEdge.
	*/

	LadderVertex::LadderVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<Ladder> ladder, float xOffset, float yOffset, int level)
		: Vertex(id, isLocationLike(sector->getType()) ? VertexType::Location : VertexType::Ladder, VertexSubType::Ladder, sector, xOffset, yOffset)
		, mLadder(ladder)
		, mLevel(level)
	{
	}

	LadderVertex::LadderVertex(shared_ptr<Sector> sector, shared_ptr<Ladder> ladder, float xOffset, float yOffset, int level)
		: Vertex(isLocationLike(sector->getType()) ? VertexType::Location : VertexType::Ladder, VertexSubType::Ladder, sector, xOffset, yOffset)
		, mLadder(ladder)
		, mLevel(level)
	{
	}

	shared_ptr<Ladder> LadderVertex::getLadder() const
	{
		return mLadder;
	}

	string LadderVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("LadderVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> LadderVertex::copyWithoutEdges()
	{
		return make_shared<LadderVertex>(
			getId(),
			getSector(),
			getLadder(),
			getSectorOffset().x,
			getSectorOffset().y,
			mLevel
		);
	}

} // core