#include "core/SectorObject.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	uint32_t SectorObject::VertexIdentifierGenerator = 0;

	SectorObject::SectorObject(SectorObjectType type, shared_ptr<const Sector> sector, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh, shared_ptr<Object> object, uint32_t* vertexIdentifer)
		: Area(cellX, cellY, 0.0f, 0.0f, (float)cellsWide, (float)decksHigh)
		, mObjectType(type)
		, mSector(sector)
		, mObject(object)
		, mVertexIdentifier(~0u)
	{
		if (vertexIdentifer)
		{
			mVertexIdentifier = VertexIdentifierGenerator++;
			*vertexIdentifer = mVertexIdentifier;
		}
	}

	SectorObjectType SectorObject::getObjectType() const
	{
		return mObjectType;
	}

	shared_ptr<const Sector> SectorObject::getSector() const
	{
		return mSector;
	}

	shared_ptr<Object> SectorObject::_getObject() const
	{
		return mObject;
	}

	string SectorObject::getDescription() const
	{
		return mObject->getDescription();
	}

	uint32_t SectorObject::getVertexIdentifier() const
	{
		return mVertexIdentifier;
	}

	bool SectorObject::hasController(string const& key) const
	{
		return mControllerLookup.find(key) != mControllerLookup.end();
	}

	shared_ptr<Controller> SectorObject::getController(string const& key) const
	{
		auto it = mControllerLookup.find(key);

		if (it != mControllerLookup.end())
		{
			return it->second;
		}
		else
		{
			throw Exception(format("Controller key '{}' not found in SectorObject Controller Map", key));
		}
	}

	void SectorObject::addController(string const& key, shared_ptr<Controller> controller)
	{
		auto it = mControllerLookup.find(key);

		if (it == mControllerLookup.end())
		{
			mControllerLookup[key] = controller;
		}
		else
		{
			throw Exception(format("Controller key '{}' already exists in SectorObject Controller Map", key));
		}
	}

	bool SectorObject::pointInside(float x, float y) const
	{
		return mObject->pointInShape(x, y);
	}

	shared_ptr<VertexController> SectorObject::createVertexController(Building const* building, vector<shared_ptr<Vertex>> const& vertices, map<shared_ptr<Controller>, shared_ptr<Vertex>> const& controllerVertexLookup) const
	{
		return nullptr;
	}


	void SectorObject::update(float frameTime)
	{
		mObject->update(frameTime);
	}

} // core