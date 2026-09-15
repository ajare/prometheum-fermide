#include <format>
#include <algorithm>
#include <cassert>

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorObjectVertex.h"
#include "core/Exceptions.h"

// General SectorObjects
#include "core/BulkheadDoorSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/WindowSectorObject.h"

// Button SectorObjects
#include "core/ButtonSectorObject.h"


namespace core
{

	using namespace std;

	Sector::Sector(SectorType type, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY, float xCellOffset, float yCellOffset, float width, float height, string const& name, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, uint32_t capacity)
		: Area(cellX, cellY, xCellOffset, yCellOffset, width, height)
		, mType(type)
		, mLayerIndex(layerIndex)
		, mIndex(index)
		, mCellsWide(cellsWide)
		, mDecksHigh(decksHigh)
		, mTopDeckHeight(topDeckHeight)
		, mName(name)
		, mCapacity(capacity)
		, mLightsOn(true)
	{
		mEnds.resize(decksHigh);
	}

	SectorType Sector::getType() const
	{
		return mType;
	}

	uint32_t Sector::getLayerIndex() const
	{
		return mLayerIndex;
	}

	uint32_t Sector::getIndex() const
	{
		return mIndex;
	}

	uint32_t Sector::getCellX0() const
	{
		return getCellX();
	}

	uint32_t Sector::getCellX1() const
	{
		return getCellX() + getCellsWide() - 1;
	}

	uint32_t Sector::getCellY0() const
	{
		return getCellY();
	}

	uint32_t Sector::getCellY1() const
	{
		return getCellY() + getDecksHigh() - 1;
	}

	uint32_t Sector::getCellsWide() const
	{
		return mCellsWide;
	}

	uint32_t Sector::getDecksHigh() const
	{
		return mDecksHigh;
	}

	float Sector::getDeckHeight(uint32_t deckIndex) const
	{
		if (deckIndex == getDecksHigh() - 1)
		{
			return getTopDeckHeight();
		}
		else
		{
			return 1;
		}
	}

	float Sector::getTopDeckHeight() const
	{
		return mTopDeckHeight;
	}

	string const& Sector::getName() const
	{
		return mName;
	}

	uint32_t Sector::getCapacity() const
	{
		return mCapacity;
	}

	SectorEndType Sector::getEndType(uint32_t deckIndex, int side) const
	{
		assert(deckIndex < getDecksHigh());
		ASSERT_SIDE_OK(side);

		return mEnds[deckIndex].end[side];
	}

	uint32_t Sector::addSectorObject(shared_ptr<SectorObject> object)
	{
		auto index = (uint32_t)mObjects.size();

		mObjects.push_back(object);

		return index;
	}

	uint32_t Sector::getNumObjects() const
	{
		return (uint32_t)mObjects.size();
	}

	shared_ptr<SectorObject> Sector::_getObject(uint32_t index)
	{
		assert(index != ~0u && index <= getNumObjects());

		return mObjects[index];
	}

	shared_ptr<SectorObject> Sector::getObject(uint32_t index) const
	{
		assert(index != ~0u && index <= getNumObjects());

		return mObjects[index];
	}

	vector<shared_ptr<SectorObject>> Sector::getSortedObjects(SectorObjectSortFunction sortFunc) const
	{
		auto sortedObjects = mObjects;
		sortedObjects.erase(remove(sortedObjects.begin(), sortedObjects.end(), nullptr),
			sortedObjects.end());

		sort(sortedObjects.begin(), sortedObjects.end(), sortFunc);

		return sortedObjects;
	}

	shared_ptr<const Object> Sector::getObjectAtPosition(float x, float y,
		shared_ptr<const SectorObject>* sectorObject) const
	{
		for (auto const& object : mObjects)
		{
			if (!object || !object->pointInside(x, y)) continue;
			if (sectorObject) *sectorObject = object;
			return object->_getObject();
		}
		return nullptr;
	}

	SectorPosition Sector::findFreeAgentPosition(Agent const* /* agent */) const
	{
		// For now just place in random cell on first level
		float x = (rand() % getCellsWide()) + 0.5f;
		float y = 0.0f;

		return SectorPosition(this, x, y);
	}

	uint32_t Sector::createDoor(shared_ptr<const Sector> sector, shared_ptr<const Sector> backSector, uint32_t x, uint32_t cellsWide, uint32_t* vertexIdentifier)
	{
		ASSERT_PTR_EQ_THIS(sector);
		assert(x >= getCellX0() && x <= getCellX1());

		shared_ptr<const Sector> sectors[2] = { sector, backSector };
		auto door = make_shared<DoorSectorObject>(x, getCellY(), cellsWide, sectors, vertexIdentifier);

		return addSectorObject(door);
	}

	void Sector::addDoor(shared_ptr<DoorSectorObject> door)
	{
		[[maybe_unused]] auto x = door->getCellX();

		assert(x >= getCellX0() && x <= getCellX1());

		addSectorObject(door);
	}

	uint32_t Sector::createWindow(shared_ptr<const Sector> sector, shared_ptr<const Sector> backSector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, uint32_t* vertexIdentifier)
	{
		ASSERT_PTR_EQ_THIS(sector);
		assert(x >= getCellX0() && x <= getCellX1());
		assert(y >= getCellY0() && y <= getCellY1());

		shared_ptr<const Sector> sectors[2] = { sector, backSector };
		auto window = make_shared<WindowSectorObject>(x, y, cellsWide, decksHigh, sectors, vertexIdentifier);

		return addSectorObject(window);
	}

	uint32_t Sector::addWindow(shared_ptr<WindowSectorObject> window)
	{
		[[maybe_unused]] auto x = window->getCellX();

		assert(x >= getCellX0() && x <= getCellX1());

		return addSectorObject(window);
	}

	uint32_t Sector::createPhysicalControl(shared_ptr<const Sector> sector, string const& name, uint32_t x, uint32_t y, float xOffset, float yOffset, uint32_t flags, uint32_t* vertexIdentifier)
	{
		ASSERT_PTR_EQ_THIS(sector);

		shared_ptr<SectorObject> inter;

		auto anchorType = mType == SectorType::Ladder ? ButtonAnchorType::UnAnchored : ButtonAnchorType::Ground;

		inter = make_shared<ButtonSectorObject>(name, x, y, xOffset, yOffset, anchorType, sector, flags, vertexIdentifier);
		return addSectorObject(inter);
	}

	uint32_t Sector::createWalkway(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t* vertexIdentifier)
	{
		ASSERT_PTR_EQ_THIS(sector);

		auto walkway = make_shared<WalkwaySectorObject>(x, y, sector, vertexIdentifier);

		return addSectorObject(walkway);
	}

	uint32_t Sector::createMarker(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, float xOffset, uint32_t* vertexIdentifier)
	{
		ASSERT_PTR_EQ_THIS(sector);

		auto marker = make_shared<MarkerSectorObject>(x, y, sector, xOffset, vertexIdentifier);

		return addSectorObject(marker);
	}

	bool Sector::removeSectorObject(uint32_t index)
	{
		if (index >= mObjects.size() || !mObjects[index]) return false;
		mObjects[index].reset();
		while (!mObjects.empty() && !mObjects.back()) mObjects.pop_back();
		return true;
	}

	uint32_t Sector::createForceBridge(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t size, int fromSide, bool extensible, bool startExtended)
	{
		ASSERT_PTR_EQ_THIS(sector);

		auto forceBridge = make_shared<ForceBridgeSectorObject>(x, y, size, fromSide, sector, extensible, startExtended);

		return addSectorObject(forceBridge);
	}

	uint32_t Sector::createLadder(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, bool extensible, bool startExtended, uint32_t decksHigh, uint32_t* vertexIdentifier)
	{
		ASSERT_PTR_EQ_THIS(sector);

		auto ladder = make_shared<LadderSectorObject>(x, y, decksHigh, sector, extensible, startExtended, vertexIdentifier);

		return addSectorObject(ladder);
	}

	uint32_t Sector::createPlatformLift(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, vector<uint32_t> const& stopOffsets, uint32_t* vertexIdentifier)
	{
		ASSERT_PTR_EQ_THIS(sector);

		auto lift = make_shared<LiftSectorObject>(LiftSectorObjectType::PlatformLift, x, y, cellsWide, stopOffsets, sector, vertexIdentifier);

		return addSectorObject(lift);
	}

	void Sector::setEndType(uint32_t deckIndex, int side, SectorEndType type)
	{
		string caller = format("Sector::setEndType({}, {}, {})", deckIndex, side, (int)type);

		if (deckIndex >= getDecksHigh())
		{
			throw SectorException(this, format("{} - deckIndex={} out of bounds", caller, deckIndex));
		}

		if (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT)
		{
			throw SectorException(this, format("{} - side={} invalid: must be 0 or 1.", caller, deckIndex));
		}

		mEnds[deckIndex].end[side] = type;
	}

	void Sector::addEndWall(uint32_t deckIndex, int side)
	{
		setEndType(deckIndex, side, SectorEndType::Wall);
	}

	void Sector::removeEndWall(uint32_t deckIndex, int side)
	{
		setEndType(deckIndex, side, SectorEndType::None);
	}

	uint32_t Sector::createBulkheadDoor(shared_ptr<const Sector> sector, shared_ptr<const Sector> rightSector, uint32_t deckIndex, int side)
	{
		ASSERT_PTR_EQ_THIS(sector);
		assert(deckIndex < getDecksHigh());
		ASSERT_SIDE_OK(side);

		uint32_t x;

		if (side == CORE_SIDE_LEFT)
		{
			x = getCellX1();
		}
		else
		{
			x = getCellX0();
			swap(sector, rightSector);
		}

		shared_ptr<const Sector> sectors[2] = { sector, rightSector };
		auto door = make_shared<BulkheadDoorSectorObject>(x, getCellY() + deckIndex, sectors);

		auto doorIndex = addSectorObject(door);

		setEndType(deckIndex, 1 - side, SectorEndType::BulkheadDoor);
		return doorIndex;
	}

	void Sector::addBulkheadDoor(shared_ptr<BulkheadDoorSectorObject> door, uint32_t deckIndex, int side)
	{
		setEndType(deckIndex, 1 - side, SectorEndType::BulkheadDoor);
		addSectorObject(door);
	}

	bool Sector::areLightsOn() const
	{
		return mLightsOn;
	}

	set<Agent*> const& Sector::getAgents() const
	{
		return mAgents;
	}

	void Sector::enterAgent(Agent* agent, SectorPosition const& pos, bool authored)
	{
		agent->setPosition(pos, authored);

		[[maybe_unused]] auto inserted = mAgents.insert(agent);
		assert(inserted.second && "Agent already in Sector!");
	}

	void Sector::enterAgent(Agent* agent, uint32_t deckIndex, float xOffset)
	{
		Vector2 offset{ xOffset, (float)deckIndex };
		SectorPosition pos(this, offset);

		enterAgent(agent, pos);
	}

	void Sector::enterAgent(Agent* agent, shared_ptr<const Vertex> vertex, Vector2 const& offset)
	{
		SectorPosition pos;

		if (vertex)
		{
			if (vertex->getSector().get() != this)
			{
				throw Exception("Vertex is not within the given Sector!");
			}

			auto vertexPos = (vertex->getPosition() - getPosition()) + offset;
			pos = SectorPosition(this, vertexPos);
		}
		else
		{
			pos = findFreeAgentPosition(agent);
		}

		enterAgent(agent, pos);
	}

	shared_ptr<Edge> Sector::exitAgent(Agent* agent)
	{
		[[maybe_unused]] auto erased = mAgents.erase(agent);
		assert(erased == 1 && "Agent not found in Sector!");

		return {};
	}

	bool Sector::lightsOn()
	{
		mLightsOn = true;
		return true;
	}

	bool Sector::lightsOff()
	{
		mLightsOn = false;
		return true;
	}

	bool Sector::toggleLights()
	{
		if (areLightsOn())
		{
			return lightsOff();
		}
		else
		{
			return lightsOn();
		}
	}

	void Sector::advanceResources(float frameTime)
	{
		for (auto const& object : mObjects)
		{
			if (object) object->update(frameTime);
		}

		updateImpl(frameTime);
	}

	void Sector::update(float frameTime)
	{
		advanceResources(frameTime);

		for (auto agent : mAgents)
		{
			agent->update(frameTime);
		}
	}

} // core