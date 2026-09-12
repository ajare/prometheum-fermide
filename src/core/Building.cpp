#include <algorithm>
#include <set>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "core/Defines.h"
#include "core/Building.h"
#include "core/Location.h"
#include "core/SectorType.h"
#include "core/SectorObjectType.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/StaircaseTransit.h"
#include "core/ButtonSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/ControllerSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/PlatformLift.h"
#include "core/ButtonDoorOrchestratedSystem.h"
#include "core/BulkheadDoorOrchestratedSystem.h"
#include "core/ButtonExtensibleObjectOrchestratedSystem.h"
#include "core/LiftOrchestratedSystem.h"
#include "core/PlatformLiftOrchestratedSystem.h"
#include "core/ShuttleOrchestratedSystem.h"
#include "core/LightingOrchestratedSystem.h"
#include "core/VertexController.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	Building::CreateDoorOptions Building::ManualDoor1Options{ 1, { false, false }, false };
	Building::CreateDoorOptions Building::OrchButtonDoor1Options{ 1, { true, true }, true };
	Building::CreateDoorOptions Building::NonOrchButtonDoor1Options{ 1, { true, true }, false };
	Building::CreateDoorOptions Building::ManualDoor2Options{ 2, { false, false }, false };
	Building::CreateDoorOptions Building::OrchButtonDoor2Options{ 2, { true, true }, true };
	Building::CreateDoorOptions Building::NonOrchButtonDoor2Options{ 2, { true, true }, false };

	/*
	Building
	--------

	This class essentially holds a game map, with all the sub-structures within it.
	
	It is responsible for map creation, acting as a facade for sub-structures such as
	Sector, Location and SectorObject.
	
	It also generates a Graph which is the master path-finding source.  While each Agent
	may have their own internal Graph, Building's is the one which these are initially 
	generated from.

	A Building has two Layers, and there is quite a bit of hard-coding and reliance around
	this, which is to say that increasing to three or more would be a lot of work.

	One important concept to bear in mind is the API difference between "y" and "deckIndex".
	"y" is used as an absolute value within the Layer, whereas "deckIndex" is used as an absolute
	value within a Sector, ie it is relative to a Sector's base y offset within the Layer.

	Buildings are created piece by piece, and must be valid at every stage of their construction.
	There is no post-build validation, this happens after each construction command.

	Rules for creation of Buildings:
	
	There are two types of Sector: Locations and Transits

	Locations:
	- Locations can go on either Layer
	- Locations on different Layers connect to each other via Doors
	- Locations on the same Layer connect to each other via BulkheadDoors
	
	Transits:
	- Transits can only go on the Back Layer, and are designed to connect Locations

	Any object which connects Locations - eg Doors and Transits - need to be placed after the two
	Locations being connected are placed.

	Windows:
	- Windows can be placed on either Layer.  If they are placed on the Back Layer, they will show
	  space/the void.  The same if they are placed on the Fore Layer with no Sector behind them.

	Buttons etc which control objects (eg Doors)
	- These are placed on the right of the object by default, unless there is no space, in which case
	  they are placed on the left (as viewed by the player).

	*/

	Building::Building(string const& name, uint32_t cellsWide, uint32_t decksHigh)
		: mName(name)
		, mCellsWide(cellsWide)
		, mDecksHigh(decksHigh)
	{
		for (uint32_t i = 0; i < CORE_NUM_LAYERS; ++i)
		{
			mLayers[i] = make_shared<Layer>(this, cellsWide, decksHigh, i);
		}

		mGraph = make_shared<Graph>(this);
		mOrchestrator = make_shared<Orchestrator>();
	}

	Building::~Building() = default;

	string const& Building::getName() const
	{
		return mName;
	}

	uint32_t Building::getCellsWide() const
	{
		return mCellsWide;
	}

	uint32_t Building::getDecksHigh() const
	{
		return mDecksHigh;
	}

	uint32_t Building::getNumSectors() const
	{
		return (uint32_t)mSectors.size();
	}

	void Building::validateCellOccupied(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (!cellDef.occupied())
		{
			throw BuildingException(this, format("{} - cell at {},{} is not occupied.", caller, x, y));
		}
	}

	void Building::validateCellUnoccupied(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.occupied())
		{
			throw BuildingException(this, format("{} - cell at {},{} is occupied.", caller, x, y));
		}
	}

	void Building::validateCellIsInSector(string const& caller, uint32_t x, uint32_t y, shared_ptr<const Sector> sector) const
	{
		auto layer = getLayer(sector->getLayerIndex());

		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sectorIndex = sector->getIndex();

		if (cellDef.sectorIndex != sectorIndex)
		{
			throw BuildingException(this, format("{} - cell at {},{} is not in sector {}.", caller, x, y, sectorIndex));
		}
	}

	void Building::validateCellHasObject(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (!cellDef.hasObject())
		{
			throw BuildingException(this, format("{} - cell at {},{} does not have an object.", caller, x, y));
		}
	}

	void Building::validateCellHasNoObject(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.hasObject())
		{
			throw BuildingException(this, format("{} - cell at {},{} has an object.", caller, x, y));
		}
	}

	void Building::validateCellIsType(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, SectorType sectorType) const
	{
		validateCellOccupied(caller, layerIndex, x, y);

		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = getSector(cellDef.sectorIndex);

		if (sector->getType() != sectorType)
		{
			throw BuildingException(this, format("{} - Sector of cell at {},{} is not type '{}'.", caller, x, y, getSectorTypeString(sectorType)));
		}
	}

	void Building::validateCellHasDoor(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.sectorObjectType != SectorObjectType::Door)
		{
			throw BuildingException(this, format("{} - cell at {},{} does not have a door.", caller, x, y));
		}
	}

	void Building::validateCellHasNoDoor(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.sectorObjectType == SectorObjectType::Door)
		{
			throw BuildingException(this, format("{} - cell at {},{} has a door.", caller, x, y));
		}
	}

	void Building::validateCellHasController(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, int side) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.controllers[side] == ~0u)
		{
			throw BuildingException(this, format("{} - cell at {},{} (side {}) does not have a controller.", caller, x, y, side));
		}
	}

	void Building::validateCellHasNoController(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, int side) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.controllers[side] != ~0u)
		{
			throw BuildingException(this, format("{} - cell at {},{} (side {}) has a controller.", caller, x, y, side));
		}
	}


	void Building::validateCellHasNoFloorType(string const& caller, string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.floorType == CellFloorType::Ground)
		{
			throw BuildingException(this, format("{} - floor is ground, so cannot be {}", caller, desiredObject));
		}
		else if (cellDef.floorType == CellFloorType::ForceBridge)
		{
			throw BuildingException(this, format("{} - there is already a ForceBridge here", caller));
		}
		else if (cellDef.floorType == CellFloorType::Walkway)
		{
			throw BuildingException(this, format("{} - there is already a Walkway here", caller));
		}
	}

	void Building::validateCellTraversableOnFoot(string const& caller, string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (!cellDef.isTraversableOnFoot())
		{
			throw BuildingException(this, format("{} - cell at {},{} is not traversable, which blocks {} being placed", caller, x, y, desiredObject));
		}
	}

	void Building::validateLayer(string const& caller, uint32_t layerIndex) const
	{
		if (layerIndex >= CORE_NUM_LAYERS)
		{
			throw BuildingException(this, format("{} - layerIndex={} is out of bounds", caller, layerIndex));
		}
	}

	void Building::validateBounds(string const& caller, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const
	{
		if (x >= mCellsWide)
		{
			throw BuildingException(this, format("{} - x={} is out of bounds", caller, x));
		}

		if ((x + cellsWide) >= mCellsWide)
		{
			throw BuildingException(this, format("{} - cellsWide={} is out of bounds", caller, cellsWide));
		}

		if (y >= mDecksHigh)
		{
			throw BuildingException(this, format("{} - y={} is out of bounds", caller, y));
		}

		if ((y + decksHigh) >= mDecksHigh)
		{
			throw BuildingException(this, format("{} - decksHigh={} is out of bounds", caller, decksHigh));
		}
	}

	void Building::validateLayerSpace(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const
	{
		auto layer = getLayer(layerIndex);

		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				validateCellUnoccupied(caller, layerIndex, x, y);
			}
		}
	}

	void Building::validateObjectAllowedInSector(string const& caller, SectorObjectType type, uint32_t sectorIndex) const
	{
		auto sector = getSector(sectorIndex);

		if (!sector->sectorSupportsObjectType(type))
		{
			throw BuildingException(this, format("{} - Sector type '{}' does not support SectorObject type '{}'", caller, getSectorTypeString(sector->getType()), getSectorObjectTypeString(type)));
		}
	}

	void Building::validateSpaceOnlyInOneSector(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const
	{
		auto layer = getLayer(layerIndex);

		auto sectorIndex = layer->getCellDefinition(x, y).sectorIndex;

		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cellDef = layer->getCellDefinition(ix, iy);

				if (cellDef.sectorIndex != sectorIndex)
				{
					throw BuildingException(this, format("{} - bounds {},{} -> {},{} cross multiple Sectors", caller, x, y, x + cellsWide, y + decksHigh));
				}
			}
		}
	}

	void Building::validateSectorDoorOptions(string const& caller, CreateDoorOptions const& options) const
	{
		if (options.orchestrate && !options.controllers[CORE_LAYER_FORE] && !options.controllers[CORE_LAYER_BACK])
		{
			throw BuildingException(this, format("{} - Orchestration requested but no Controllers requested.", caller));
		}
	}

	void Building::validateSectorForceBridgeOptions(string const& caller, CreateForceBridgeOptions const& options) const
	{
		if (options.extensible && (options.controllerCount < 1 || options.controllerCount > 2))
		{
			throw BuildingException(this, format("{} - Controller count must be [1,2] for a controlled ForceBridge, not {}", caller, options.controllerCount));
		}
	}

	void Building::validateSectorLadderOptions(string const& caller, CreateLadderOptions const& options) const
	{
	}

	void Building::validateLiftOptions(string const& caller, CreateLiftOptions const& options) const
	{
		if (options.stopOffsets.size() < 2)
		{
			throw BuildingException(this, format("{} - Lift must have at least 2 stops.", caller));
		}
	}

	void Building::validateShuttleOptions(string const& caller, CreateShuttleOptions const& options) const
	{
		if (options.carWidth != 3 && options.carWidth != 4)
		{
			throw BuildingException(this, format("{} - Shuttle car width must be 3 or 4.", caller));
		}

		if (options.initialStop >= (uint32_t)options.stopOffsets.size())
		{
			throw BuildingException(this, format("{} - Shuttle initialStop parameter out of bounds.", caller));
		}
	}

	shared_ptr<const Layer> Building::getLayer(uint32_t layerIndex) const
	{
		validateLayer(format("Building::getLayer({})", layerIndex), layerIndex);
		return mLayers[layerIndex];
	}

	shared_ptr<Layer> Building::getLayer(uint32_t layerIndex)
	{
		validateLayer(format("Building::getLayer({})", layerIndex), layerIndex);
		return mLayers[layerIndex];
	}

	uint32_t Building::createLocation(string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight)
	{
		auto sectorIndex = (uint32_t)mSectors.size();

		auto location = make_shared<Location>(name, type, layerIndex, sectorIndex, x, y, cellsWide, decksHigh, topDeckHeight, ~0u);
		
		mSectors.push_back(location);
		return sectorIndex;
	}

	uint32_t Building::createLadder(uint32_t x, uint32_t y, CreateLadderOptions const& options)
	{
		auto y0 = y;
		auto y1 = y + options.decksHigh - 1;

		// Get Locations this Ladder connects.
		auto const& cellDef0 = mLayers[CORE_LAYER_FORE]->getCellDefinition(x, y0);
		auto const& cellDef1 = mLayers[CORE_LAYER_FORE]->getCellDefinition(x, y1);
		
		shared_ptr<const Sector> sectors[2] = {
			getSector(cellDef0.sectorIndex),
			getSector(cellDef1.sectorIndex)
		};

		// Transit stops
		vector<TransitStop> stops = {
			{ sectors[CORE_LEVEL_LOW], (int)x - (int)sectors[0]->getCellX(), (int)y - (int)sectors[0]->getCellY() },
			{ sectors[CORE_LEVEL_HIGH], (int)x - (int)sectors[1]->getCellX(), (int)y - (int)sectors[1]->getCellY() }
		};

		auto sectorIndex = (uint32_t)mSectors.size();
		auto ladder = make_shared<LadderTransit>(sectorIndex, x, y, options.decksHigh, stops, options.extensible, options.startExtended);

		mSectors.push_back(ladder);
		return sectorIndex;
	}

	uint32_t Building::createStaircase(uint32_t x, uint32_t y, uint32_t decksHigh, int mountSide)
	{
		ASSERT_SIDE_OK(mountSide);

		// Get Locations this Staircase connects.  Because a Staircase is two cells wide, we
		// just check the first horizontal cell, ie xOffset==0.
		vector<TransitStop> stops;

		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			auto const& cellDef = mLayers[CORE_LAYER_FORE]->getCellDefinition(x, iy);
			auto sector = getSector(cellDef.sectorIndex);

			stops.push_back({ 
				sector,
				(int)x - (int)sector->getCellX(),
				(int)iy - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();
		auto staircase = make_shared<StaircaseTransit>(sectorIndex, x, y, decksHigh, mountSide, stops);

		mSectors.push_back(staircase);
		return sectorIndex;
	}

	Building::CreateObjectResult Building::createLift(uint32_t x, uint32_t y, uint32_t cellsWide, vector<uint32_t> const& stopOffsets)
	{
		// Get Locations this Lift connects.
		vector<TransitStop> stops;

		for (auto stopOffset : stopOffsets)
		{
			uint32_t iy = y + stopOffset;

			auto const& cellDef = mLayers[CORE_LAYER_FORE]->getCellDefinition(x, iy);
			auto sector = getSector(cellDef.sectorIndex);

			stops.push_back({
				sector,
				(int)x - (int)sector->getCellX(),
				(int)iy - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();
		auto lift = make_shared<LiftTransit>(sectorIndex, x, y, cellsWide, stops);

		mSectors.push_back(lift);
		
		return {
			~0u,
			SectorObjectType::Lift,
			lift
		};
	}

	Building::CreateObjectResult Building::createShuttle(uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth, vector<uint32_t> const& stopOffsets)
	{
		// Get Locations this Shuttle connects.
		vector<TransitStop> stops;

		for (auto stopOffset : stopOffsets)
		{
			uint32_t ix = x + stopOffset;

			// Add 1 to ix to get position of the first door
			uint32_t cx = ix + 1;

			auto const& cellDef = mLayers[CORE_LAYER_FORE]->getCellDefinition(cx, y);
			auto sector = getSector(cellDef.sectorIndex);

			stops.push_back({
				sector,
				(int)ix - (int)sector->getCellX(),
				(int)y - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();

		// Need to account for proper track dimensions, width is not necessarily
		// the last stop offset
		auto shuttle = make_shared<ShuttleTransit>(sectorIndex, x, y, cellsWide, numCars, carWidth, stops);

		mSectors.push_back(shuttle);
		
		return {
			~0u,
			SectorObjectType::Shuttle,
			shuttle
		};
	}

	uint32_t Building::addLocation(string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight)
	{
		string caller = format("Building::addLocation({}, {}, {}, {}, {}, {}, {} {})", name, getSectorTypeString(type), layerIndex, x, y, cellsWide, decksHigh, topDeckHeight);
		
		validateBounds(caller, x, y, cellsWide, decksHigh);
		validateLayerSpace(caller, layerIndex, x, y, cellsWide, decksHigh);

		// Create sector
		auto sectorIndex = createLocation(name, type, layerIndex, x, y, cellsWide, decksHigh, topDeckHeight);

		// Set layers
		auto layer = getLayer(layerIndex);

		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);

				cellDef.sectorIndex = sectorIndex;
				cellDef.floorType = iy == y ? CellFloorType::Ground : CellFloorType::None;
			}
		}

		return sectorIndex;
	}

	Building::CreateObjectResult Building::createDoor(uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t* vertexIdentifier)
	{	
		string caller = format("Building::createDoor({}, {}, {})", x, y, cellsWide);

		validateCellOccupied(caller, CORE_LAYER_FORE, x, y);
		validateCellOccupied(caller, CORE_LAYER_BACK, x, y);
		validateCellHasNoDoor(caller, CORE_LAYER_FORE, x, y);
		validateCellHasNoDoor(caller, CORE_LAYER_BACK, x, y);

		// Find Sectors that the Door is connecting.
		auto foreSector = _getSector(mLayers[CORE_LAYER_FORE]->getCellDefinition(x, y).sectorIndex);
		auto backSector = _getSector(mLayers[CORE_LAYER_BACK]->getCellDefinition(x, y).sectorIndex);

		assert(foreSector->getType() == SectorType::Location);

		// Create door in Fore Location and add to Back.
		uint32_t doorIndex = foreSector->createDoor(foreSector, backSector, x, cellsWide, vertexIdentifier);
		backSector->addDoor(dynamic_pointer_cast<DoorSectorObject>(foreSector->_getObject(doorIndex)));

		return {
			doorIndex,
			SectorObjectType::Door,
			foreSector
		};
	}

	Building::CreateObjectResult Building::createWindow(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, uint32_t* vertexIdentifier)
	{
		string caller = format("Building::createWindow({}, {}, {}, {}, {})", layerIndex, x, y, cellsWide, decksHigh);

		validateCellOccupied(caller, CORE_LAYER_BACK, x, y);

		// Find sectors that the Window is connecting, if any.
		shared_ptr<Sector> foreSector{ nullptr };
		shared_ptr<Sector> backSector{ nullptr };

		if (layerIndex == CORE_LAYER_FORE)
		{
			validateCellOccupied(caller, CORE_LAYER_FORE, x, y);

			auto const& cellDef0 = mLayers[CORE_LAYER_FORE]->getCellDefinition(x, y);
			auto const& cellDef1 = mLayers[CORE_LAYER_BACK]->getCellDefinition(x, y);

			foreSector = _getSector(cellDef0.sectorIndex);
			backSector = cellDef1.sectorIndex != ~0u ? _getSector(cellDef1.sectorIndex) : nullptr;
		}
		else
		{
			auto const& cellDef1 = mLayers[CORE_LAYER_BACK]->getCellDefinition(x, y);
		
			foreSector = _getSector(cellDef1.sectorIndex);
			backSector = nullptr;
		}

		// Create window in fore Location and add to back
		auto windowIndex = foreSector->createWindow(foreSector, backSector, x, y, cellsWide, decksHigh, vertexIdentifier);

		if (backSector)
		{
			backSector->addWindow(dynamic_pointer_cast<WindowSectorObject>(foreSector->_getObject(windowIndex)));
		}

		return {
			windowIndex,
			SectorObjectType::Window,
			foreSector
		};
	}

	Building::CreateObjectResult Building::createBulkheadDoor(uint32_t layerIndex, uint32_t x, uint32_t y, int side)
	{
		ASSERT_SIDE_OK(side);

		string caller = format("Building::createBulkheadDoor({}, {}, {}, {})", layerIndex, x, y, side);
		uint32_t cx0, cx1;

		if (side == CORE_SIDE_LEFT)
		{
			cx0 = x - 1;
			cx1 = x;
		}
		else
		{
			cx0 = x;
			cx1 = x + 1;
		}

		validateCellOccupied(caller, layerIndex, cx0, y);
		validateCellOccupied(caller, layerIndex, cx1, y);

		// Find locations that the Door is connecting.
		shared_ptr<Sector> leftSector{ nullptr };
		shared_ptr<Sector> rightSector{ nullptr };

		auto layer = getLayer(layerIndex);

		auto& cellDef0 = layer->getCellDefinition(cx0, y);
		auto& cellDef1 = layer->getCellDefinition(cx1, y);

		leftSector = _getSector(cellDef0.sectorIndex);
		rightSector = _getSector(cellDef1.sectorIndex);

		// Create bulkhead door in one location and add to the other
		auto doorIndex = leftSector->createBulkheadDoor(leftSector, rightSector, y - leftSector->getCellY(), CORE_SIDE_LEFT);

		rightSector->addBulkheadDoor(dynamic_pointer_cast<BulkheadDoorSectorObject>(leftSector->_getObject(doorIndex)), y - rightSector->getCellY(), CORE_SIDE_RIGHT);
		
		return {
			doorIndex,
			SectorObjectType::BulkheadDoor,
			leftSector
		};
	}

	Building::CreateObjectResult Building::createController(string const& name, uint32_t layerIndex, uint32_t x, uint32_t y, int side, uint32_t flags, uint32_t* vertexIdentifier)
	{
		string caller = format("Building::createController({}, {}, {}, {}, {})", layerIndex, x, y, side, flags);
		
		validateCellOccupied(caller, layerIndex, x, y);
		validateCellHasNoController(caller, layerIndex, x, y, side);

		// Create Controller
		auto& cellDef = mLayers[layerIndex]->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		// Calculate x and y offset within cell.
		// - If side is left or right, then x offset is either -.x or 1.x, respectively, relative to x
		// - If side is middle, then x offset is 0.5
		float xOffset, yOffset;

		switch (side)
		{
		case CORE_SIDE_LEFT:
		case CORE_SIDE_RIGHT:
			xOffset = side - CORE_BUTTON_SIZE * 0.5f;
			yOffset = CORE_BUTTON_Y_OFFSET;
			break;

		case CORE_SIDE_MIDDLE:
			xOffset = 0.5f - CORE_BUTTON_SIZE * 0.5f;
			yOffset = CORE_BUTTON_Y_OFFSET;
			break;

		default:
			throw UnhandledException(side, "side");
		}

		// Check to see if any Controllers are already in this cell.  Adjust y position accordingly.
		if (side == CORE_SIDE_LEFT && x > 0)
		{
			auto& lcd = mLayers[layerIndex]->getCellDefinition(x - 1, y);
			if (lcd.sectorIndex == cellDef.sectorIndex)
			{
				if (lcd.controllers[CORE_SIDE_RIGHT] != ~0u)
				{
					// Clash: adjust heights
					auto ctrl = _getSector(lcd.sectorIndex)->getObject(lcd.controllers[CORE_SIDE_RIGHT]);
					auto button = static_pointer_cast<Button>(ctrl->_getObject());

					button->_adjustY(0.025f);
					yOffset -= 0.025f;
				}
			}
		}
		else if (side == CORE_SIDE_RIGHT && x < (getCellsWide() - 1))
		{
			auto& lcd = mLayers[layerIndex]->getCellDefinition(x + 1, y);

			if (lcd.sectorIndex == cellDef.sectorIndex)
			{
				if (lcd.controllers[CORE_SIDE_LEFT] != ~0u)
				{
					// Clash: adjust heights
					auto ctrl = _getSector(lcd.sectorIndex)->getObject(lcd.controllers[CORE_SIDE_LEFT]);
					auto button = static_pointer_cast<Button>(ctrl->_getObject());

					button->_adjustY(-0.025f);
					yOffset += 0.025f;
				}
			}
		}

		auto controllerIndex = sector->createController(sector, name, x, y, xOffset, yOffset, flags, vertexIdentifier);
		cellDef.controllers[side] = controllerIndex;

		return {
			controllerIndex,
			SectorObjectType::Controller,
			sector
		};
	}

	Building::CreateObjectResult Building::createWalkway(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t* vertexIdentifier)
	{
		string caller = format("Building::createWalkway({}, {}, {})", layerIndex, x, y);
		
		validateCellOccupied(caller, layerIndex, x, y);

		// Create Walkway
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			sector->createWalkway(sector, x, y, vertexIdentifier),
			SectorObjectType::Walkway,
			sector
		};
	}

	Building::CreateObjectResult Building::createMarker(uint32_t layerIndex, uint32_t x, uint32_t y, float xOffset, uint32_t* vertexIdentifier)
	{
		string caller = format("Building::createMarker({}, {}, {}, {})", layerIndex, x, y, xOffset);

		float xPos = x + xOffset;
		x = (uint32_t)xPos;
		

		validateCellOccupied(caller, layerIndex, x, y);

		// Create Marker
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			sector->createMarker(sector, x, y, xPos - x, vertexIdentifier),
			SectorObjectType::Marker,
			sector
		};
	}

	Building::CreateObjectResult Building::createForceBridge(uint32_t layerIndex, uint32_t x, uint32_t y, CreateForceBridgeOptions const& options)
	{
		ASSERT_SIDE_OK(options.fromSide);

		string caller = format("Building::createForceBridge({}, {}, {}, {}, {}, {})", layerIndex, x, y, options.width, options.fromSide, options.startExtended);

		validateCellOccupied(caller, layerIndex, x, y);

		// Create ForceBridge
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			sector->createForceBridge(sector, x, y, options.width, options.fromSide, options.extensible, options.startExtended),
			SectorObjectType::ForceBridge,
			sector
		};
	}

	Building::CreateObjectResult Building::createLadderSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options, uint32_t* vertexIdentifier)
	{
		auto layer = getLayer(layerIndex);

		string caller = format("Building::createLadderSectorObject({}, {}, {}, {}, {})", layerIndex, x, y, options.startExtended, options.decksHigh);

		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		return {
			sector->createLadder(sector, x, y, options.extensible, options.startExtended, options.decksHigh, vertexIdentifier),
			SectorObjectType::Ladder,
			sector
		};
	}

	Building::CreateObjectResult Building::createPlatformLiftSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLiftOptions const& options, uint32_t* vertexIdentifier)
	{
		auto layer = getLayer(layerIndex);

		string caller = format("Building::createPlatformLiftSectorObject({}, {}, {})", layerIndex, x, y);

		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = _getSector(cellDef.sectorIndex);

		vector<uint32_t> stops;

		for (auto stopOffset : options.stopOffsets)
		{
			stops.push_back(y + stopOffset);
		}

		return {
			sector->createPlatformLift(sector, x, y, options.cellsWide, stops, vertexIdentifier),
			SectorObjectType::Lift,
			sector
		};
	}

	shared_ptr<Sector> Building::_getSector(uint32_t index)
	{
		if (index >= getNumSectors())
		{
			throw BuildingException(this, format("Building::getSector({}) - index={} is out of range.", index, index));
		}

		return mSectors[index];
	}

	shared_ptr<const Sector> Building::getSector(uint32_t index) const
	{
		if (index >= getNumSectors())
		{
			throw BuildingException(this, format("Building::getSector({}) - index={} is out of range.", index, index));
		}

		return mSectors[index];
	}

	vector<shared_ptr<const Sector>> Building::getSectorsInBounds(uint32_t layerIndex, float x, float y, float width, float height) const
	{
		auto layer = getLayer(layerIndex);

		// Convert screen bounds to cell bounds
		int cellX0 = max((int)(x / CORE_CELL_WIDTH_PIXELS), 0);
		int cellY0 = max((int)(y / CORE_DECK_HEIGHT_PIXELS), 0);
		int cellX1 = min((int)((x + width) / CORE_CELL_WIDTH_PIXELS), (int)mCellsWide - 1);
		int cellY1 = min((int)((y + height) / CORE_CELL_WIDTH_PIXELS), (int)mDecksHigh - 1);

		// Add all Locations to a set, as a single Location will have a reference for every CellDefinition,
		// but we only want it once.
		set<shared_ptr<const Sector>> sectors;

		for (int y = cellY0; y <= cellY1; ++y)
		{
			for (int x = cellX0; x <= cellX1; ++x)
			{
				auto const& cellDef = layer->getCellDefinition(x, y);

				if (cellDef.sectorIndex != ~0u)
				{
					auto location = getSector(cellDef.sectorIndex);
					sectors.insert(location);
				}
			}
		}

		return vector<shared_ptr<const Sector>>(sectors.begin(), sectors.end());
	}

	vector<shared_ptr<const Sector>> Building::getSectors(uint32_t layerIndex) const
	{
		vector<shared_ptr<const Sector>> sectors;

		for (auto sector : mSectors)
		{
			if (sector->getLayerIndex() == layerIndex)
			{
				sectors.push_back(sector);
			}
		}

		return sectors;
	}

	shared_ptr<const Graph> Building::getGraph() const
	{
		return mGraph;
	}

	Log const& Building::getBuildLog() const
	{
		return mBuildLog;
	}

	uint32_t Building::addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh)
	{
		return addLocation("Corridor", SectorType::Location, CORE_LAYER_FORE, x, y, cellsWide, decksHigh, CORE_CORRIDOR_HEIGHT);
	}

	uint32_t Building::addRoom(string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight)
	{
		if (topDeckHeight < CORE_ROOM_MIN_HEIGHT || topDeckHeight > CORE_ROOM_MAX_HEIGHT)
		{
			string caller = format("Building::addRoom({}, {}, {}, {}, {}, {}, {})", name, layerIndex, y, x, cellsWide, decksHigh, topDeckHeight);

			throw BuildingException(this, format("{} - topDeckHeight={} is out of range", caller, topDeckHeight));
		}

		return addLocation(name, SectorType::Location, layerIndex, x, y, cellsWide, decksHigh, topDeckHeight);
	}

	Building::CreateLadderResult Building::addLadder(uint32_t y, uint32_t x, CreateLadderOptions const& options)
	{
		auto foreLayer = getLayer(CORE_LAYER_FORE);
		auto backLayer = getLayer(CORE_LAYER_BACK);

		// Checks
		string caller = format("Building::addLadder({}, {}, {}, {})", y, x, options.decksHigh, options.startExtended);

		if (options.decksHigh < 2)
		{
			throw BuildingException(this, format("{} - Ladder at {},{} must be at least 2 decks high", caller, x, y));
		}

		validateBounds(caller, x, y, 1, options.decksHigh);
		validateLayerSpace(caller, CORE_LAYER_BACK, x, y, 1, options.decksHigh);

		auto y0 = y;
		auto y1 = y + options.decksHigh - 1;

		// Make sure the foreground cells have a Sector
		auto const& cellDef0 = foreLayer->getCellDefinition(x, y0);
		auto const& cellDef1 = foreLayer->getCellDefinition(x, y1);
		auto foreSectorIndex0 = cellDef0.sectorIndex;
		auto foreSectorIndex1 = cellDef1.sectorIndex;

		if (foreSectorIndex0 == ~0u)
		{
			throw BuildingException(this, format("{} - foreground cell at {},{} is not occupied, which blocks ladder being placed", caller, x, y0));
		}
		if (foreSectorIndex1 == ~0u)
		{
			throw BuildingException(this, format("{} - foreground cell at {},{} is not occupied, which blocks ladder being placed", caller, x, y1));
		}
		if (foreSectorIndex0 == foreSectorIndex1)
		{
			throw BuildingException(this, format("{} - foreground cells from {},{} to {},{} are the same sector, which blocks ladder being placed", caller, x, y0, x, y1));
		}

		// Ladders can only connect Locations
		auto const& foreSector0 = _getSector(foreSectorIndex0);
		auto const& foreSector1 = _getSector(foreSectorIndex1);

		if (foreSector0->getType() != SectorType::Location)
		{
			throw BuildingException(this, format("{} - foreground cell at {},{} is not a Location, which blocks ladder being placed", caller, x, y0));
		}
		if (foreSector1->getType() != SectorType::Location)
		{
			throw BuildingException(this, format("{} - foreground cell at {},{} is not a Location, which blocks ladder being placed", caller, x, y1));
		}

		// Ladders ends must not be in the air
		validateCellTraversableOnFoot(caller, "Ladder", CORE_LAYER_FORE, x, y0);
		validateCellTraversableOnFoot(caller, "Ladder", CORE_LAYER_FORE, x, y1);

		// Create ladder
		auto sectorIndex = createLadder(x, y, options);

		// Set layers
		for (uint32_t iy = y; iy < y + options.decksHigh; ++iy)
		{
			auto& cellDef = backLayer->getCellDefinition(x, iy);

			cellDef.sectorIndex = sectorIndex;
		}

		auto ladderSector = _getSector(sectorIndex);
		auto ladderTransit = dynamic_pointer_cast<LadderTransit>(ladderSector);
		auto ladder = ladderTransit->getLadder();

		// See if we need an Controller
		CreateObjectResult createdCtrls[2];

		if (options.extensible)
		{
			// Set up the ForceBridge and Button with an appropriate Orchestrator
			auto orchSystem = make_shared<ButtonExtensibleObjectOrchestratedSystem>(mOrchestrator);

			orchSystem->setExtensibleObject(ladder);

			// Try and place on the right of the Ladder, unless it's at the end of the Location
			auto locX0 = foreSector0->getCellX();
			auto locX1 = locX0 + foreSector0->getCellsWide();
			int side = x == locX1 ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;

			// Lower
			createdCtrls[CORE_LEVEL_LOW] = _createLadderButton(foreSector0, x, y0, side, 0);

			auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_LEVEL_LOW].sector->_getObject(createdCtrls[CORE_LEVEL_LOW].index));
			orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

			// Upper
			locX0 = foreSector1->getCellX();
			locX1 = locX1 + foreSector1->getCellsWide();
			side = x == locX1 ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;

			createdCtrls[CORE_LEVEL_HIGH] = _createLadderButton(foreSector1, x, y1, side, 0);

			buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_LEVEL_HIGH].sector->_getObject(createdCtrls[CORE_LEVEL_HIGH].index));
			orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

			mOrchestrator->addSystem(orchSystem);
		}

		return {
			{ ~0u, SectorObjectType::Ladder, ladderSector },
			{ createdCtrls[0], createdCtrls[1] }
		};
	}

	uint32_t Building::addStaircase(uint32_t y, uint32_t x, uint32_t decksHigh, int mountSide)
	{
		ASSERT_SIDE_OK(mountSide);

		auto foreLayer = getLayer(CORE_LAYER_FORE);
		auto backLayer = getLayer(CORE_LAYER_BACK);
		const uint32_t cellsWide = 2;

		// Checks
		string caller = format("Building::addStaircase({}, {}, {}, {})", y, x, decksHigh, mountSide);

		if (decksHigh < 2)
		{
			throw BuildingException(this, format("{} - Staircase at {},{} must be at least 2 decks high", caller, x, y));
		}

		validateBounds(caller, x, y, cellsWide, decksHigh);
		validateLayerSpace(caller, CORE_LAYER_BACK, x, y, cellsWide, decksHigh);

		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			auto const& cellDef0 = foreLayer->getCellDefinition(x, iy);
			auto deckSectorIndex = cellDef0.sectorIndex;

			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cellDef = foreLayer->getCellDefinition(ix, iy);
				auto foreSectorIndex = cellDef.sectorIndex;

				// Make sure the foreground cells have a Sector, and that the horizontal Sectors are not different:
				// Staircases cannot span different Sectors horizontally, due to placement of the door leading to them.
				if (foreSectorIndex != deckSectorIndex)
				{
					throw BuildingException(this, format("{} - the staircase horizontally spans different foreground Sectors between {},{} and {},{}, which is not allowed", caller, x, iy, x + 1, iy));
				}

				// Fore Sector can't be empty
				if (foreSectorIndex == ~0u)
				{
					throw BuildingException(this, format("{} - foreground cell at {},{} is not occupied, which blocks staircase being placed", caller, ix, iy));
				}

				// Staircases can only connect Locations
				auto const& foreSector = getSector(foreSectorIndex);

				if (foreSector->getType() != SectorType::Location)
				{
					throw BuildingException(this, format("{} - foreground cell at {},{} is not a Location, which blocks staircase being placed", caller, ix, iy));
				}

				// Staircases must not be in the air
				validateCellTraversableOnFoot(caller, "Staircase", CORE_LAYER_FORE, ix, iy);
			}
		}

		// Create staircase
		auto sectorIndex = createStaircase(x, y, decksHigh, mountSide);

		// Set layers
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = backLayer->getCellDefinition(ix, iy);

				cellDef.sectorIndex = sectorIndex;
			}
		}

		return sectorIndex;
	}

	Building::CreateLiftResult Building::addLift(uint32_t y, uint32_t x, CreateLiftOptions const& options)
	{
		auto foreLayer = getLayer(CORE_LAYER_FORE);
		auto backLayer = getLayer(CORE_LAYER_BACK);

		// Checks
		string caller = format("Building::addLift({}, {}, {}, <stopOffsts>)", y, x, options.cellsWide);

		validateLiftOptions(caller, options);

		auto decksHigh = options.stopOffsets.back() + 1;

		validateBounds(caller, x, y, options.cellsWide, decksHigh);
		validateLayerSpace(caller, CORE_LAYER_BACK, x, y, options.cellsWide, decksHigh);

		for (auto stopOffset : options.stopOffsets)
		{
			auto iy = y + stopOffset;
		
			auto const& cellDef = foreLayer->getCellDefinition(x, iy);
			auto deckSectorIndex = cellDef.sectorIndex;

			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				auto const& cellDef = foreLayer->getCellDefinition(ix, iy);
				auto foreSectorIndex = cellDef.sectorIndex;

				// Make sure the foreground cells have a Sector, and that the horizontal Sectors are not different:
				// Lifts cannot span different Sectors horizontally, due to placement of the door leading to them.
				if (foreSectorIndex != deckSectorIndex)
				{
					throw BuildingException(this, format("{} - the lift horizontally spans different foreground Sectors between {},{} and {},{}, which is not allowed", caller, x, iy, x + 1, iy));
				}

				// Fore Sector can't be empty
				if (foreSectorIndex == ~0u)
				{
					throw BuildingException(this, format("{} - foreground cell at {},{} is not occupied, which blocks lift being placed", caller, ix, iy));
				}

				// Lifts can only connect Locations
				auto const& foreSector = getSector(foreSectorIndex);

				if (foreSector->getType() != SectorType::Location)
				{
					throw BuildingException(this, format("{} - foreground cell at {},{} is not a Location, which blocks lift being placed", caller, ix, iy));
				}

				// Lifts must not be in the air
				validateCellTraversableOnFoot(caller, "Lift", CORE_LAYER_FORE, ix, iy);
			}
		}

		// Create lift
		auto liftObject = createLift(x, y, options.cellsWide, options.stopOffsets);

		auto liftTransit = dynamic_pointer_cast<LiftTransit>(liftObject.sector);
		auto lift = liftTransit->getLift();

		// Set layers
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				auto& cellDef = backLayer->getCellDefinition(ix, iy);

				cellDef.sectorIndex = liftObject.sector->getIndex();
			}
		}

		CreateLiftResult liftRes;

		liftRes.lift = liftObject;

		// Orchestrate
		auto orchSystem = make_shared<LiftOrchestratedSystem>(mOrchestrator);
		liftRes.orchSystem = orchSystem;
		
		orchSystem->setLift(liftTransit);

		// Create Doors and Buttons
		for (auto stopOffset : options.stopOffsets)
		{
			auto doorRes = _addSectorDoor(y + stopOffset, x, { options.cellsWide, { true, false }, false });
			
			auto door = static_pointer_cast<DoorSectorObject>(doorRes.door.sector->_getObject(doorRes.door.index))->getDoor();
			
			auto ctrl = doorRes.controllers[CORE_LAYER_FORE];
			auto button = dynamic_pointer_cast<Button>(ctrl.sector->_getObject(ctrl.index)->_getObject());

			orchSystem->addStop(door, button);

			liftRes.doors.push_back(doorRes);
		}

		mOrchestrator->addSystem(orchSystem);
		
		return liftRes;
	}

	Building::CreateShuttleResult Building::addShuttle(uint32_t y, uint32_t x, uint32_t cellsWide, CreateShuttleOptions const& options)
	{
		auto foreLayer = getLayer(CORE_LAYER_FORE);
		auto backLayer = getLayer(CORE_LAYER_BACK);

		// Checks
		string caller = format("Building::addShuttle({}, {}, {}, <options>)", y, x, cellsWide);

		// Bear in mind that we may have to allow extra space on the back layer beyond the x/x+cellsWide
		// extents.  Eg, for where a=x and b=x+cellsWide, and s=stop offsets:
		//       a        b
		//        s      s 
		// BACK: XDX-------
		// FORE: -D-....-D-


		validateShuttleOptions(caller, options);
		validateBounds(caller, x, y, cellsWide, 1);
		validateLayerSpace(caller, CORE_LAYER_BACK, x, y, cellsWide, 1);		

		auto shuttleWidth = options.carWidth * options.numCars + (options.numCars - 1);

		// Make sure the shuttle track is long enough
		if (shuttleWidth > cellsWide)
		{
			throw BuildingException(this, format("{} - shuttle track is not wide enough for shuttle", caller));
		}

		auto numStops = (uint32_t)options.stopOffsets.size();
		for (uint32_t i = 0; i < numStops; ++i)
		{
			auto ix = x + options.stopOffsets[i];

			// Each stop must be at least shuttleWidth cells apart
			if (i < numStops - 1)
			{
				auto ix1 = x + options.stopOffsets[i + 1];

				if (ix1 - ix < shuttleWidth)
				{
					throw BuildingException(this, format("{} - shuttle is too wide to fit between stop offsets {} and {}", caller, i, i + 1));
				}
			}

			// Must be able to fit whole shuttle into stop
			if (ix + shuttleWidth > (x + cellsWide))
			{
				throw BuildingException(this, format("{} - shuttle is too wide to fit at stop offset {}", caller, i));
			}

			// For door openings, fore cell must be occupied and a Location
			uint32_t stopSectorIndex{ ~0u };
			for (uint32_t j = 0; j < options.numCars; ++j)
			{
				// Offset of door is 1 within a car, and each car has a gap of 1 between
				// it and the previous
				uint32_t cx = ix + j * (options.carWidth + 1) + 1;

				validateCellIsType(caller, CORE_LAYER_FORE, cx, y, SectorType::Location);

				if (options.carWidth == 4)
				{
					validateCellIsType(caller, CORE_LAYER_FORE, cx + 1, y, SectorType::Location);
				}

				// Stop doors can only connect a single Location
				auto thisStopSectorIndex = foreLayer->getCellDefinition(cx, y).sectorIndex;
				if (thisStopSectorIndex != ~0u)
				{
					if (stopSectorIndex == ~0u)
					{
						thisStopSectorIndex = stopSectorIndex;
					}
					else
					{
						throw BuildingException(this, format("{} - stop offset {} doors span multiple fore Locations", caller, i));
					}
				}
			}
		}

		// Create shuttle
		auto shuttleObject = createShuttle(x, y, cellsWide, options.numCars, options.carWidth, options.stopOffsets);

		auto shuttleTransit = dynamic_pointer_cast<ShuttleTransit>(shuttleObject.sector);
		auto shuttle = shuttleTransit->getShuttle();

		// Set layers
		for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef = backLayer->getCellDefinition(ix, y);

			cellDef.sectorIndex = shuttleObject.sector->getIndex();
		}

		CreateShuttleResult shuttleRes;

		shuttleRes.shuttle = shuttleObject;

		// Orchestrate
		auto orchSystem = make_shared<ShuttleOrchestratedSystem>(mOrchestrator);
		shuttleRes.orchSystem = orchSystem;

		orchSystem->setShuttle(shuttleTransit);

		// Create Doors and Buttons
		uint32_t stopIndex{ 0 };
		for (auto stopOffset : options.stopOffsets)
		{
			uint32_t doorWidth = options.carWidth - 2;

			vector<shared_ptr<Door>> stopDoors;
			vector<shared_ptr<Button>> stopButtons;

			for (uint32_t door_i = 0; door_i < options.numCars; ++door_i)
			{
				uint32_t doorX = door_i * options.carWidth + door_i + 1;
				auto doorRes = _addSectorDoor(y, x + stopOffset + doorX, { doorWidth, { true, false }, false });

				auto door = static_pointer_cast<DoorSectorObject>(doorRes.door.sector->_getObject(doorRes.door.index))->getDoor();
				stopDoors.push_back(door);

				auto ctrl = doorRes.controllers[CORE_LAYER_FORE];
				auto button = dynamic_pointer_cast<Button>(ctrl.sector->_getObject(ctrl.index)->_getObject());
				stopButtons.push_back(button);

				shuttleRes.doors.push_back(doorRes);
			}

			orchSystem->addStop(stopIndex++, stopDoors, stopButtons);
		}

		mOrchestrator->addSystem(orchSystem);

		return shuttleRes;
	}

	void Building::removeLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side)
	{
		ASSERT_SIDE_OK(side);

		auto sector = _getSector(sectorIndex);

		sector->removeEndWall(deckIndex, side);

		// See if we need to remove anything on the other side.  Below code
		// works because "Right" is value 1.
		uint32_t x = sector->getCellX() + side * sector->getCellsWide();
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);
		uint32_t neighbourSectorIndex{ ~0u };

		if (side == CORE_SIDE_LEFT && x > 0)
		{
			neighbourSectorIndex = layer->getCellDefinition(x - 1, deckIndex).sectorIndex;
		}
		else if (side == CORE_SIDE_RIGHT && x < getCellsWide() - 1)
		{
			neighbourSectorIndex = layer->getCellDefinition(x + 1, deckIndex).sectorIndex;
		}

		if (neighbourSectorIndex != ~0u)
		{
			auto neighbour = _getSector(neighbourSectorIndex);

			// Convert deckIndex as they may start on different decks.
			uint32_t neighbourDeckIndex = (sector->getCellY() + deckIndex) - sector->getCellY();

			neighbour->removeEndWall(neighbourDeckIndex, 1 - side);
		}
	}

	Building::CreateObjectResult Building::_createSectorButton(string const& name, shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t flags, uint32_t* index)
	{
		auto side = CORE_SIDE_MIDDLE;
		uint32_t buttonX = sector->getCellX() + x;

		auto const& obj = createController(name, sector->getLayerIndex(), buttonX, y, side, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateObjectResult Building::_createDoorButton(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t flags, uint32_t* index)
	{
		int side = ((x + cellsWide) - 1) == sector->getCellX1() ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;
		uint32_t buttonX = x + (side == CORE_SIDE_LEFT ? 0 : cellsWide - 1);
		
		auto obj = createController("Door button", sector->getLayerIndex(), buttonX, y, side, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateObjectResult Building::_createBulkheadDoorButton(shared_ptr<const Sector> sector, uint32_t y, int side, uint32_t* index)
	{
		uint32_t buttonX = side == CORE_SIDE_LEFT ? sector->getCellX1() : sector->getCellX0();

		auto obj = createController("BulkheadDoor button", sector->getLayerIndex(), buttonX, y, CORE_SIDE_MIDDLE, 0);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateObjectResult Building::_createForceBridgeButton(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index)
	{
		string caller = format("_createForceBridgeButton(<sector>, {}, {}, {}, {}, {}, <index>)", x, y, cellsWide, side, flags);
		uint32_t buttonX = x + (side == CORE_SIDE_LEFT ? 0 : cellsWide - 1);

		// Check position of button cell
		validateCellIsInSector(caller, buttonX, y, sector);

		auto obj = createController("ForceBridge button", sector->getLayerIndex(), buttonX, y, side, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateObjectResult Building::_createLadderButton(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, int side, uint32_t flags, uint32_t* index)
	{
		string caller = format("_createLadderButton(<sector>, {}, {}, {}, {}, <index>)", x, y, side, flags);
		uint32_t buttonX = x;

		// Check position of button cell
		validateCellIsInSector(caller, buttonX, y, sector);

		auto obj = createController("Ladder button", sector->getLayerIndex(), buttonX, y, side, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateObjectResult Building::_createPlatformLiftButton(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index)
	{
		string caller = format("_createPlatformLiftButton(<sector>, {}, {}, {}, {}, {}, <index>)", x, y, cellsWide, side, flags);
		uint32_t buttonX = x + side == CORE_SIDE_LEFT ? 0 : (cellsWide - 1);

		// Check position of button cell
		validateCellIsInSector(caller, buttonX, y, sector);

		auto obj = createController("Platform lift button", sector->getLayerIndex(), buttonX, y, side, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateDoorResult Building::addSectorDoor(uint32_t y, uint32_t x, CreateDoorOptions const& options)
	{
		return _addSectorDoor(y, x, options);
	}

	Building::CreateDoorResult Building::_addSectorDoor(uint32_t y, uint32_t x, CreateDoorOptions const& options)
	{
		string caller = format("Building::addSectorDoor({}, {}, {})", y, x, options.width);

		auto cellsWide = options.width;

		validateSectorDoorOptions(caller, options);
		validateBounds(caller, x, y, cellsWide, 1);
		validateSpaceOnlyInOneSector(caller, CORE_LAYER_FORE, x, y, cellsWide, 1);
		validateSpaceOnlyInOneSector(caller, CORE_LAYER_BACK, x, y, cellsWide, 1);

		auto layer0 = getLayer(CORE_LAYER_FORE);
		auto layer1 = getLayer(CORE_LAYER_BACK);

		shared_ptr<Sector> sectors[2];
		for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef0 = layer0->getCellDefinition(ix, y);
			auto& cellDef1 = layer1->getCellDefinition(ix, y);

			if (cellDef0.sectorIndex == ~0u)
			{
				throw BuildingException(this, format("{} - foreground cell at {},{} is not occupied.", caller, ix, y));
			}

			sectors[CORE_LAYER_FORE] = _getSector(cellDef0.sectorIndex);

			if (cellDef1.sectorIndex == ~0u)
			{
				throw BuildingException(this, format("{} - background cell at {},{} is not occupied.", caller, ix, y));
			}

			sectors[CORE_LAYER_BACK] = _getSector(cellDef1.sectorIndex);

			if (cellDef0.sectorObjectType == SectorObjectType::Door)
			{
				throw BuildingException(this, format("{} - cell at {},{} is already has a door.", caller, ix, y));
			}

			validateObjectAllowedInSector(caller, SectorObjectType::Door, cellDef0.sectorIndex);
			validateObjectAllowedInSector(caller, SectorObjectType::Door, cellDef1.sectorIndex);
		}

		// Create door, making sure we add it to the other Location as well
		// If there is a button, then a stateful button will control a stateless door - the state
		// can only be in one object.
		auto doorObject = createDoor(x, y, cellsWide);

		// Set layers
		for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef0 = layer0->getCellDefinition(ix, y);
			auto& cellDef1 = layer1->getCellDefinition(ix, y);

			cellDef0.sectorObjectIndex = doorObject.index;
			cellDef0.sectorObjectType = doorObject.type;

			cellDef1.sectorObjectIndex = doorObject.index;
			cellDef1.sectorObjectType = doorObject.type;
		}

		// Set up controllers and orchestration
		CreateObjectResult createdCtrls[2];

		for (int i = 0; i < 2; ++i)
		{
			if (options.controllers[i])
			{
				if (x == sectors[i]->getCellX0() && (x + options.width - 1) == sectors[i]->getCellX1())
				{
					throw BuildingException(this, format("{} - No space to place Buttons for Door", caller));
				}

				auto buttonObject = _createDoorButton(sectors[i], x, y, cellsWide, CORE_BUTTON_F_AUTO_REENABLE, &createdCtrls[i].index);
				createdCtrls[i].type = SectorObjectType::Controller;
				createdCtrls[i].sector = sectors[i];
			}
		}

		// Orchestrate
		shared_ptr<ButtonDoorOrchestratedSystem> orchSystem;

		if (options.orchestrate)
		{
			auto doorSectorObject = dynamic_pointer_cast<DoorSectorObject>(doorObject.sector->_getObject(doorObject.index));
			auto door = doorSectorObject->getDoor();
			
			orchSystem = make_shared<ButtonDoorOrchestratedSystem>(mOrchestrator);

			orchSystem->setDoor(door);

			for (int i = 0; i < CORE_NUM_LAYERS; ++i)
			{
				if (options.controllers[i])
				{
					auto const& ctrl = createdCtrls[i];

					auto button = dynamic_pointer_cast<Button>(ctrl.sector->_getObject(ctrl.index)->_getObject());

					doorSectorObject->addController(i == CORE_LAYER_FORE ? "ForeController" : "BackController", button);
					orchSystem->addButton(button);
				}
			}

			mOrchestrator->addSystem(orchSystem);
		}

		return {
			doorObject,
			{ createdCtrls[0], createdCtrls[1] },
			orchSystem
		};
	}

	uint32_t Building::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh)
	{
		string caller = format("Building::addSectorWindow({}, {}, {}, {})", layerIndex, y, x, cellsWide);

		validateBounds(caller, x, y, cellsWide, 1);

		if (layerIndex == CORE_LAYER_FORE)
		{
			validateSpaceOnlyInOneSector(caller, CORE_LAYER_FORE, x, y, cellsWide, decksHigh);
		}

		validateSpaceOnlyInOneSector(caller, CORE_LAYER_BACK, x, y, cellsWide, decksHigh);

		// Windows can be be placed on either Layer, but there must be a Location on that Layer.
		auto layer = getLayer(layerIndex);

		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);

				if (cellDef.sectorIndex == ~0u)
				{
					throw BuildingException(this, format("{} - {} cell at {},{} is not occupied.",
						caller, layerIndex ? "background" : "foreground", ix, y));
				}

				validateObjectAllowedInSector(caller, SectorObjectType::Window, cellDef.sectorIndex);
			}
		}

		// Create window
		auto const& [windowIndex, windowObjType, windowSector] = createWindow(layerIndex, x, y, cellsWide, decksHigh);

		// Set layers
		for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef = layer->getCellDefinition(ix, y);

			cellDef.sectorObjectIndex = windowIndex;
			cellDef.sectorObjectType = SectorObjectType::Window;
		}

		return windowIndex;
	}

	Building::CreateBulkheadDoorResult Building::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side)
	{
		ASSERT_SIDE_OK(side);

		string caller = format("Building::addSectorBulkheadDoor({}, {}, {}, {})", layerIndex, y, x, side);

		if (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT)
		{
			throw BuildingException(this, format("{} - side={} is invalid and must be 0 or 1", caller, side));
		}

		// Get locations on either side.
		auto layer = getLayer(layerIndex);

		uint32_t cx0, cx1;

		if (side == CORE_SIDE_LEFT)
		{
			cx0 = x - 1;
			cx1 = x;
		}
		else
		{
			cx0 = x;
			cx1 = x + 1;
		}

		auto& cellDef0 = layer->getCellDefinition(cx0, y);
		auto& cellDef1 = layer->getCellDefinition(cx1, y);

		// Check that there are no Doors or Windows in cells X and X-1, as there won't
		// be space for them.
		if (cellDef0.sectorObjectType == SectorObjectType::Door || cellDef0.sectorObjectType == SectorObjectType::Window)
		{
			throw BuildingException(this, format("{} - cell at {}, {} has an object blocking the Bulkhead door", caller, cx0, y));
		}
		if (cellDef1.sectorObjectType == SectorObjectType::Door || cellDef1.sectorObjectType == SectorObjectType::Window)
		{
			throw BuildingException(this, format("{} - cell at {}, {} has an object blocking the Bulkhead door", caller, cx1, y));
		}

		validateObjectAllowedInSector(caller, SectorObjectType::BulkheadDoor, cellDef0.sectorIndex);
		validateObjectAllowedInSector(caller, SectorObjectType::BulkheadDoor, cellDef1.sectorIndex);
		
		// Create Bulkhead door
		auto doorObject = createBulkheadDoor(layerIndex, x, y, side);

		// Update cells
		cellDef0.bulkheadIndices[CORE_SIDE_RIGHT] = doorObject.index;
		cellDef1.bulkheadIndices[CORE_SIDE_LEFT] = doorObject.index;

		// Add buttons: place two, one of each side.
		auto sector0 = _getSector(cellDef0.sectorIndex);
		auto sector1 = _getSector(cellDef1.sectorIndex);

		auto dooSectorObject = doorObject.sector->_getObject(doorObject.index);
		auto door = dynamic_pointer_cast<BulkheadDoorSectorObject>(dooSectorObject)->getDoor();

		CreateObjectResult createdCtrls[2];

		createdCtrls[CORE_SIDE_LEFT] = _createBulkheadDoorButton(sector0, y, CORE_SIDE_LEFT);
		createdCtrls[CORE_SIDE_RIGHT] = _createBulkheadDoorButton(sector1, y, CORE_SIDE_RIGHT);

		// Set up the ForceBridge and Button with an appropriate Orchestrator
		auto orchSystem = make_shared<BulkheadDoorOrchestratedSystem>(mOrchestrator);

		orchSystem->setBulkheadDoor(door);

		auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_SIDE_LEFT].sector->_getObject(createdCtrls[CORE_SIDE_LEFT].index));
		auto leftButton = dynamic_pointer_cast<Button>(buttonSectorObject->_getObject());
		
		dooSectorObject->addController("LeftController", leftButton);
		orchSystem->addButton(leftButton);
		
		buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_SIDE_RIGHT].sector->_getObject(createdCtrls[CORE_SIDE_RIGHT].index));
		auto rightButton = dynamic_pointer_cast<Button>(buttonSectorObject->_getObject());

		dooSectorObject->addController("RightController", rightButton);
		orchSystem->addButton(rightButton);

		mOrchestrator->addSystem(orchSystem);

		return {
			doorObject,
			{ createdCtrls[CORE_SIDE_LEFT], createdCtrls[CORE_SIDE_RIGHT] }
		};
	}

	Building::CreateObjectResult Building::addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset)
	{
		auto sector = _getSector(sectorIndex);
		auto ctrl = _createSectorButton("Lightswitch", sector, xOffset, 0, CORE_BUTTON_F_AUTO_REENABLE);

		// Set up the Location and Button with an appropriate Orchestrator
		auto orchSystem = make_shared<LightingOrchestratedSystem>(mOrchestrator);

		orchSystem->setSector(sector);

		auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(ctrl.sector->_getObject(ctrl.index));
		orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

		mOrchestrator->addSystem(orchSystem);

		return ctrl;
	}

	void Building::addSectorWalkway(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset)
	{
		string caller = format("Building::addSectorWalkway({}, {}, {})", sectorIndex, deckIndex, xOffset);

		validateObjectAllowedInSector(caller, SectorObjectType::Walkway, sectorIndex);

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(sector->getCellX() + xOffset, sector->getCellY() + deckIndex);

		if (cellDef.floorType == CellFloorType::Ground)
		{
			throw BuildingException(this, format("{} - floor is ground, so cannot be a walkway", caller));
		}

		auto const& [walkwayIndex, walkwayObjType, walkwaySector] = 
			createWalkway(layerIndex, sector->getCellX() + xOffset, sector->getCellY() + deckIndex);

		// Set layers
		cellDef.floorIndex = walkwayIndex;
		cellDef.floorType = CellFloorType::Walkway;
	}

	void Building::addSectorMarker(uint32_t sectorIndex, uint32_t deckIndex, float xOffset, uint32_t* vertexIdentifier)
	{
		string caller = format("Building::addSectorMarker({}, {}, {})", sectorIndex, deckIndex, xOffset);

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(sector->getCellX() + (uint32_t)xOffset, sector->getCellY() + deckIndex);

		if (cellDef.floorType != CellFloorType::Ground)
		{
			throw BuildingException(this, format("{} - floor is not ground, so cannot add a marker here", caller));
		}

		auto const& [markerIndex, markerObjType, markerSector] =
			createMarker(layerIndex, sector->getCellX(), sector->getCellY() + deckIndex, xOffset, vertexIdentifier);

		cellDef.markers.push_back(markerIndex);
	}

	Building::CreateForceBridgeResult Building::addSectorForceBridge(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateForceBridgeOptions const& options)
	{
		ASSERT_SIDE_OK(options.fromSide);

		string caller = format("Building::addSectorForceBridge({}, {}, {}, {}, {}, {})", sectorIndex, deckIndex, xOffset, options.width, options.fromSide, options.startExtended);

		validateSectorForceBridgeOptions(caller, options);
		validateObjectAllowedInSector(caller, SectorObjectType::ForceBridge, sectorIndex);

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();

		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + deckIndex;

		validateCellHasNoFloorType(caller, "ForceBridge", layerIndex, x, y);
		validateSpaceOnlyInOneSector(caller, layerIndex, x, y, options.width, 1);

		auto layer = getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(x, y);

		if (options.width > CORE_FORCEBRIDGE_MAX_SIZE)
		{
			throw BuildingException(this, format("{} - width={} is larger than {}", caller, options.width, CORE_FORCEBRIDGE_MAX_SIZE));
		}

		if (xOffset == 0 || xOffset == (getCellsWide() - 1))
		{
			throw BuildingException(this, format("{} - xOffset={} is out of bounds: force bridges cannot be on the end of a building", caller, xOffset));
		}

		// Make sure the Cells on either side are not air
		auto const& cellDef0 = layer->getCellDefinition(x - 1, y);

		if (cellDef0.floorType != CellFloorType::Walkway && cellDef0.floorType != CellFloorType::Ground)
		{
			throw BuildingException(this, format("{} - cell to the left has floorType={}, so cannot place force bridge here", caller, getCellFloorTypeString(cellDef0.floorType)));
		}

		auto const& cellDef1 = layer->getCellDefinition(x + 1, y);

		if (cellDef1.floorType != CellFloorType::Walkway && cellDef1.floorType != CellFloorType::Ground)
		{
			throw BuildingException(this, format("{} - cell to the right has floorType={}, so cannot place force bridge here", caller, getCellFloorTypeString(cellDef1.floorType)));
		}

		// Create
		auto fbObject = createForceBridge(layerIndex, x, y, options);

		// Set layers
		cellDef.floorIndex = fbObject.index;
		cellDef.floorType = CellFloorType::ForceBridge;

		// See if we need an Controller
		CreateObjectResult createdCtrls[2];

		if (options.extensible)
		{
			if (x == sector->getCellX0() && (x + options.width - 1) == sector->getCellX1())
			{
				throw BuildingException(this, format("{} - No space to place Buttons for Ladder", caller));
			}

			auto forceBridge = dynamic_pointer_cast<ForceBridgeSectorObject>(fbObject.sector->_getObject(fbObject.index))->getForceBridge();

			// Set up the ForceBridge and Button with an appropriate Orchestrator
			auto orchSystem = make_shared<ButtonExtensibleObjectOrchestratedSystem>(mOrchestrator);

			orchSystem->setExtensibleObject(forceBridge);

			if (options.controllerCount > 0)
			{
				createdCtrls[0] = _createForceBridgeButton(fbObject.sector, x, y, options.width, options.fromSide, 0);

				auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[0].sector->_getObject(createdCtrls[0].index));
				orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));
			}
			if (options.controllerCount > 1)
			{
				createdCtrls[1] = _createForceBridgeButton(fbObject.sector, x, y, options.width, 1 - options.fromSide, 0);

				auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[1].sector->_getObject(createdCtrls[1].index));
				orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));
			}

			mOrchestrator->addSystem(orchSystem);
		}

		return {
			fbObject,
			{ createdCtrls[0], createdCtrls[1] }
		};
	}

	Building::CreateLadderResult Building::addSectorLadder(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateLadderOptions const& options)
	{
		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();

		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + deckIndex;

		auto y0 = y;
		auto y1 = y + options.decksHigh - 1;

		// Checks
		string caller = format("Building::addSectorLadder({}, {}, {}, {})", sectorIndex, deckIndex, xOffset, options.startExtended);

		validateSectorLadderOptions(caller, options);
		validateObjectAllowedInSector(caller, SectorObjectType::Ladder, sectorIndex);
		validateSpaceOnlyInOneSector(caller, layerIndex, x, y, 1, options.decksHigh);

		if (options.decksHigh < 2)
		{
			throw BuildingException(this, format("{} - Ladder at {},{} must be at least 2 decks high", caller, x, y));
		}

		if (deckIndex > sector->getDecksHigh())
		{
			throw BuildingException(this, format("{} - deckIndex={} out of bounds", caller, deckIndex));
		}

		validateCellHasNoObject(caller, layerIndex, x, y);
		validateCellTraversableOnFoot(caller, "Ladder", layerIndex, x, y0);
		validateCellTraversableOnFoot(caller, "Ladder", layerIndex, x, y1);

		// Create
		auto layer = getLayer(layerIndex);
		auto const& cellDef0 = layer->getCellDefinition(x, y0);
		auto const& cellDef1 = layer->getCellDefinition(x, y1);

		auto ladderObject = createLadderSectorObject(layerIndex, x, y, options);

		for (uint32_t iy = y0; iy <= y1; ++iy)
		{
			auto& cellDef = layer->getCellDefinition(x, iy);

			cellDef.sectorObjectType = SectorObjectType::Ladder;
			cellDef.sectorObjectIndex = ladderObject.index;
		}

		// See if we need an Controller
		CreateObjectResult createdCtrls[2];

		if (options.extensible)
		{
			if (x == sector->getCellX0() && x == sector->getCellX1())
			{
				throw BuildingException(this, format("{} - No space to place Buttons for Ladder", caller));
			}

			auto ladder = dynamic_pointer_cast<LadderSectorObject>(ladderObject.sector->_getObject(ladderObject.index))->getLadder();

			// Set up the ForceBridge and Button with an appropriate Orchestrator
			auto orchSystem = make_shared<ButtonExtensibleObjectOrchestratedSystem>(mOrchestrator);

			orchSystem->setExtensibleObject(ladder);

			// Try and place on the right of the Ladder, unless it's at the end of the Location
			int side = x == sector->getCellX1() ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;

			// Lower
			createdCtrls[CORE_LEVEL_LOW] = _createLadderButton(ladderObject.sector, x, y0, side, 0);

			auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_LEVEL_LOW].sector->_getObject(createdCtrls[CORE_LEVEL_LOW].index));
			orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

			// Upper
			createdCtrls[CORE_LEVEL_HIGH] = _createLadderButton(ladderObject.sector, x, y1, side, 0);

			buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_LEVEL_HIGH].sector->_getObject(createdCtrls[CORE_LEVEL_HIGH].index));
			orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

			mOrchestrator->addSystem(orchSystem);
		}

		return {
			ladderObject,
			{ createdCtrls[CORE_LEVEL_LOW], createdCtrls[CORE_LEVEL_HIGH] }
		};
	}

	Building::CreatePlatformLiftResult Building::addSectorPlatformLift(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateLiftOptions const& options)
	{
		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);

		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + deckIndex;

		// Checks
		string caller = format("Building::addSectorPlatformLift({}, {}, {})", sectorIndex, deckIndex, xOffset);

		validateObjectAllowedInSector(caller, SectorObjectType::Lift, sectorIndex);
		validateSpaceOnlyInOneSector(caller, layerIndex, x, y, options.cellsWide, options.stopOffsets.back());

		if (options.stopOffsets.size() < 2)
		{
			throw BuildingException(this, format("{} - PlatformLift at {},{} must have at least 2 stops", caller, x, y));
		}

		if (deckIndex > sector->getDecksHigh())
		{
			throw BuildingException(this, format("{} - deckIndex={} out of bounds", caller, deckIndex));
		}

		bool buttonSidesOk[2] = { true, true };
		for (auto stopOffset : options.stopOffsets)
		{
			auto iy = y + stopOffset;

			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				validateCellHasNoObject(caller, layerIndex, ix, iy);
				validateCellTraversableOnFoot(caller, "PlatformLift", layerIndex, ix, iy);
			}

			// Check there is space for buttons.  On each stop, there needs to be, to the left or the right,
			// an empty cell with a floor.  Check for both and left and right sides, and update variable if
			// there is no space. 
			if (x == 0 || (x - 1) <= sector->getCellX0() || !layer->getCellDefinition(x - 1, iy).isTraversableOnFoot())
			{
				buttonSidesOk[CORE_SIDE_LEFT] = false;
			}
			if ((x + options.cellsWide) >= sector->getCellX1() || !layer->getCellDefinition(x + 1, iy).isTraversableOnFoot())
			{
				buttonSidesOk[CORE_SIDE_RIGHT] = false;
			}
		}

		int side;
		if (buttonSidesOk[CORE_SIDE_RIGHT])
		{
			side = CORE_SIDE_RIGHT;
		}
		else if (buttonSidesOk[CORE_SIDE_LEFT])
		{
			side = CORE_SIDE_LEFT;
		}
		else
		{
			throw BuildingException(this, format("{} - No space to place Buttons for PlatformLift", caller));
		}

		// Create
		auto liftObject = createPlatformLiftSectorObject(layerIndex, x, y, options);

		for (uint32_t iy = y; iy < y + (options.stopOffsets.back() + 1); ++iy)
		{
			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);
					
				cellDef.sectorObjectType = SectorObjectType::Lift;
				cellDef.sectorObjectIndex = liftObject.index;
			}
		}

		// Orchestrate
		CreatePlatformLiftResult liftRes;

		liftRes.lift = liftObject;

		auto orchSystem = make_shared<PlatformLiftOrchestratedSystem>(mOrchestrator);
		liftRes.orchSystem = orchSystem;

		orchSystem->setLift(dynamic_pointer_cast<LiftSectorObject>(liftObject.sector->_getObject(liftObject.index))->getLift());

		// Create Buttons
		for (auto stopOffset : options.stopOffsets)
		{
			// Set up controllers and orchestration
			CreateObjectResult ctrl = _createPlatformLiftButton(sector, x, y + stopOffset, options.cellsWide, side, CORE_BUTTON_F_AUTO_REENABLE, &ctrl.index);
			
			auto button = dynamic_pointer_cast<Button>(ctrl.sector->_getObject(ctrl.index)->_getObject());

			orchSystem->addStop(button);

			liftRes.buttons.push_back(ctrl);
		}

		mOrchestrator->addSystem(orchSystem);

		return liftRes;
	}

	void Building::buildGraph()
	{
		mVertexControllers = mGraph->build();
		mGraph->validate();
	}

	void Building::finishBuild()
	{
		try
		{
			buildGraph();
		}
		catch(BuildingException& e)
		{
			auto const& graphLog = mGraph->getBuildLog();

			mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());

			throw e;
		}

		auto const& graphLog = mGraph->getBuildLog();

		mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());
	}

	shared_ptr<const Sector> Building::getSectorAtPosition(uint32_t layerIndex, float x, float y) const
	{
		// Get cell
		int cellX = (int)x;
		int cellY = (int)y;

		if (cellX < 0 || cellY < 0 || cellX >= (int)getCellsWide() || cellY >= (int)getDecksHigh())
		{
			return nullptr;
		}

		auto const& layer = mLayers[layerIndex];

		auto const& cellDef = layer->getCellDefinition(cellX, cellY);
		return cellDef.sectorIndex != ~0u ? getSector(cellDef.sectorIndex) : nullptr;
	}

	Agent* Building::getAgentAtPosition(uint32_t layerIndex, float x, float y) const
	{
		// Get cell
		int cellX = (int)x;
		int cellY = (int)y;

		auto const& layer = mLayers[layerIndex];

		try
		{
			auto const& cellDef = layer->getCellDefinition(cellX, cellY);
			auto sector = getSector(cellDef.sectorIndex);
		
			auto const& agents = sector->getAgents();

			for (auto agent : agents)
			{
				auto bounds = agent->getBounds();

				if (bounds.pointInShape(x, y))
				{
					return agent;
				}
			}
		}
		catch (BuildingException&)
		{
			// This should just catch an out-of-bounds validation check, which we don't mind failing.
			return nullptr;
		}

		return nullptr;
	}

	shared_ptr<Useable> Building::getUseableObjectAtPosition(uint32_t layerIndex, float x, float y, bool includeDisabled, shared_ptr<SectorObject>* sectorObject) const
	{
		// Get cell
		int cellX = (int)x;
		int cellY = (int)y;

		auto const& layer = mLayers[layerIndex];

		try
		{
			auto const& cellDef = layer->getCellDefinition(cellX, cellY);
			auto sector = getSector(cellDef.sectorIndex);

			switch (sector->getType())
			{
			case SectorType::Location:
				return cellDef.occupied() ? sector->getUseableObjectAtPosition(x, y, includeDisabled, sectorObject) : nullptr;

			case SectorType::Ladder:
				return dynamic_pointer_cast<const LadderTransit>(sector)->getLadder();

			default:
				return nullptr;
			}
		}
		catch (BuildingException&)
		{
			// This should just catch an out-of-bounds validation check, which we don't mind failing.
			return nullptr;
		}
	}

	AgentId Building::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId, uint32_t deckOffset, float xOffset)
	{
		if (!agent)
		{
			throw invalid_argument("Building cannot own a null Agent");
		}
		if (mAgentIds.contains(agent.get()))
		{
			throw invalid_argument("Agent is already owned by this Building");
		}

		auto sector = _getSector(sectorId);
		auto rawAgent = agent.get();
		sector->enterAgent(rawAgent, deckOffset, xOffset);
		auto id = mAgents.add(std::move(agent));
		mAgentIds.emplace(rawAgent, id);

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::AgentAdded;
		event.agent = makeAgentSnapshot(rawAgent);
		mEvents.push_back(std::move(event));
		return id;
	}

	AgentId Building::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId)
	{
		if (!agent)
		{
			throw invalid_argument("Building cannot own a null Agent");
		}
		if (mAgentIds.contains(agent.get()))
		{
			throw invalid_argument("Agent is already owned by this Building");
		}

		auto sector = _getSector(sectorId);
		auto rawAgent = agent.get();
		sector->enterAgent(rawAgent);
		auto id = mAgents.add(std::move(agent));
		mAgentIds.emplace(rawAgent, id);

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::AgentAdded;
		event.agent = makeAgentSnapshot(rawAgent);
		mEvents.push_back(std::move(event));
		return id;
	}

	AgentId Building::createAgent(string const& name, uint32_t sectorId, uint32_t deckOffset, float xOffset)
	{
		return addOwnedAgentToSector(make_unique<Agent>(name), sectorId, deckOffset, xOffset);
	}

	AgentId Building::createAgent(string const& name, uint32_t sectorId)
	{
		return addOwnedAgentToSector(make_unique<Agent>(name), sectorId);
	}

	void Building::addAgentToSector(Agent* agent, uint32_t sectorId, uint32_t deckOffset, float xOffset)
	{
		if (agent && mAgentIds.contains(agent))
		{
			throw invalid_argument("Agent is already owned by this Building");
		}
		(void)addOwnedAgentToSector(unique_ptr<Agent>(agent), sectorId, deckOffset, xOffset);
	}

	void Building::addAgentToSector(Agent* agent, uint32_t sectorId)
	{
		if (agent && mAgentIds.contains(agent))
		{
			throw invalid_argument("Agent is already owned by this Building");
		}
		(void)addOwnedAgentToSector(unique_ptr<Agent>(agent), sectorId);
	}

	void Building::wakeAllAgents()
	{
		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			agent->wake();
		}
	}

	AgentSnapshot Building::makeAgentSnapshot(Agent const* agent) const
	{
		AgentSnapshot result;
		result.id = getAgentId(agent);
		result.name = agent->getName();
		result.sectorId = SectorId{ agent->getSector() ? (uint64_t)agent->getSector()->getIndex() + 1 : 0 };
		result.localPosition = agent->getLocalPosition();
		result.globalPosition = agent->getGlobalPosition();
		result.hasPath = (bool)agent->getPath();
		result.targetPathNode = agent->getPathTargetNodeIndex();
		result.pathNodeCount = result.hasPath ? (uint32_t)agent->getPath()->nodes.size() : 0;

		switch (agent->getState())
		{
		case Agent::State::Idle:
			result.state = AgentPathState::Idle;
			break;
		case Agent::State::MovingToVertex:
			result.state = AgentPathState::MovingToVertex;
			break;
		case Agent::State::UnderVertexControl:
			result.state = AgentPathState::UnderVertexControl;
			break;
		}

		return result;
	}

	InteractionPointSnapshot Building::makeInteractionPointSnapshot(InteractionPointId id, InteractionPoint const& point) const
	{
		return { id, point.getName() };
	}

	DeviceOperationSnapshot Building::makeDeviceOperationSnapshot(DeviceOperationId id, DeviceOperation const& operation) const
	{
		return { id, operation.getName(), operation.getRequester(), operation.getState() };
	}

	TraversalResourceSnapshot Building::makeTraversalResourceSnapshot(TraversalResourceId id, TraversalResource const& resource) const
	{
		return { id, resource.getName() };
	}

	AgentId Building::getAgentId(Agent const* agent) const
	{
		auto found = mAgentIds.find(agent);
		return found == mAgentIds.end() ? AgentId{} : found->second;
	}

	EntityLookup<Agent> Building::lookupAgent(AgentId id)
	{
		auto entity = mAgents.find(id);
		return entity ? EntityLookup<Agent>{ entity, {} }
			: EntityLookup<Agent>{ nullptr, format("Agent handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<Agent const> Building::lookupAgent(AgentId id) const
	{
		auto entity = mAgents.find(id);
		return entity ? EntityLookup<Agent const>{ entity, {} }
			: EntityLookup<Agent const>{ nullptr, format("Agent handle {} is invalid or has been removed", id.value) };
	}

	EntityRemovalResult Building::removeAgent(AgentId id)
	{
		auto found = lookupAgent(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		if (found.entity->getState() != Agent::State::Idle)
		{
			return { false, format("Agent handle {} is active and cannot be removed safely", id.value) };
		}

		vector<DeviceOperationId> ownedOperations;
		for (auto const& [operationId, operation] : mDeviceOperations.entries())
		{
			if (operation->getRequester() == id)
			{
				ownedOperations.push_back(operationId);
			}
		}
		for (auto operationId : ownedOperations)
		{
			auto operation = mDeviceOperations.find(operationId);
			if (operation->getState() == DeviceOperationState::Pending
				|| operation->getState() == DeviceOperationState::Running)
			{
				operation->setState(DeviceOperationState::Cancelled);
			}
			(void)removeDeviceOperation(operationId);
		}

		auto snapshot = makeAgentSnapshot(found.entity);
		if (auto sector = const_cast<Sector*>(found.entity->getSector()))
		{
			sector->exitAgent(found.entity);
		}
		mAgentIds.erase(found.entity);
		mAgents.remove(id);

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::AgentRemoved;
		event.agent = std::move(snapshot);
		mEvents.push_back(std::move(event));
		return { true, {} };
	}

	InteractionPointId Building::createInteractionPoint(string const& name)
	{
		auto id = mInteractionPoints.add(unique_ptr<InteractionPoint>(new InteractionPoint(name)));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::InteractionPointAdded;
		event.interactionPoint = makeInteractionPointSnapshot(id, *mInteractionPoints.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<InteractionPoint> Building::lookupInteractionPoint(InteractionPointId id)
	{
		auto entity = mInteractionPoints.find(id);
		return entity ? EntityLookup<InteractionPoint>{ entity, {} }
			: EntityLookup<InteractionPoint>{ nullptr, format("InteractionPoint handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<InteractionPoint const> Building::lookupInteractionPoint(InteractionPointId id) const
	{
		auto entity = mInteractionPoints.find(id);
		return entity ? EntityLookup<InteractionPoint const>{ entity, {} }
			: EntityLookup<InteractionPoint const>{ nullptr, format("InteractionPoint handle {} is invalid or has been removed", id.value) };
	}

	EntityRemovalResult Building::removeInteractionPoint(InteractionPointId id)
	{
		auto found = lookupInteractionPoint(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		auto snapshot = makeInteractionPointSnapshot(id, *found.entity);
		mInteractionPoints.remove(id);

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::InteractionPointRemoved;
		event.interactionPoint = std::move(snapshot);
		mEvents.push_back(std::move(event));
		return { true, {} };
	}

	DeviceOperationId Building::createDeviceOperation(string const& name, AgentId requester)
	{
		auto agent = lookupAgent(requester);
		if (!agent)
		{
			throw invalid_argument(format("Cannot create DeviceOperation: {}", agent.diagnostic));
		}

		auto id = mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester)));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.deviceOperation = makeDeviceOperationSnapshot(id, *mDeviceOperations.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<DeviceOperation> Building::lookupDeviceOperation(DeviceOperationId id)
	{
		auto entity = mDeviceOperations.find(id);
		return entity ? EntityLookup<DeviceOperation>{ entity, {} }
			: EntityLookup<DeviceOperation>{ nullptr, format("DeviceOperation handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<DeviceOperation const> Building::lookupDeviceOperation(DeviceOperationId id) const
	{
		auto entity = mDeviceOperations.find(id);
		return entity ? EntityLookup<DeviceOperation const>{ entity, {} }
			: EntityLookup<DeviceOperation const>{ nullptr, format("DeviceOperation handle {} is invalid or has been removed", id.value) };
	}

	EntityRemovalResult Building::removeDeviceOperation(DeviceOperationId id)
	{
		auto found = lookupDeviceOperation(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		auto snapshot = makeDeviceOperationSnapshot(id, *found.entity);
		mDeviceOperations.remove(id);

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::DeviceOperationRemoved;
		event.deviceOperation = std::move(snapshot);
		mEvents.push_back(std::move(event));
		return { true, {} };
	}

	TraversalResourceId Building::createTraversalResource(string const& name)
	{
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(name)));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	EntityLookup<TraversalResource> Building::lookupTraversalResource(TraversalResourceId id)
	{
		auto entity = mTraversalResources.find(id);
		return entity ? EntityLookup<TraversalResource>{ entity, {} }
			: EntityLookup<TraversalResource>{ nullptr, format("TraversalResource handle {} is invalid or has been removed", id.value) };
	}

	EntityLookup<TraversalResource const> Building::lookupTraversalResource(TraversalResourceId id) const
	{
		auto entity = mTraversalResources.find(id);
		return entity ? EntityLookup<TraversalResource const>{ entity, {} }
			: EntityLookup<TraversalResource const>{ nullptr, format("TraversalResource handle {} is invalid or has been removed", id.value) };
	}

	EntityRemovalResult Building::removeTraversalResource(TraversalResourceId id)
	{
		auto found = lookupTraversalResource(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		auto snapshot = makeTraversalResourceSnapshot(id, *found.entity);
		mTraversalResources.remove(id);

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceRemoved;
		event.traversalResource = std::move(snapshot);
		mEvents.push_back(std::move(event));
		return { true, {} };
	}

	SimulationSnapshot Building::getSimulationSnapshot() const
	{
		SimulationSnapshot result;
		result.tick = mSimulationTick;
		result.agents.reserve(mAgents.entries().size());
		result.interactionPoints.reserve(mInteractionPoints.entries().size());
		result.deviceOperations.reserve(mDeviceOperations.entries().size());
		result.traversalResources.reserve(mTraversalResources.entries().size());

		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			result.agents.push_back(makeAgentSnapshot(agent.get()));
		}
		for (auto const& [id, point] : mInteractionPoints.entries())
		{
			result.interactionPoints.push_back(makeInteractionPointSnapshot(id, *point));
		}
		for (auto const& [id, operation] : mDeviceOperations.entries())
		{
			result.deviceOperations.push_back(makeDeviceOperationSnapshot(id, *operation));
		}
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			result.traversalResources.push_back(makeTraversalResourceSnapshot(id, *resource));
		}

		return result;
	}

	void Building::runSimulationPhase(SimulationPhase phase)
	{
		mCurrentPhase = phase;
		auto const timestep = getFixedTimestep();

		switch (phase)
		{
		case SimulationPhase::ResourceAdvancement:
			for (auto const& sector : mSectors)
			{
				sector->advanceResources(timestep);
			}
			break;

		case SimulationPhase::IntentCollection:
		case SimulationPhase::Allocation:
			// Explicit seams for replacement traversal tasks. Legacy agents collect
			// and allocate synchronously during the movement phase for now.
			break;

		case SimulationPhase::Movement:
			for (auto const& [id, agent] : mAgents.entries())
			{
				(void)id;
				agent->update(timestep);
			}

			for (auto const& vertexController : mVertexControllers)
			{
				vertexController->update(timestep);
			}
			break;

		case SimulationPhase::Commit:
			// Reserved for atomic transition commits introduced by the traversal
			// protocol. Legacy movement commits synchronously.
			break;

		case SimulationPhase::CleanupAndEventPublication:
		case SimulationPhase::None:
			break;
		}
	}

	void Building::publishTickEvents(SimulationSnapshot const& before)
	{
		for (auto phase : { SimulationPhase::ResourceAdvancement, SimulationPhase::IntentCollection,
			SimulationPhase::Allocation, SimulationPhase::Movement, SimulationPhase::Commit,
			SimulationPhase::CleanupAndEventPublication })
		{
			SimulationEvent event;
			event.sequence = mNextEventSequence++;
			event.tick = mSimulationTick;
			event.type = SimulationEventType::PhaseCompleted;
			event.phase = phase;
			mEvents.push_back(std::move(event));
		}

		auto after = getSimulationSnapshot();
		for (auto const& current : after.agents)
		{
			auto previousIt = find_if(before.agents.begin(), before.agents.end(), [&](AgentSnapshot const& candidate)
			{
				return candidate.id == current.id;
			});
			if (previousIt == before.agents.end())
			{
				continue;
			}

			auto const& previous = *previousIt;
			auto changed = current.sectorId != previous.sectorId
				|| current.localPosition != previous.localPosition
				|| current.globalPosition != previous.globalPosition
				|| current.state != previous.state
				|| current.hasPath != previous.hasPath
				|| current.targetPathNode != previous.targetPathNode
				|| current.pathNodeCount != previous.pathNodeCount;

			if (changed)
			{
				SimulationEvent event;
				event.sequence = mNextEventSequence++;
				event.tick = mSimulationTick;
				event.type = SimulationEventType::AgentChanged;
				event.phase = SimulationPhase::CleanupAndEventPublication;
				event.hasPreviousAgent = true;
				event.previousAgent = previous;
				event.agent = current;
				mEvents.push_back(std::move(event));
			}
		}

		for (auto const& current : after.deviceOperations)
		{
			auto previousIt = find_if(before.deviceOperations.begin(), before.deviceOperations.end(), [&](DeviceOperationSnapshot const& candidate)
			{
				return candidate.id == current.id;
			});
			if (previousIt != before.deviceOperations.end() && previousIt->state != current.state)
			{
				SimulationEvent event;
				event.sequence = mNextEventSequence++;
				event.tick = mSimulationTick;
				event.type = SimulationEventType::DeviceOperationChanged;
				event.phase = SimulationPhase::CleanupAndEventPublication;
				event.deviceOperation = current;
				mEvents.push_back(std::move(event));
			}
		}
	}

	void Building::advanceTick()
	{
		auto before = getSimulationSnapshot();
		++mSimulationTick;

		runSimulationPhase(SimulationPhase::ResourceAdvancement);
		runSimulationPhase(SimulationPhase::IntentCollection);
		runSimulationPhase(SimulationPhase::Allocation);
		runSimulationPhase(SimulationPhase::Movement);
		runSimulationPhase(SimulationPhase::Commit);
		runSimulationPhase(SimulationPhase::CleanupAndEventPublication);
		publishTickEvents(before);
		mCurrentPhase = SimulationPhase::None;
	}

	void Building::advanceTicks(uint64_t count)
	{
		for (uint64_t i = 0; i < count; ++i)
		{
			advanceTick();
		}
	}

	void Building::update(float elapsedSeconds)
	{
		if (elapsedSeconds <= 0.0f)
		{
			return;
		}

		mAccumulatedTime += elapsedSeconds;
		auto const timestep = (double)getFixedTimestep();
		while (mAccumulatedTime + timestep * 1e-9 >= timestep)
		{
			advanceTick();
			mAccumulatedTime -= timestep;
		}

		if (mAccumulatedTime < 0.0)
		{
			mAccumulatedTime = 0.0;
		}
	}

	uint64_t Building::getSimulationTick() const
	{
		return mSimulationTick;
	}

	SimulationPhase Building::getCurrentSimulationPhase() const
	{
		return mCurrentPhase;
	}

	vector<SimulationEvent> Building::consumeSimulationEvents()
	{
		auto result = std::move(mEvents);
		mEvents.clear();
		return result;
	}

} // core