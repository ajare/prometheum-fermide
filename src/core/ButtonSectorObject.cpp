#include <cassert>

#include "core/Defines.h"
#include "core/ButtonSectorObject.h"
#include "core/SectorObjectVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;
	
	/***

	ButtonSectorObject
	------------------

	Wrapper for a Button.  This creates and manages the Button instance.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	*/
	ButtonSectorObject::ButtonSectorObject(string const& name, uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, ButtonAnchorType anchorType, shared_ptr<const Sector> sector, uint32_t buttonFlags, uint32_t* vertexIdentifer)
		: SectorObject(SectorObjectType::InteractionPoint, sector, cellX, cellY, 1, 1,
			make_shared<Button>(name, cellX, cellY, xOffset, yOffset, buttonFlags), vertexIdentifer)
		, mAnchorType(anchorType)
	{
	}

	/***

	createVertex()
	-------------

	Creates a Vertex to be placed in front of the Button. 

	Arguments:

	- object is actually a shared_ptr to this ButtonSectorObject instance.  While this is awkward, it lets us
	  capture the shared_ptr rather than the raw one, within the Vertex.
	*/
	shared_ptr<Vertex> ButtonSectorObject::createVertex(shared_ptr<SectorObject> object, shared_ptr<Sector> sector, void* user) const
	{
		ASSERT_PTR_EQ_THIS(object);

		auto button = _getObject();

		auto const& pos = button->getPosition();
		auto const& size = button->getSize();

		float xOffset, yOffset, height;

		switch (mAnchorType)
		{
		case ButtonAnchorType::Ground:
			xOffset = (button->getPosition().x - sector->getCellX()) + size.x * 0.5f;
			yOffset = (float)(getCellY() - sector->getCellY());
			height = CORE_BUTTON_Y_OFFSET + size.y;
			break;

		case ButtonAnchorType::UnAnchored:
			xOffset = (pos.x - sector->getCellX()) + size.x * 0.5f;
			yOffset = (pos.y - sector->getCellY()) + size.y * 0.5f;
			height = size.y;
			break;

		default:
			throw UnhandledException(mAnchorType, "ButtonAnchorType");
		}

		auto vertex = make_shared<SectorObjectVertex>(
			VertexSubType::Interactable, sector, object, xOffset, yOffset);

		vertex->setObject(object->_getObject());
		return vertex;
	}

} // core