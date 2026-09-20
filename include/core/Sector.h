#pragma once

#include <string>
#include <vector>
#include <set>
#include <functional>

#include "core/Area.h"
#include "core/SectorType.h"
#include "core/SectorEnd.h"
#include "core/SectorObject.h"
#include "core/SectorPosition.h"
#include "core/Agent.h"
#include "core/DoorSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/WindowSectorObject.h"


namespace core
{
	class SectorObjectVertex;

	typedef std::function<bool(std::shared_ptr<SectorObject>, std::shared_ptr<SectorObject>)> SectorObjectSortFunction;

	class Sector : public Area
	{
		friend class Building;
		friend class Graph;
		// Committing a traversal transfers sector membership at the destination
		// endpoint with an explicitly authored, non-default position, which only
		// the private SectorPosition overload carries. The coordinator therefore
		// shares Building's friendship here rather than widening Sector's public
		// surface (ADR 0004).
		friend class SimulationCoordinator;

	private:

		SectorType mType;

		uint32_t mLayerIndex;

		uint32_t mIndex;

		uint32_t mCellsWide, mDecksHigh;

		float mTopDeckHeight;

		std::string mName;

		uint32_t mCapacity;

		bool mLightsOn;

		std::vector<std::shared_ptr<SectorObject>> mObjects;

		std::set<Agent*> mAgents;

	protected:

		std::vector<SectorEnd> mEnds;

	private:

		[[nodiscard]] std::shared_ptr<SectorObject> _getObject(uint32_t index);

		SectorPosition findFreeAgentPosition(Agent const* agent) const;

		// Following functions to be called by Building
		uint32_t createDoor(std::shared_ptr<const Sector> sector, std::shared_ptr<const Sector> backSector,
			uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh = 1,
			uint32_t* vertexIdentifier = nullptr);

		void addDoor(std::shared_ptr<DoorSectorObject> door);

		uint32_t createWindow(std::shared_ptr<const Sector> sector, std::shared_ptr<const Sector> backSector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, uint32_t* vertexIdentifier = nullptr);

		uint32_t addWindow(std::shared_ptr<WindowSectorObject> window);

		uint32_t createPhysicalControl(std::shared_ptr<const Sector> sector, std::string const& name, uint32_t x, uint32_t y, float xOffset, float yOffset, uint32_t flags, uint32_t* vertexIdentifier = nullptr);

		uint32_t createWalkway(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t* vertexIdentifier = nullptr);

		uint32_t createMarker(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, float xOffset, uint32_t* vertexIdentifier = nullptr);

		bool removeSectorObject(uint32_t index);

		uint32_t createForceBridge(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t size, int fromSide, bool extensible, bool startExtended);

		uint32_t createLadder(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, bool extensible, bool startExtended, uint32_t decksHigh, uint32_t* vertexIdentifier = nullptr);

		uint32_t createPlatformLift(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, std::vector<uint32_t> const& stopOffsets, uint32_t* vertexIdentifier = nullptr);

		uint32_t createBulkheadDoor(std::shared_ptr<const Sector> sector, std::shared_ptr<const Sector> rightLocation, uint32_t deckIndex, int side);

		void addBulkheadDoor(std::shared_ptr<BulkheadDoorSectorObject> door, uint32_t deckIndex, int side);

		void setEndType(uint32_t deckIndex, int side, SectorEndType type);

		void addEndWall(uint32_t deckIndex, int side);

		void removeEndWall(uint32_t deckIndex, int side);

		void enterAgent(Agent* agent, SectorPosition const& pos, bool authored = true);

		// Advance device and object state without moving agents. Building uses
		// this to keep resource work in the first deterministic tick phase.
		void advanceResources(float frameTime);

		// This is designed to be subclassed if required.
		virtual void updateImpl(float /* frameTime */) {}

	protected:

		uint32_t addSectorObject(std::shared_ptr<SectorObject> object);

	public:

		Sector(SectorType type, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY, float xCellOffset, float yCellOffset, float width, float height, std::string const& name, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, uint32_t capacity);

		virtual ~Sector() = default;

		[[nodiscard]] SectorType getType() const;

		[[nodiscard]] uint32_t getLayerIndex() const;

		[[nodiscard]] uint32_t getIndex() const;

		[[nodiscard]] uint32_t getCellX0() const;

		[[nodiscard]] uint32_t getCellX1() const;

		[[nodiscard]] uint32_t getCellY0() const;

		[[nodiscard]] uint32_t getCellY1() const;

		[[nodiscard]] uint32_t getCellsWide() const;

		[[nodiscard]] uint32_t getDecksHigh() const;

		[[nodiscard]] float getDeckHeight(uint32_t deckIndex) const;

		[[nodiscard]] float getTopDeckHeight() const;

		[[nodiscard]] std::string const& getName() const;

		[[nodiscard]] uint32_t getCapacity() const;

		[[nodiscard]] SectorEndType getEndType(uint32_t deckIndex, int side) const;

		[[nodiscard]] virtual std::string getDescription() const = 0;

		[[nodiscard]] uint32_t getNumObjects() const;

		[[nodiscard]] std::shared_ptr<SectorObject> getObject(uint32_t index) const;

		[[nodiscard]] std::vector<std::shared_ptr<SectorObject>> getSortedObjects(SectorObjectSortFunction sortFunc) const;

		[[nodiscard]] std::shared_ptr<const Object> getObjectAtPosition(float x, float y,
			std::shared_ptr<const SectorObject>* sectorObject = nullptr) const;

		[[nodiscard]] virtual bool sectorSupportsObjectType(SectorObjectType type) const = 0;

		// Whether this sector may act as the back side of a look-through (a Window on the
		// Layer in front may look into it). Distinct from hosting: a Background may host
		// nothing yet still be looked into. Defaults to the hosting rule.
		[[nodiscard]] virtual bool sectorSupportsObjectAsLookTarget(SectorObjectType type) const;

		[[nodiscard]] bool areLightsOn() const;

		[[nodiscard]] std::set<Agent*> const& getAgents() const;

		bool lightsOn();

		bool lightsOff();

		bool toggleLights();

		void enterAgent(Agent* agent, uint32_t deckIndex, float xOffset);

		void enterAgent(Agent* agent, std::shared_ptr<const Vertex> vertex = {}, Vector2 const& offset = { 0, 0 });

		std::shared_ptr<Edge> exitAgent(Agent* agent);

		void update(float frameTime);
	};

} // core
