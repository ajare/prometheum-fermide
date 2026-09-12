#include "core/Defines.h"
#include "core/WindowVertex.h"
#include "core/Location.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	
	WindowVertex
	------------

	Implementation of Vertex for the vertex at a Window.  This is used so that an Agent can stop
	at a Window, either to use it in some way (open/close/smash) or just pause and look out.
	*/

	WindowVertex::WindowVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<const Window> Window, float xLocationOffset, float yLocationOffset)
		: Vertex(id, VertexType::Location, VertexSubType::Window, sector, xLocationOffset, yLocationOffset)
		, mWindow(Window)
	{
	}

	WindowVertex::WindowVertex(shared_ptr<Sector> sector, shared_ptr<const Window> Window, float xLocationOffset, float yLocationOffset)
		: Vertex(VertexType::Location, VertexSubType::Window, sector, xLocationOffset, yLocationOffset)
		, mWindow(Window)
	{
	}

	shared_ptr<const Window> WindowVertex::getWindow() const
	{
		return mWindow;
	}

	string WindowVertex::getDescription() const
	{
		auto pos = getPosition();
		auto loc = getSector();

		return format("WindowVertex at {},{} for Location {}", pos.x, pos.y, loc->getDescription());
	}

	shared_ptr<Vertex> WindowVertex::copyWithoutEdges()
	{
		return make_shared<WindowVertex>(
			getId(), 
			getSector(), 
			getWindow(), 
			getSectorOffset().x, 
			getSectorOffset().y
		);
	}

} // core