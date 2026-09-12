#include "core/Defines.h"
#include "core/LiftVertex.h"
#include "core/Location.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	LiftVertex
	----------

	This Vertex is used at each stop of a Lift.  The Vertices are connected in a chain from the lowest Lift stop to
	the highest, via LiftEdges.
	*/

	LiftVertex::LiftVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<Lift> lift, float xOffset, float yOffset, uint32_t stopOffset)
		: Vertex(id, sector->getType() == SectorType::Location ? VertexType::Location : VertexType::Lift, VertexSubType::Lift, sector, xOffset, yOffset)
		, mLift(lift)
		, mStopOffset(stopOffset)
	{
	}

	LiftVertex::LiftVertex(shared_ptr<Sector> sector, shared_ptr<Lift> lift, float xOffset, float yOffset, uint32_t stopOffset)
		: Vertex(sector->getType() == SectorType::Location ? VertexType::Location : VertexType::Lift, VertexSubType::Lift, sector, xOffset, yOffset)
		, mLift(lift)
		, mStopOffset(stopOffset)
	{
	}

	shared_ptr<Lift> LiftVertex::getLift() const
	{
		return mLift;
	}

	string LiftVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("LiftVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> LiftVertex::copyWithoutEdges()
	{
		return make_shared<LiftVertex>(
			getId(),
			getSector(),
			getLift(),
			getSectorOffset().x,
			getSectorOffset().y,
			mStopOffset
		);
	}

} // core