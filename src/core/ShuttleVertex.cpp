#include "core/Defines.h"
#include "core/ShuttleVertex.h"
#include "core/Location.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	ShuttleVertex
	-------------

	This Vertex is used at each end of a Shuttle.  It is functionally similar to a LiftVertex, except of course the
	Vertices are arranged from left to right, not bottom to top.
	*/

	ShuttleVertex::ShuttleVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<Shuttle> shuttle, float xOffset, float yOffset, uint32_t stopOffset)
		: Vertex(id, sector->getType() == SectorType::Location ? VertexType::Location : VertexType::Shuttle, VertexSubType::Shuttle, sector, xOffset, yOffset)
		, mShuttle(shuttle)
		, mStopOffset(stopOffset)
	{
	}

	ShuttleVertex::ShuttleVertex(shared_ptr<Sector> sector, shared_ptr<Shuttle> shuttle, float xOffset, float yOffset, uint32_t stopOffset)
		: Vertex(sector->getType() == SectorType::Location ? VertexType::Location : VertexType::Shuttle, VertexSubType::Shuttle, sector, xOffset, yOffset)
		, mShuttle(shuttle)
		, mStopOffset(stopOffset)
	{
	}

	shared_ptr<Shuttle> ShuttleVertex::getShuttle() const
	{
		return mShuttle;
	}

	string ShuttleVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("(ShuttleVertex at {},{} for Location {})", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> ShuttleVertex::copyWithoutEdges()
	{
		return make_shared<ShuttleVertex>(
			getId(),
			getSector(),
			getShuttle(),
			getSectorOffset().x,
			getSectorOffset().y,
			mStopOffset
		);
	}

} // core