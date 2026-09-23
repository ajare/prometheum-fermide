#include <cassert>

#include "core/Defines.h"
#include "core/WindowSectorObject.h"
#include "core/WindowVertex.h"
#include "core/Window.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	WindowSectorObject
	--------------------

	Wrapper for a Window.  This creates and manages the Window instance.  As a Window technically spans
	two Locations, the Fore Location is used as the primary one.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	*/
	WindowSectorObject::WindowSectorObject(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t levelsHigh, shared_ptr<const Sector> sectors[2], uint32_t * vertexIdentifer)
		: SectorObject(SectorObjectType::Window, sectors[0], cellX, cellY, cellsWide, levelsHigh, make_shared<Window>(cellX, cellY, cellsWide, levelsHigh, sectors), vertexIdentifer)
	{
	}

	/***

	getWindow()
	-----------

	Get the Window instance.
	*/
	shared_ptr<Window> WindowSectorObject::getWindow() const
	{
		return static_pointer_cast<Window>(_getObject());
	}


	/***
	
	createVertex()
	-------------

	Creates a Vertex to be placed in front of the Window.  This can be used for opening/closing the Window,
	or just looking of it.  The Vertex is placed at ground level, in the middle of the Window.

	Arguments:

	- object is actually a shared_ptr to this WindowSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	*/
	shared_ptr<Vertex> WindowSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* /* user */) const
	{
		ASSERT_PTR_EQ_THIS(object);

		auto windowCentre = getWindow()->getCellsWide() / 2.0f;

		float xOffset = (float)(getCellX() - sector->getCellX()) + windowCentre;
		float yOffset = (float)(getCellY() - sector->getCellY());

		auto vertex = make_shared<WindowVertex>(sector, dynamic_pointer_cast<WindowSectorObject>(object)->getWindow(), xOffset, yOffset);

		vertex->setObject(object->_getObject());
		return vertex;
	}

} // core