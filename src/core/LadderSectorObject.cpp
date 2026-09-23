#include <cassert>

#include "core/Defines.h"
#include "core/LadderSectorObject.h"
#include "core/SectorObjectVertex.h"
#include "core/LadderVertex.h"
#include "core/LadderEdge.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	LadderSectorObject
	---------------------

	Wrapper for a Ladder.  This creates and manages the Ladder instance.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	LadderSectorObject::LadderSectorObject(uint32_t cellX, uint32_t cellY, uint32_t endpointsHigh, shared_ptr<const Sector> sector, bool extensible, bool startExtended, uint32_t* vertexIdentifer)
		: SectorObject(SectorObjectType::Ladder, sector, cellX, cellY, 1, endpointsHigh, make_shared<Ladder>(cellX, cellY, endpointsHigh, extensible, startExtended), vertexIdentifer)
		, VerticalEdgeCreator()
	{
	}

	/***

	getLadder()
	-----------

	Get the Ladder instance.
	*/
	shared_ptr<Ladder> LadderSectorObject::getLadder() const
	{
		return static_pointer_cast<Ladder>(_getObject());
	}

	/***

	createCrossLevelEdge()
	---------------------

	Create an Edge between two Levels.

	Arguments:

	- edgeCreator is actually a shared_ptr to this LadderSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Edge.
	*/
	shared_ptr<Edge> LadderSectorObject::createCrossLevelEdge([[maybe_unused]] shared_ptr<VerticalEdgeCreator> edgeCreator) const
	{
		ASSERT_PTR_EQ_THIS(edgeCreator);

		return make_shared<LadderEdge>(getLadder());
	}

	/*
	
	createVertex()
	-------------

	Creates a Vertex to be placed in front of the Ladder.  There are actually two Vertices to be placed at each point,
	one Location Vertex (on the front Layer) and one Ladder Vertex (on the back Layer): this method creates the front one.

	Arguments:

	- object is actually a shared_ptr to this LadderSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	*/
	shared_ptr<Vertex> LadderSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* user) const
	{
		int endpoint = *(static_cast<int*>(user));

		ASSERT_PTR_EQ_THIS(object);
		ASSERT_LADDER_ENDPOINT_OK(endpoint);

		float xOffset = (float)(getCellX() - sector->getCellX()) + 0.5f;
		float yOffset = (float)(getCellY() - sector->getCellY());

		if (endpoint == CORE_LADDER_ENDPOINT_HIGH)
		{
			yOffset += (getLadder()->getLevelsHigh() - 1);
		}

		auto vertex = make_shared<SectorObjectVertex>(
			VertexSubType::Ladder, 
			sector,
			object, 
			xOffset, yOffset
		);

		vertex->setObject(object->_getObject());
		return vertex;
	}

} // core