#include <cassert>

#include "core/Defines.h"
#include "core/LiftSectorObject.h"
#include "core/PlatformLiftVertex.h"
#include "core/LiftVertex.h"
#include "core/LiftEdge.h"
#include "core/PlatformLift.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	shared_ptr<Object> createLiftSubclass(LiftSectorObjectType type, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, vector<uint32_t> const& stopOffsets, shared_ptr<const Sector> sector)
	{
		switch (type)
		{
		case LiftSectorObjectType::PlatformLift:
			return make_shared<PlatformLift>(cellX, cellY, cellsWide, stopOffsets);

		default:
			throw UnhandledException(type, "LiftSectorObjectType");
		}
	}

	/***

	LiftSectorObject
	---------------------

	Wrapper for a Lift.  This creates and manages the Lift instance.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	LiftSectorObject::LiftSectorObject(LiftSectorObjectType type, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, vector<uint32_t> const& stopOffsets, shared_ptr<const Sector> sector, uint32_t* vertexIdentifer)
		: SectorObject(SectorObjectType::Lift, sector, cellX, cellY, cellsWide, stopOffsets.back() + 1,
			createLiftSubclass(type, cellX, cellY, cellsWide, stopOffsets, sector), vertexIdentifer)
		, VerticalEdgeCreator()
		, mType(type)
	{
	}

	/***

	getLift()
	-----------

	Get the Lift instance.
	*/
	shared_ptr<Lift> LiftSectorObject::getLift() const
	{
		return static_pointer_cast<Lift>(_getObject());
	}

	/***

	createCrossDeckEdge()
	---------------------

	Create an Edge between two Decks.

	Arguments:

	- edgeCreator is actually a shared_ptr to this LiftSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Edge.
	*/
	shared_ptr<Edge> LiftSectorObject::createCrossDeckEdge(shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);

		return make_shared<LiftEdge>(getLift());
	}

	/***

	createVertex()
	-------------

	Creates a Vertex to be placed in front of the Lift.  There are actually two Vertices to be placed at each point,
	one Location Vertex (on the front Layer) and one Lift Vertex (on the back Layer): this method creates the front one.

	Arguments:

	- object is actually a shared_ptr to this LiftSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	*/
	shared_ptr<Vertex> LiftSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* user) const
	{
		uint32_t stopOffset= *(static_cast<uint32_t*>(user));

		ASSERT_PTR_EQ_THIS(object);

		auto liftWidth = getSize().x;

		float xOffset = (float)(getCellX() - sector->getCellX()) + liftWidth * 0.5f;
		float yOffset = (float)(getCellY() - sector->getCellY()) + stopOffset;

		switch (mType)
		{
		case LiftSectorObjectType::PlatformLift:
		{
			auto vertex = make_shared<PlatformLiftVertex>(
				sector,
				getLift(),
				xOffset, yOffset,
				stopOffset
			);

			vertex->setObject(object->_getObject());
			return vertex;
		}
		default:
			throw UnhandledException(mType, "LiftSectorObjectType");
		}

	}

} // core