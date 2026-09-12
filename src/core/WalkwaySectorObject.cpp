#include <cassert>

#include "core/Defines.h"
#include "core/WalkwaySectorObject.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	WalkwaySectorObject
	---------------------

	Wrapper for a Walkway.  This creates and manages the Walkway instance.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	WalkwaySectorObject::WalkwaySectorObject(uint32_t cellX, uint32_t cellY, shared_ptr<const Sector> sector, uint32_t* vertexIdentifer)
		: SectorObject(SectorObjectType::Walkway, sector, cellX, cellY, 1, 1, make_shared<Walkway>(cellX, cellY), vertexIdentifer)
	{
	}

	/***

	getWalkway()
	------------

	Get the Walkway instance.
	*/
	shared_ptr<const Walkway> WalkwaySectorObject::getWalkway() const
	{
		return static_pointer_cast<Walkway>(_getObject());
	}

	/***

	createVertex()
	-------------

	Creates a Vertex to be placed in front of the Walkway.  This can be used for opening/closing the Walkway,
	or just looking of it.  The Vertex is placed at ground level, in the middle of the Walkway.

	Arguments:

	- object is actually a shared_ptr to this WalkwaySectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	*/
	shared_ptr<Vertex> WalkwaySectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* user) const
	{
		ASSERT_PTR_EQ_THIS(object);

		throw NotImplementedException("Walkways do not create Vertices");
	}

} // core