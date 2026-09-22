#include <cassert>

#include "core/Defines.h"
#include "core/MarkerSectorObject.h"
#include "core/SectorMarkerVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	MarkerSectorObject
	---------------------

	Wrapper for a Marker.  This creates and manages the Marker instance.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	MarkerSectorObject::MarkerSectorObject(MarkerId id, string name, uint32_t cellX,
		uint32_t cellY, shared_ptr<const Sector> sector, float xOffset, uint32_t* vertexIdentifer)
		: SectorObject(SectorObjectType::Marker, sector, cellX, cellY, 1, 1,
			shared_ptr<Marker>(new Marker(id, std::move(name), cellX, cellY, xOffset)),
			vertexIdentifer)
	{
	}

	/***

	getMarker()
	------------

	Get the Marker instance.
	*/
	shared_ptr<const Marker> MarkerSectorObject::getMarker() const
	{
		return static_pointer_cast<Marker>(_getObject());
	}

	/***

	createVertex()
	-------------

	Creates a Vertex to be placed in front of the Marker.  This can be used for opening/closing the Marker,
	or just looking of it.  The Vertex is placed at ground level, in the middle of the Marker.

	Arguments:

	- object is actually a shared_ptr to this MarkerSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	*/
	shared_ptr<Vertex> MarkerSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* /* user */) const
	{
		ASSERT_PTR_EQ_THIS(object);

		auto marker = static_pointer_cast<Marker>(object->_getObject());

		float xOffset = (float)(getCellX() - sector->getCellX()) + marker->getOffset();
		float yOffset = (float)(getCellY() - sector->getCellY());

		auto vertex = make_shared<SectorMarkerVertex>(sector, xOffset, yOffset);

		vertex->setObject(object->_getObject());
		return vertex;
	}

} // core