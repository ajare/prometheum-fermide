#include <algorithm>
#include <cmath>
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

	Building::CreateDoorOptions Building::ManualDoor1Options{ 1, { false, false }, false, DoorActivationMode::Manual };
	Building::CreateDoorOptions Building::OrchButtonDoor1Options{ 1, { true, true }, true, DoorActivationMode::RemoteControlled };
	Building::CreateDoorOptions Building::NonOrchButtonDoor1Options{ 1, { true, true }, false, DoorActivationMode::Unavailable };
	Building::CreateDoorOptions Building::ManualDoor2Options{ 2, { false, false }, false, DoorActivationMode::Manual };
	Building::CreateDoorOptions Building::OrchButtonDoor2Options{ 2, { true, true }, true, DoorActivationMode::RemoteControlled };
	Building::CreateDoorOptions Building::NonOrchButtonDoor2Options{ 2, { true, true }, false, DoorActivationMode::Unavailable };

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
		if (options.holdOpenSeconds < 0.0f)
		{
			throw BuildingException(this, format("{} - Door hold-open time cannot be negative.", caller));
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
		if (options.agentSpacing <= 0.0f)
		{
			throw BuildingException(this, format("{} - Ladder agent spacing must be positive.", caller));
		}
		if (options.directionalBatchLimit == 0)
		{
			throw BuildingException(this, format("{} - Ladder directional batch limit must be positive.", caller));
		}
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

		validateSectorLadderOptions(caller, options);

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
		auto traversalResource = createLadderTraversalResource("Ladder capacity", ladder,
			SectorId{ (uint64_t)sectorIndex + 1 }, options.agentSpacing,
			options.directionalBatchLimit);
		ladder->configureTraversal(traversalResource);
		auto registerExtensionControl = [&](CreateObjectResult const& control)
		{
			auto object = control.sector->_getObject(control.index);
			DeviceCommand command;
			command.type = DeviceCommandType::SetExtendedState;
			command.desiredState = true;
			command.traversalResource = traversalResource;
			auto point = createInteractionPoint("Ladder extension control",
				SectorId{ (uint64_t)control.sector->getIndex() + 1 },
				object->getPosition() + object->getSize() * 0.5f, 0.15f,
				getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
			addTraversalControl(traversalResource, point);
		};

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
			registerExtensionControl(createdCtrls[CORE_LEVEL_LOW]);

			auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_LEVEL_LOW].sector->_getObject(createdCtrls[CORE_LEVEL_LOW].index));
			orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

			// Upper
			locX0 = foreSector1->getCellX();
			locX1 = locX1 + foreSector1->getCellsWide();
			side = x == locX1 ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;

			createdCtrls[CORE_LEVEL_HIGH] = _createLadderButton(foreSector1, x, y1, side, 0);
			registerExtensionControl(createdCtrls[CORE_LEVEL_HIGH]);

			buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_LEVEL_HIGH].sector->_getObject(createdCtrls[CORE_LEVEL_HIGH].index));
			orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

			mOrchestrator->addSystem(orchSystem);
		}

		return {
			{ ~0u, SectorObjectType::Ladder, ladderSector },
			{ createdCtrls[0], createdCtrls[1] },
			traversalResource
		};
	}

	uint32_t Building::addStaircase(uint32_t y, uint32_t x, uint32_t decksHigh, int mountSide)
	{
		return addStaircase(y, x, CreateStaircaseOptions{ decksHigh, mountSide }).sectorIndex;
	}

	Building::CreateStaircaseResult Building::addStaircase(uint32_t y, uint32_t x,
		CreateStaircaseOptions const& options)
	{
		auto decksHigh = options.decksHigh;
		auto mountSide = options.mountSide;
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

		if (options.directionalCapacity > 0 && options.directionalBatchLimit == 0)
		{
			throw BuildingException(this, format("{} - Narrow staircase directional batch limit must be positive.", caller));
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

		TraversalResourceId traversalResource;
		if (options.directionalCapacity > 0)
		{
			auto staircaseTransit = dynamic_pointer_cast<StaircaseTransit>(_getSector(sectorIndex));
			auto staircase = staircaseTransit->getStaircase();
			traversalResource = createStaircaseTraversalResource("Narrow staircase capacity",
				staircase, SectorId{ (uint64_t)sectorIndex + 1 }, options.directionalCapacity,
				options.directionalBatchLimit);
			staircase->configureTraversal(traversalResource);
		}

		return { sectorIndex, traversalResource };
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
			auto doorRes = _addSectorDoor(y + stopOffset, x, { options.cellsWide, { true, false }, false, DoorActivationMode::Unavailable });
			
			auto door = static_pointer_cast<DoorSectorObject>(doorRes.door.sector->_getObject(doorRes.door.index))->getDoor();
			
			auto ctrl = doorRes.controllers[CORE_LAYER_FORE];
			auto button = dynamic_pointer_cast<Button>(ctrl.sector->_getObject(ctrl.index)->_getObject());

			orchSystem->addStop(door, button);

			liftRes.doors.push_back(doorRes);
		}

		mOrchestrator->addSystem(orchSystem);

		// The replacement lift coordinator owns the car manifest and schedule. Each
		// landing remains a distinct threshold resource, linked to this coordinator.
		vector<LiftStop> liftStops;
		for (uint32_t i = 0; i < options.stopOffsets.size(); ++i)
		{
			auto location = getSector(getLayer(CORE_LAYER_FORE)->getCellDefinition(x, y + options.stopOffsets[i]).sectorIndex);
			liftStops.push_back({ SectorId{ (uint64_t)location->getIndex() + 1 },
				(float)(y + options.stopOffsets[i]), liftRes.doors[i].traversalResource, {} });
		}
		auto coordinator = createLiftTraversalResource("Lift journey", lift,
			SectorId{ (uint64_t)liftTransit->getIndex() + 1 }, liftStops);
		lift->configureTraversal(coordinator);
		liftRes.traversalResource = coordinator;
		auto liftResource = mTraversalResources.find(coordinator);
		liftResource->mCapacityPositions[0] = {
			x + options.cellsWide * 0.5f - liftTransit->getPosition().x, 0.0f };
		for (uint32_t i = 0; i < liftRes.doors.size(); ++i)
		{
			auto landing = mTraversalResources.find(liftRes.doors[i].traversalResource);
			landing->mLiftCoordinator = coordinator;
			landing->mLiftStopIndex = i;

			auto const& controller = liftRes.doors[i].controllers[CORE_LAYER_FORE];
			auto controlObject = controller.sector->_getObject(controller.index);
			auto controlPosition = controlObject->getPosition() + controlObject->getSize() * 0.5f;
			DeviceCommand call;
			call.type = DeviceCommandType::CallLift;
			call.traversalResource = coordinator;
			call.stopIndex = i;
			auto point = createInteractionPoint("Lift landing call",
				liftStops[i].locationSector, controlPosition, 0.15f, getFixedTimestep(),
				{ { call, InteractionBindingRequirement::Required } });
			landing->mControls.push_back(point);
			liftResource->mLiftStops[i].callControl = point;

			DeviceCommand select;
			select.type = DeviceCommandType::SelectLiftDestination;
			select.traversalResource = coordinator;
			select.stopIndex = i;
			auto selector = createInteractionPoint("Lift destination selector",
				liftResource->mLiftSector, { x + options.cellsWide * 0.5f, liftResource->mLiftPosition },
				0.25f, getFixedTimestep(), { { select, InteractionBindingRequirement::Required } });
			liftResource->mControls.push_back(selector);
			if (i == 0) liftRes.interiorSelector = selector;
		}
		liftResource->mLiftSelector = liftRes.interiorSelector;
		
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
				auto doorRes = _addSectorDoor(y, x + stopOffset + doorX, { doorWidth, { true, false }, false, DoorActivationMode::Unavailable });

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
		auto doorSectorObject = dynamic_pointer_cast<DoorSectorObject>(doorObject.sector->_getObject(doorObject.index));
		auto door = doorSectorObject->getDoor();
		auto traversalResource = createDoorTraversalResource(
			format("Door at {},{}", x, y), door, options.activationMode, options.holdOpenSeconds);
		door->configureTraversal(options.activationMode, traversalResource, options.holdOpenSeconds);
		if (options.crossingLanes != 0)
		{
			configureDoorCrossingLanes(traversalResource, options.crossingLanes);
		}

		// Door approaches are explicit resource geometry. Prefer the longer clear
		// horizontal run in each source sector and place the first waiter one safe
		// spacing away from the threshold whenever the room permits it.
		auto const threshold = Vector2{ x + cellsWide * 0.5f, (float)y };
		for (auto const& sector : sectors)
		{
			auto const leftExtent = threshold.x - (sector->getCellX0() + CORE_AGENT_MAX_WIDTH * 0.5f);
			auto const rightExtent = (sector->getCellX1() + 1.0f - CORE_AGENT_MAX_WIDTH * 0.5f) - threshold.x;
			auto const direction = rightExtent > leftExtent ? Vector2::UNIT_X : Vector2::NEGATIVE_UNIT_X;
			auto const available = max(0.0f, max(leftExtent, rightExtent));
			auto const firstDistance = min((float)CORE_DOOR_QUEUE_STOP_WIDTH, available);
			auto const origin = threshold + direction * firstDistance;
			configureDoorQueueLane(traversalResource,
				SectorId{ (uint64_t)sector->getIndex() + 1 }, origin, direction,
				max(0.0f, available - firstDistance));
		}

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

				if (options.activationMode == DoorActivationMode::RemoteControlled)
				{
					auto controlObject = sectors[i]->_getObject(createdCtrls[i].index);
					auto controlPosition = controlObject->getPosition() + controlObject->getSize() * 0.5f;
					DeviceCommand command;
					command.type = DeviceCommandType::OpenDoor;
					command.desiredState = true;
					command.traversalResource = traversalResource;
					auto point = createInteractionPoint("Door button",
						SectorId{ (uint64_t)sectors[i]->getIndex() + 1 }, controlPosition,
						0.15f, getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
					addTraversalControl(traversalResource, point);
				}
			}
		}

		// Orchestrate
		shared_ptr<ButtonDoorOrchestratedSystem> orchSystem;

		if (options.orchestrate)
		{
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
			orchSystem,
			traversalResource
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

		auto forceBridge = dynamic_pointer_cast<ForceBridgeSectorObject>(
			fbObject.sector->_getObject(fbObject.index))->getForceBridge();
		auto traversalResource = createForceBridgeTraversalResource("Force bridge", forceBridge);
		forceBridge->configureTraversal(traversalResource);

		// See if we need an Controller
		CreateObjectResult createdCtrls[2];

		if (options.extensible)
		{
			if (x == sector->getCellX0() && (x + options.width - 1) == sector->getCellX1())
			{
				throw BuildingException(this, format("{} - No space to place Buttons for Ladder", caller));
			}

			// Set up the ForceBridge and Button with an appropriate Orchestrator
			auto orchSystem = make_shared<ButtonExtensibleObjectOrchestratedSystem>(mOrchestrator);

			orchSystem->setExtensibleObject(forceBridge);

			if (options.controllerCount > 0)
			{
				createdCtrls[0] = _createForceBridgeButton(fbObject.sector, x, y, options.width, options.fromSide, 0);
				auto object = createdCtrls[0].sector->_getObject(createdCtrls[0].index);
				DeviceCommand command{ DeviceCommandType::SetExtendedState, {}, true, traversalResource };
				auto point = createInteractionPoint("Force bridge extension control",
					SectorId{ (uint64_t)createdCtrls[0].sector->getIndex() + 1 },
					object->getPosition() + object->getSize() * 0.5f, 0.15f,
					getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
				addTraversalControl(traversalResource, point);

				auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[0].sector->_getObject(createdCtrls[0].index));
				orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));
			}
			if (options.controllerCount > 1)
			{
				createdCtrls[1] = _createForceBridgeButton(fbObject.sector, x, y, options.width, 1 - options.fromSide, 0);
				auto object = createdCtrls[1].sector->_getObject(createdCtrls[1].index);
				DeviceCommand command{ DeviceCommandType::SetExtendedState, {}, true, traversalResource };
				auto point = createInteractionPoint("Force bridge extension control",
					SectorId{ (uint64_t)createdCtrls[1].sector->getIndex() + 1 },
					object->getPosition() + object->getSize() * 0.5f, 0.15f,
					getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
				addTraversalControl(traversalResource, point);

				auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[1].sector->_getObject(createdCtrls[1].index));
				orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));
			}

			mOrchestrator->addSystem(orchSystem);
		}

		return {
			fbObject,
			{ createdCtrls[0], createdCtrls[1] },
			traversalResource
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
		auto ladder = dynamic_pointer_cast<LadderSectorObject>(
			ladderObject.sector->_getObject(ladderObject.index))->getLadder();
		auto traversalResource = createLadderTraversalResource("Ladder capacity", ladder,
			SectorId{ (uint64_t)sectorIndex + 1 }, options.agentSpacing,
			options.directionalBatchLimit);
		ladder->configureTraversal(traversalResource);
		auto registerExtensionControl = [&](CreateObjectResult const& control)
		{
			auto object = control.sector->_getObject(control.index);
			DeviceCommand command;
			command.type = DeviceCommandType::SetExtendedState;
			command.desiredState = true;
			command.traversalResource = traversalResource;
			auto point = createInteractionPoint("Ladder extension control",
				SectorId{ (uint64_t)control.sector->getIndex() + 1 },
				object->getPosition() + object->getSize() * 0.5f, 0.15f,
				getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
			addTraversalControl(traversalResource, point);
		};

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

			// Set up the ForceBridge and Button with an appropriate Orchestrator
			auto orchSystem = make_shared<ButtonExtensibleObjectOrchestratedSystem>(mOrchestrator);

			orchSystem->setExtensibleObject(ladder);

			// Try and place on the right of the Ladder, unless it's at the end of the Location
			int side = x == sector->getCellX1() ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;

			// Lower
			createdCtrls[CORE_LEVEL_LOW] = _createLadderButton(ladderObject.sector, x, y0, side, 0);
			registerExtensionControl(createdCtrls[CORE_LEVEL_LOW]);

			auto buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_LEVEL_LOW].sector->_getObject(createdCtrls[CORE_LEVEL_LOW].index));
			orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

			// Upper
			createdCtrls[CORE_LEVEL_HIGH] = _createLadderButton(ladderObject.sector, x, y1, side, 0);
			registerExtensionControl(createdCtrls[CORE_LEVEL_HIGH]);

			buttonSectorObject = dynamic_pointer_cast<ButtonSectorObject>(createdCtrls[CORE_LEVEL_HIGH].sector->_getObject(createdCtrls[CORE_LEVEL_HIGH].index));
			orchSystem->addButton(dynamic_pointer_cast<Button>(buttonSectorObject->_getObject()));

			mOrchestrator->addSystem(orchSystem);
		}

		return {
			ladderObject,
			{ createdCtrls[CORE_LEVEL_LOW], createdCtrls[CORE_LEVEL_HIGH] },
			traversalResource
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
		rawAgent->attachToBuilding(this);
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
		rawAgent->attachToBuilding(this);
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
		result.hasLocomotionTask = agent->hasActiveLocomotionTask();
		result.traversalRequest = agent->getTraversalRequestId();
		result.traversalPermit = agent->getTraversalPermitId();
		for (auto const& [requestId, request] : mInteractionRequests.entries())
		{
			if (request->getActor() == result.id && request->getResult() == InteractionResult::Pending)
			{
				result.interactionRequest = requestId;
				result.hasLocomotionTask = true;
				break;
			}
		}

		switch (agent->getState())
		{
		case Agent::State::Idle:
			result.state = AgentPathState::Idle;
			break;
		case Agent::State::MovingToVertex:
			result.state = AgentPathState::MovingToVertex;
			break;
		case Agent::State::WaitingForTraversal:
			result.state = AgentPathState::WaitingForTraversal;
			break;
		case Agent::State::TraversingEdge:
			result.state = AgentPathState::TraversingEdge;
			break;
		case Agent::State::AwaitingTraversalCommit:
			result.state = AgentPathState::AwaitingTraversalCommit;
			break;
		case Agent::State::UnderVertexControl:
			result.state = AgentPathState::UnderVertexControl;
			break;
		}

		return result;
	}

	InteractionPointSnapshot Building::makeInteractionPointSnapshot(InteractionPointId id, InteractionPoint const& point) const
	{
		return { id, point.getName(), point.getSector(), point.getPosition(), point.getReach(),
			point.getDurationTicks(), point.getActiveRequest() };
	}

	InteractionRequestSnapshot Building::makeInteractionRequestSnapshot(InteractionRequestId id, InteractionRequest const& request) const
	{
		InteractionRequestSnapshot result;
		result.id = id;
		result.point = request.getPoint();
		result.actor = request.getActor();
		result.result = request.getResult();
		for (auto const& [operation, requirement] : request.getOperations())
		{
			(void)requirement;
			result.operations.push_back(operation);
		}
		return result;
	}

	DeviceOperationSnapshot Building::makeDeviceOperationSnapshot(DeviceOperationId id, DeviceOperation const& operation) const
	{
		DeviceOperationSnapshot result;
		result.id = id;
		result.name = operation.getName();
		result.requester = operation.getRequester();
		result.requesters.assign(operation.getRequesters().begin(), operation.getRequesters().end());
		result.hasCommand = operation.hasCommand();
		if (result.hasCommand)
		{
			result.command = operation.getCommand();
		}
		result.state = operation.getState();
		return result;
	}

	TraversalResourceSnapshot Building::makeTraversalResourceSnapshot(TraversalResourceId id, TraversalResource const& resource) const
	{
		TraversalResourceSnapshot result;
		result.id = id;
		result.name = resource.getName();
		result.isDoor = resource.mDoor != nullptr;
		result.isLadder = resource.mLadder != nullptr;
		result.isForceBridge = resource.mForceBridge != nullptr;
		result.isLift = resource.mLift != nullptr;
		result.liftMoving = resource.mLiftMoving;
		result.liftCarDoorOpen = resource.mLiftCarDoorOpen;
		result.liftCurrentStop = resource.mLiftCurrentStop;
		result.liftTargetStop = resource.mLiftTargetStop;
		result.liftPosition = resource.mLiftPosition;
		result.liftSector = resource.mLiftSector;
		result.liftPassenger = resource.mLiftPassenger;
		result.liftAdmissionReservation = resource.mLiftAdmissionReservation;
		result.liftDestinationStop = resource.mLiftDestinationStop;
		result.liftSelector = resource.mLiftSelector;
		result.liftAligned = !resource.mLiftMoving && resource.mLiftCurrentStop < resource.mLiftStops.size()
			&& abs(resource.mLiftPosition - resource.mLiftStops[resource.mLiftCurrentStop].globalPosition) < 0.001f;
		result.isExtensible = resource.mExtensible && resource.mExtensible->isExtensible();
		result.extended = resource.mExtensible && resource.mExtensible->isExtended();
		result.retractionPending = resource.mRetractionPending;
		result.extensionRequestLeaseCount = (uint32_t)resource.mExtensionRequestLeases.size();
		result.extensionOccupantLeaseCount = (uint32_t)resource.mExtensionOccupantLeases.size();
		result.isNarrowStaircase = resource.mStaircase != nullptr;
		result.enabled = resource.mEnabled;
		result.capacity = resource.mCapacity;
		result.agentSpacing = resource.mLadderSpacing;
		result.capacitySector = resource.mLadderSector;
		result.admissionQueue = resource.mAdmissionQueue;
		result.activeDirection = resource.mActiveDirection;
		result.directionalBatchCount = resource.mDirectionalBatchCount;
		result.directionalBatchLimit = resource.mDirectionalBatchLimit;
		for (auto requestId : resource.mAdmissionQueue)
		{
			if (auto request = mTraversalRequests.find(requestId))
			{
				if (request->mDirection == TraversalDirection::Ascending) ++result.ascendingWaitingCount;
				else if (request->mDirection == TraversalDirection::Descending) ++result.descendingWaitingCount;
			}
		}
		for (uint32_t i = 0; i < resource.mCapacityPositions.size(); ++i)
		{
			result.capacityPositions.push_back({ i, resource.mCapacityPositions[i],
				resource.mOccupants[i], resource.mAdmissionReservations[i] });
			if (resource.mOccupants[i]) ++result.occupantCount;
			if (resource.mAdmissionReservations[i]) ++result.admissionReservationCount;
		}
		result.doorActivationMode = resource.mDoorActivationMode;
		result.holdOpenTicks = resource.mHoldOpenTicks;
		result.openLeaseCount = (uint32_t)resource.mOpenLeases.size();
		for (auto const& [leaseId, lease] : resource.mOpenLeases)
		{
			(void)leaseId;
			switch (lease.kind)
			{
			case DoorOpenLeaseKind::Preparation: ++result.preparationLeaseCount; break;
			case DoorOpenLeaseKind::Crossing: ++result.crossingLeaseCount; break;
			case DoorOpenLeaseKind::ExternalHoldOpen: ++result.externalOpenLeaseCount; break;
			}
		}
		for (auto const& [sensor, observation] : resource.mSensorObservations)
		{
			(void)sensor;
			result.presenceObserved = result.presenceObserved || observation == DoorSensorObservation::Presence;
			result.obstructionObserved = result.obstructionObserved || observation == DoorSensorObservation::Obstruction;
		}
		result.controls = resource.mControls;
		result.activePreparation = resource.mActivePreparation;
		result.preparationOperator = resource.mPreparationOperator;
		for (uint32_t i = 0; i < resource.mCrossingOwners.size(); ++i)
		{
			result.crossingLanes.push_back({ i, resource.mCrossingOwners[i] });
			if (!result.crossingOwner && resource.mCrossingOwners[i])
			{
				result.crossingOwner = resource.mCrossingOwners[i];
			}
		}
		for (auto const& lane : resource.mQueueLanes)
		{
			if (!lane.sector)
			{
				continue;
			}
			DoorQueueLaneSnapshot laneSnapshot;
			laneSnapshot.sector = lane.sector;
			laneSnapshot.origin = lane.origin;
			laneSnapshot.direction = lane.direction;
			laneSnapshot.extent = lane.extent;
			laneSnapshot.queue = lane.queue;
			for (uint32_t i = 0; i < lane.positions.size(); ++i)
			{
				laneSnapshot.positions.push_back({ i, lane.positions[i], lane.positionOwners[i] });
			}
			result.queueLanes.push_back(std::move(laneSnapshot));
		}
		if (resource.mDoor)
		{
			result.doorOpenPercentage = resource.mDoor->getOpenPercentage();
			switch (resource.mDoor->getState())
			{
			case OpenableObject::State::Closed: result.doorState = DoorSnapshotState::Closed; break;
			case OpenableObject::State::Opening: result.doorState = DoorSnapshotState::Opening; break;
			case OpenableObject::State::Open: result.doorState = DoorSnapshotState::Open; break;
			case OpenableObject::State::Closing: result.doorState = DoorSnapshotState::Closing; break;
			}
		}
		return result;
	}

	TraversalRequestSnapshot Building::makeTraversalRequestSnapshot(TraversalRequestId id, TraversalRequest const& request) const
	{
		TraversalRequestSnapshot result{ id, request.getOwner(), request.getEdgeType(), request.getSourceSector(),
			request.getDestinationSector(), request.getSourceEndpoint(), request.getDestinationEndpoint(),
			request.getState(), request.getResource(), request.getPreparationOperation(), request.getPermit(),
			request.getFailureReason() };
		result.queueTicket = request.mQueueTicket;
		result.queuedAtTick = request.mQueuedAtTick;
		result.queueApproach = request.mQueueApproach;
		result.hasQueuePosition = request.mQueuePosition != ~0u;
		result.queuePosition = request.mQueuePosition;
		result.hasCrossingLane = request.mCrossingLane != ~0u;
		result.crossingLane = request.mCrossingLane;
		result.hasCapacityPosition = request.mCapacityPosition != ~0u;
		result.capacityPosition = request.mCapacityPosition;
		result.direction = request.mDirection;
		result.positionAssignedAtTick = request.mPositionAssignedAtTick;
		result.lastPositionProgressTick = request.mLastPositionProgressTick;
		result.positionRetryAtTick = request.mPositionRetryAtTick;
		result.positionRetryCount = request.mPositionRetryCount;
		if (result.hasQueuePosition)
		{
			if (auto resource = mTraversalResources.find(request.mResource);
				resource && request.mQueueApproach < resource->mQueueLanes.size()
				&& request.mQueuePosition < resource->mQueueLanes[request.mQueueApproach].positions.size())
			{
				result.queuePositionTarget = resource->mQueueLanes[request.mQueueApproach].positions[request.mQueuePosition];
			}
		}
		return result;
	}

	TraversalPermitSnapshot Building::makeTraversalPermitSnapshot(TraversalPermitId id, TraversalPermit const& permit) const
	{
		return { id, permit.getRequest(), permit.getOwner(), permit.getState(), permit.getExpiresAtTick() };
	}

	TraversalRequestId Building::createTraversalRequest(Agent const& agent, shared_ptr<const Edge> const& edge,
		shared_ptr<const Vertex> const& source, shared_ptr<const Vertex> const& destination)
	{
		auto owner = getAgentId(&agent);
		if (!owner || !edge || !source || !destination)
		{
			throw invalid_argument("A traversal request requires an owned Agent, Edge, and two endpoints");
		}

		auto sourceSector = SectorId{ (uint64_t)source->getSector()->getIndex() + 1 };
		auto destinationSector = SectorId{ (uint64_t)destination->getSector()->getIndex() + 1 };
		auto id = mTraversalRequests.add(unique_ptr<TraversalRequest>(new TraversalRequest(owner,
			edge->getType(), sourceSector, destinationSector, source->getPosition(), destination->getPosition())));
		auto request = mTraversalRequests.find(id);
		request->mResource = edge->getTraversalResourceId();
		if (auto resource = mTraversalResources.find(request->mResource); resource)
		{
			if (resource->mExtensible && resource->mExtensionRequestLeases.insert(id).second)
				resource->mExtensible->acquireExtensionLease();
			if (resource->mDoor && !resource->mLiftCoordinator) attachDoorQueueTicket(id, *resource);
			else if ((resource->mLadder || resource->mStaircase)
				&& isLadderAdmission(*request, *resource))
				attachLadderAdmissionRequest(id, *resource);
		}

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalRequestAdded;
		event.phase = mCurrentPhase;
		event.traversalRequest = makeTraversalRequestSnapshot(id, *mTraversalRequests.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	void Building::attachDoorQueueTicket(TraversalRequestId requestId, TraversalResource& resource)
	{
		auto request = mTraversalRequests.find(requestId);
		if (!request || request->mQueueTicket)
		{
			return;
		}
		for (uint32_t i = 0; i < resource.mQueueLanes.size(); ++i)
		{
			if (resource.mQueueLanes[i].sector != request->mSourceSector)
			{
				continue;
			}
			request->mQueueTicket = QueueTicketId{ mNextQueueTicketValue++ };
			request->mQueuedAtTick = mSimulationTick;
			request->mQueueApproach = i;
			resource.mQueueLanes[i].queue.push_back(requestId);
			refreshDoorQueuePositions(resource);
			return;
		}
	}

	void Building::refreshDoorQueuePositions(TraversalResource& resource)
	{
		for (auto& lane : resource.mQueueLanes)
		{
			map<TraversalRequestId, uint32_t> previousPositions;
			for (uint32_t i = 0; i < lane.positionOwners.size(); ++i)
			{
				if (lane.positionOwners[i]) previousPositions[lane.positionOwners[i]] = i;
			}
			fill(lane.positionOwners.begin(), lane.positionOwners.end(), TraversalRequestId{});
			uint32_t position = 0;
			for (auto requestId : lane.queue)
			{
				auto request = mTraversalRequests.find(requestId);
				if (!request || request->mState != TraversalRequestState::Pending)
				{
					continue;
				}
				request->mQueuePosition = ~0u;
				if (auto agent = mAgents.find(request->mOwner)) agent->mTraversalLocalGoal.reset();

				// Operators and timed-out assignments keep their logical place while
				// releasing the scarce physical position.
				if (resource.mPreparationOperator == requestId
					|| mSimulationTick < request->mPositionRetryAtTick)
				{
					continue;
				}
				if (position < lane.positionOwners.size())
				{
					lane.positionOwners[position] = requestId;
					request->mQueuePosition = position;
					auto agent = mAgents.find(request->mOwner);
					if (agent)
					{
						agent->mTraversalLocalGoal = lane.positions[position];
						auto distance = agent->getGlobalPosition().distanceTo(lane.positions[position]);
						auto previous = previousPositions.find(requestId);
						if (previous == previousPositions.end() || previous->second != position)
						{
							request->mPositionAssignedAtTick = mSimulationTick;
							request->mLastPositionProgressTick = mSimulationTick;
							request->mBestPositionDistance = distance;
						}
					}
					++position;
				}
			}
		}
	}

	void Building::updateTraversalProgressAndTimeouts()
	{
		vector<TraversalPermitId> expiredPermits;
		for (auto const& [permitId, permit] : mTraversalPermits.entries())
		{
			if (permit->mState != TraversalPermitState::Active) continue;
			auto request = mTraversalRequests.find(permit->mRequest);
			auto agent = request ? mAgents.find(request->mOwner) : nullptr;
			if (!request || !agent) { expiredPermits.push_back(permitId); continue; }
			auto distance = agent->getGlobalPosition().distanceTo(request->mDestinationEndpoint);
			if (distance + 0.001f < permit->mBestDestinationDistance)
			{
				permit->mBestDestinationDistance = distance;
				permit->mExpiresAtTick = mSimulationTick + mTraversalWaitingPolicy.permitProgressTimeoutTicks;
			}
			else if (mSimulationTick >= permit->mExpiresAtTick)
			{
				expiredPermits.push_back(permitId);
			}
		}
		for (auto permitId : expiredPermits) expireTraversalPermit(permitId);

		vector<TraversalRequestId> unreachableRequests;
		for (auto const& [resourceId, resource] : mTraversalResources.entries())
		{
			(void)resourceId;
			if (!resource->mDoor) continue;
			bool refresh = false;
			for (auto& lane : resource->mQueueLanes)
			{
				for (auto requestId : lane.queue)
				{
					auto request = mTraversalRequests.find(requestId);
					if (!request) continue;
					if (request->mQueuePosition == ~0u)
					{
						if (request->mPositionRetryAtTick != 0
							&& mSimulationTick >= request->mPositionRetryAtTick)
						{
							request->mPositionRetryAtTick = 0;
							refresh = true;
						}
						continue;
					}
					auto agent = mAgents.find(request->mOwner);
					if (!agent || request->mQueuePosition >= lane.positions.size()) continue;
					auto distance = agent->getGlobalPosition().distanceTo(lane.positions[request->mQueuePosition]);
					if (distance + 0.001f < request->mBestPositionDistance)
					{
						request->mBestPositionDistance = distance;
						request->mLastPositionProgressTick = mSimulationTick;
					}
					else if (distance > 0.001f && mSimulationTick - request->mLastPositionProgressTick
						>= mTraversalWaitingPolicy.localGoalTimeoutTicks)
					{
						request->mQueuePosition = ~0u;
						request->mPositionRetryAtTick = mSimulationTick
							+ mTraversalWaitingPolicy.localGoalRetryDelayTicks;
						++request->mPositionRetryCount;
						if (agent) agent->mTraversalLocalGoal.reset();
						refresh = true;
						if (request->mPositionRetryCount > mTraversalWaitingPolicy.maximumLocalGoalRetries)
						{
							unreachableRequests.push_back(requestId);
						}
					}
				}
			}
			if (refresh) refreshDoorQueuePositions(*resource);
		}
		for (auto requestId : unreachableRequests)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::LocalGoalUnreachable);
		}
	}

	void Building::expireTraversalPermit(TraversalPermitId permitId)
	{
		auto permit = mTraversalPermits.find(permitId);
		if (!permit || permit->mState != TraversalPermitState::Active) return;
		auto requestId = permit->mRequest;
		auto request = mTraversalRequests.find(requestId);
		if (!request) return;

		permit->mState = TraversalPermitState::Cancelled;
		SimulationEvent permitChanged;
		permitChanged.sequence = mNextEventSequence++;
		permitChanged.tick = mSimulationTick;
		permitChanged.type = SimulationEventType::TraversalPermitChanged;
		permitChanged.phase = mCurrentPhase;
		permitChanged.traversalPermit = makeTraversalPermitSnapshot(permitId, *permit);
		mEvents.push_back(std::move(permitChanged));

		request->mPermit = {};
		request->mState = TraversalRequestState::Pending;
		request->mFailureReason = TraversalFailureReason::PermitExpired;
		if (auto resource = mTraversalResources.find(request->mResource);
			resource && (resource->mLadder || resource->mStaircase))
		{
			if (isLadderAdmission(*request, *resource))
			{
				releaseLadderAdmission(requestId, *resource);
				attachLadderAdmissionRequest(requestId, *resource);
			}
		}
		else if (auto resource = mTraversalResources.find(request->mResource);
			resource && resource->mDoor && resource->mLiftCoordinator)
		{
			for (auto& owner : resource->mCrossingOwners) if (owner == requestId) owner = {};
			if (!request->mPreparationLease)
				request->mPreparationLease = acquireDoorOpenLease(*resource,
					DoorOpenLeaseKind::Preparation, requestId);
			if (request->mCrossingLease) releaseDoorOpenLease(*resource, request->mCrossingLease);
			request->mCrossingLease = {};
			request->mCrossingLane = ~0u;
		}
		else if (auto resource = mTraversalResources.find(request->mResource); resource && resource->mDoor)
		{
			for (auto& owner : resource->mCrossingOwners) if (owner == requestId) owner = {};
			// Downgrade crossing authority to preparation before releasing its safety
			// lease, so even a zero-hold door cannot close around a stalled agent.
			if (!request->mPreparationLease && resource->mEnabled)
			{
				request->mPreparationLease = acquireDoorOpenLease(*resource,
					DoorOpenLeaseKind::Preparation, requestId);
			}
			if (request->mCrossingLease) releaseDoorOpenLease(*resource, request->mCrossingLease);
			request->mCrossingLease = {};
			request->mCrossingLane = ~0u;
			auto& queue = resource->mQueueLanes[request->mQueueApproach].queue;
			queue.push_back(requestId);
			sort(queue.begin(), queue.end(), [&](TraversalRequestId lhs, TraversalRequestId rhs)
			{
				return mTraversalRequests.find(lhs)->mQueueTicket < mTraversalRequests.find(rhs)->mQueueTicket;
			});
			refreshDoorQueuePositions(*resource);
		}
		if (auto agent = mAgents.find(request->mOwner); agent && agent->mTraversalTask)
		{
			agent->mTraversalTask->permit = {};
			agent->mState = Agent::State::WaitingForTraversal;
		}

		auto removed = makeTraversalPermitSnapshot(permitId, *permit);
		mTraversalPermits.remove(permitId);
		SimulationEvent permitRemoved;
		permitRemoved.sequence = mNextEventSequence++;
		permitRemoved.tick = mSimulationTick;
		permitRemoved.type = SimulationEventType::TraversalPermitRemoved;
		permitRemoved.phase = mCurrentPhase;
		permitRemoved.traversalPermit = std::move(removed);
		mEvents.push_back(std::move(permitRemoved));

		SimulationEvent requestChanged;
		requestChanged.sequence = mNextEventSequence++;
		requestChanged.tick = mSimulationTick;
		requestChanged.type = SimulationEventType::TraversalRequestChanged;
		requestChanged.phase = mCurrentPhase;
		requestChanged.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
		mEvents.push_back(std::move(requestChanged));
	}

	void Building::tryGrantDoorQueue(TraversalResource& resource)
	{
		if (!resource.mEnabled || !resource.mDoor || !resource.mDoor->isOpen())
		{
			return;
		}

		for (uint32_t crossingLane = 0; crossingLane < resource.mCrossingOwners.size(); ++crossingLane)
		{
			if (resource.mCrossingOwners[crossingLane])
			{
				continue;
			}
			TraversalRequestId selected;
			for (auto const& lane : resource.mQueueLanes)
			{
				if (lane.queue.empty()) continue;
				auto candidateId = lane.queue.front();
				auto candidate = mTraversalRequests.find(candidateId);
				if (!candidate || candidate->mState != TraversalRequestState::Pending
					|| candidate->mQueuePosition == ~0u || resource.mPreparationOperator == candidateId)
				{
					continue;
				}
				auto agent = mAgents.find(candidate->mOwner);
				if (!agent || agent->getGlobalPosition().distanceTo(
					lane.positions[candidate->mQueuePosition]) > 0.001f)
				{
					continue;
				}
				if (!selected)
				{
					selected = candidateId;
					continue;
				}
				auto current = mTraversalRequests.find(selected);
				if (candidate->mQueuedAtTick < current->mQueuedAtTick
					|| (candidate->mQueuedAtTick == current->mQueuedAtTick && candidate->mOwner < current->mOwner))
				{
					selected = candidateId;
				}
			}
			if (!selected) break;

			auto selectedRequest = mTraversalRequests.find(selected);
			auto& queueLane = resource.mQueueLanes[selectedRequest->mQueueApproach];
			queueLane.queue.erase(remove(queueLane.queue.begin(), queueLane.queue.end(), selected), queueLane.queue.end());
			selectedRequest->mQueuePosition = ~0u;
			selectedRequest->mCrossingLane = crossingLane;
			resource.mCrossingOwners[crossingLane] = selected;
			if (auto agent = mAgents.find(selectedRequest->mOwner)) agent->mTraversalLocalGoal.reset();
			refreshDoorQueuePositions(resource);
			grantTraversalRequest(selected);
		}
	}

	void Building::releaseDoorQueueOwnership(TraversalRequestId requestId, TraversalResource& resource)
	{
		for (auto& lane : resource.mQueueLanes)
		{
			lane.queue.erase(remove(lane.queue.begin(), lane.queue.end(), requestId), lane.queue.end());
		}
		for (auto& owner : resource.mCrossingOwners)
		{
			if (owner == requestId) owner = {};
		}
		if (auto request = mTraversalRequests.find(requestId))
		{
			request->mQueuePosition = ~0u;
			request->mCrossingLane = ~0u;
			if (auto agent = mAgents.find(request->mOwner))
			{
				agent->mTraversalLocalGoal.reset();
			}
		}
		refreshDoorQueuePositions(resource);
	}

	bool Building::isLadderAdmission(TraversalRequest const& request,
		TraversalResource const& resource) const
	{
		if ((!resource.mLadder && !resource.mStaircase)
			|| request.mDestinationSector != resource.mLadderSector)
		{
			return false;
		}
		if (resource.mStaircase)
		{
			// Ordinary mount edges remain unconstrained. A narrow staircase owns
			// capacity only for the actual sloping, cross-deck edge.
			return request.mSourceSector == resource.mLadderSector
				&& request.mEdgeType == EdgeType::Staircase;
		}
		if (request.mSourceSector != resource.mLadderSector)
		{
			return true;
		}
		if (request.mEdgeType != EdgeType::Ladder)
		{
			return false;
		}
		return find(resource.mOccupants.begin(), resource.mOccupants.end(), request.mOwner)
			== resource.mOccupants.end();
	}

	void Building::attachLadderAdmissionRequest(TraversalRequestId requestId,
		TraversalResource& resource)
	{
		auto request = mTraversalRequests.find(requestId);
		if (!request) return;
		if (request->mDirection == TraversalDirection::None)
		{
			if (request->mSourceSector == resource.mLadderSector)
			{
				auto deltaY = request->mDestinationEndpoint.y - request->mSourceEndpoint.y;
				request->mDirection = deltaY >= 0.0f
					? TraversalDirection::Ascending : TraversalDirection::Descending;
			}
			else
			{
				auto objectY = resource.mLadder ? resource.mLadder->getPosition().y
					: resource.mStaircase->getPosition().y;
				auto objectHeight = resource.mLadder ? resource.mLadder->getSize().y
					: resource.mStaircase->getSize().y;
				request->mDirection = request->mSourceEndpoint.y < objectY + objectHeight * 0.5f
					? TraversalDirection::Ascending : TraversalDirection::Descending;
			}
		}
		if (find(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(), requestId)
			== resource.mAdmissionQueue.end())
		{
			resource.mAdmissionQueue.push_back(requestId);
			sort(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(),
				[&](auto lhs, auto rhs)
				{
					auto left = mTraversalRequests.find(lhs);
					auto right = mTraversalRequests.find(rhs);
					if (!left || !right) return lhs < rhs;
					return left->mQueuedAtTick != right->mQueuedAtTick
						? left->mQueuedAtTick < right->mQueuedAtTick
						: (left->mOwner != right->mOwner ? left->mOwner < right->mOwner : lhs < rhs);
				});
		}
	}

	void Building::tryGrantLadderAdmissions(TraversalResource& resource)
	{
		if (!resource.mEnabled || (!resource.mLadder && !resource.mStaircase)
			|| (resource.mExtensible && !resource.mExtensible->isExtended())) return;

		auto hasInFlight = any_of(resource.mOccupants.begin(), resource.mOccupants.end(),
			[](auto id) { return (bool)id; })
			|| any_of(resource.mAdmissionReservations.begin(), resource.mAdmissionReservations.end(),
				[](auto id) { return (bool)id; });
		auto oldestDirection = [&]()
		{
			for (auto requestId : resource.mAdmissionQueue)
			{
				if (auto request = mTraversalRequests.find(requestId);
					request && request->mState == TraversalRequestState::Pending)
					return request->mDirection;
			}
			return TraversalDirection::None;
		};
		auto hasWaitingDirection = [&](TraversalDirection direction)
		{
			return any_of(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(),
				[&](auto id)
				{
					auto request = mTraversalRequests.find(id);
					return request && request->mState == TraversalRequestState::Pending
						&& request->mDirection == direction;
				});
		};

		if (resource.mActiveDirection == TraversalDirection::None)
		{
			resource.mActiveDirection = oldestDirection();
			resource.mDirectionalBatchCount = 0;
		}
		else if (!hasInFlight)
		{
			auto opposite = resource.mActiveDirection == TraversalDirection::Ascending
				? TraversalDirection::Descending : TraversalDirection::Ascending;
			if (hasWaitingDirection(opposite)
				&& (!hasWaitingDirection(resource.mActiveDirection)
					|| resource.mDirectionalBatchCount >= resource.mDirectionalBatchLimit))
			{
				resource.mActiveDirection = opposite;
				resource.mDirectionalBatchCount = 0;
			}
			else if (!hasWaitingDirection(resource.mActiveDirection))
			{
				resource.mActiveDirection = oldestDirection();
				resource.mDirectionalBatchCount = 0;
			}
		}

		auto opposite = resource.mActiveDirection == TraversalDirection::Ascending
			? TraversalDirection::Descending : TraversalDirection::Ascending;
		if (hasWaitingDirection(opposite)
			&& resource.mDirectionalBatchCount >= resource.mDirectionalBatchLimit)
			return;

		for (uint32_t position = 0; position < resource.mCapacity; ++position)
		{
			if (resource.mOccupants[position] || resource.mAdmissionReservations[position]) continue;
			auto selected = find_if(resource.mAdmissionQueue.begin(), resource.mAdmissionQueue.end(),
				[&](auto id)
				{
					auto request = mTraversalRequests.find(id);
					return request && request->mState == TraversalRequestState::Pending
						&& request->mDirection == resource.mActiveDirection;
				});
			if (selected == resource.mAdmissionQueue.end()) break;
			auto requestId = *selected;
			resource.mAdmissionQueue.erase(selected);
			auto request = mTraversalRequests.find(requestId);
			resource.mAdmissionReservations[position] = requestId;
			request->mCapacityPosition = position;
			++resource.mDirectionalBatchCount;
			grantTraversalRequest(requestId);
			if (hasWaitingDirection(opposite)
				&& resource.mDirectionalBatchCount >= resource.mDirectionalBatchLimit) break;
		}
	}

	void Building::releaseLadderAdmission(TraversalRequestId requestId,
		TraversalResource& resource)
	{
		resource.mAdmissionQueue.erase(remove(resource.mAdmissionQueue.begin(),
			resource.mAdmissionQueue.end(), requestId), resource.mAdmissionQueue.end());
		for (auto& reservation : resource.mAdmissionReservations)
		{
			if (reservation == requestId) reservation = {};
		}
		if (auto request = mTraversalRequests.find(requestId)) request->mCapacityPosition = ~0u;
	}

	void Building::releaseLadderOccupancy(AgentId agentId, TraversalResource& resource)
	{
		for (auto& occupant : resource.mOccupants)
		{
			if (occupant == agentId) occupant = {};
		}
	}

	TraversalPermitId Building::grantTraversalRequest(TraversalRequestId requestId)
	{
		auto request = mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending)
		{
			return {};
		}

		auto permitId = mTraversalPermits.add(unique_ptr<TraversalPermit>(
			new TraversalPermit(requestId, request->mOwner)));
		auto permit = mTraversalPermits.find(permitId);
		permit->mExpiresAtTick = mSimulationTick + mTraversalWaitingPolicy.permitProgressTimeoutTicks;
		if (auto agent = mAgents.find(request->mOwner))
		{
			permit->mBestDestinationDistance = agent->getGlobalPosition().distanceTo(request->mDestinationEndpoint);
		}
		request->mPermit = permitId;
		request->mState = TraversalRequestState::Granted;
		request->mFailureReason = TraversalFailureReason::None;
		if (auto resource = mTraversalResources.find(request->mResource); resource && resource->mDoor)
		{
			request->mCrossingLease = acquireDoorOpenLease(*resource, DoorOpenLeaseKind::Crossing, requestId);
			if (request->mPreparationLease)
			{
				releaseDoorOpenLease(*resource, request->mPreparationLease);
				request->mPreparationLease = {};
			}
		}

		SimulationEvent requestEvent;
		requestEvent.sequence = mNextEventSequence++;
		requestEvent.tick = mSimulationTick;
		requestEvent.type = SimulationEventType::TraversalRequestChanged;
		requestEvent.phase = mCurrentPhase;
		requestEvent.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
		mEvents.push_back(std::move(requestEvent));

		SimulationEvent permitEvent;
		permitEvent.sequence = mNextEventSequence++;
		permitEvent.tick = mSimulationTick;
		permitEvent.type = SimulationEventType::TraversalPermitAdded;
		permitEvent.phase = mCurrentPhase;
		permitEvent.traversalPermit = makeTraversalPermitSnapshot(permitId, *mTraversalPermits.find(permitId));
		mEvents.push_back(std::move(permitEvent));
		return permitId;
	}

	void Building::allocateTraversalRequest(TraversalRequestId requestId,
		shared_ptr<const Edge> const& edge, shared_ptr<const Vertex> const& destination)
	{
		auto request = mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending || !edge || !destination)
		{
			return;
		}

		if (request->mResource)
		{
			auto resource = mTraversalResources.find(request->mResource);
			if (!resource)
			{
				denyTraversalRequest(requestId);
				return;
			}
			if (resource->mLift || resource->mLiftCoordinator)
			{
				allocateLiftTraversal(requestId, *resource);
				return;
			}
			if (resource->mExtensible && !resource->mExtensible->isExtended())
			{
				allocateExtensiblePreparation(requestId, *resource);
				return;
			}
			if (resource->mForceBridge)
			{
				if (!resource->mEnabled)
				{
					denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
					return;
				}
				grantTraversalRequest(requestId);
				return;
			}
			if (resource->mLadder || resource->mStaircase)
			{
				if (isLadderAdmission(*request, *resource))
				{
					if (!resource->mEnabled)
					{
						denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
						return;
					}
					attachLadderAdmissionRequest(requestId, *resource);
					tryGrantLadderAdmissions(*resource);
				}
				else
				{
					grantTraversalRequest(requestId);
				}
				return;
			}
			if (!resource->mDoor)
			{
				denyTraversalRequest(requestId);
				return;
			}
			if (!resource->mEnabled)
			{
				return; // deactivation cleanup denies waiters after active lanes drain
			}
			if (resource->mDoorActivationMode == DoorActivationMode::Unavailable)
			{
				denyTraversalRequest(requestId);
				return;
			}
			if (!request->mPreparationLease)
			{
				request->mPreparationLease = acquireDoorOpenLease(*resource,
					DoorOpenLeaseKind::Preparation, requestId);
			}
			if (resource->mDoorActivationMode == DoorActivationMode::RemoteControlled)
			{
				allocateRemoteDoorPreparation(requestId, *resource);
				return;
			}
			if (resource->mDoor->isOpen())
			{
				tryGrantDoorQueue(*resource);
				return;
			}
			if (!request->mPreparationRequested)
			{
				request->mPreparationRequested = true;
				DeviceCommand command;
				command.type = DeviceCommandType::OpenDoor;
				command.desiredState = true;
				command.traversalResource = request->mResource;
				request->mPreparationOperation = findOrCreateDeviceOperation(command, request->mOwner);
				if (auto operation = mDeviceOperations.find(request->mPreparationOperation))
				{
					// Presence activates an automatic door; reaching the threshold and
					// requesting traversal is the manual interaction for a manual door.
					operation->mActivated = true;
				}
			}
			if (auto operation = mDeviceOperations.find(request->mPreparationOperation);
				!operation || operation->mState == DeviceOperationState::Failed
				|| operation->mState == DeviceOperationState::Rejected
				|| operation->mState == DeviceOperationState::Cancelled)
			{
				denyTraversalRequest(requestId, operation && operation->mState == DeviceOperationState::Rejected
					? TraversalFailureReason::ControlRejected : TraversalFailureReason::PreparationFailed);
			}
			return;
		}

		shared_ptr<const Agent> agentView(mAgents.find(request->mOwner), [](Agent const*) {});
		if (edge->isTraversable(destination, agentView))
		{
			grantTraversalRequest(requestId);
			return;
		}
		if (!request->mPreparationRequested)
		{
			request->mPreparationRequested = true;
			auto result = edge->requestTraversal(destination, agentView);
			if (result == EdgeTraversalRequestResult::Failed)
			{
				denyTraversalRequest(requestId);
			}
			else if (edge->isTraversable(destination, agentView))
			{
				grantTraversalRequest(requestId);
			}
		}
	}

	uint32_t Building::findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const
	{
		uint32_t best = ~0u;
		float distance = 0.0f;
		for (uint32_t i = 0; i < resource.mLiftStops.size(); ++i)
		{
			auto candidate = abs(resource.mLiftStops[i].globalPosition - endpoint.y);
			if (best == ~0u || candidate < distance)
			{
				best = i;
				distance = candidate;
			}
		}
		return best;
	}

	void Building::allocateLiftTraversal(TraversalRequestId requestId, TraversalResource& edgeResource)
	{
		auto request = mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		auto coordinatorId = edgeResource.mLift ? request->mResource : edgeResource.mLiftCoordinator;
		auto coordinator = mTraversalResources.find(coordinatorId);
		if (!coordinator || !coordinator->mLift || !coordinator->mEnabled)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			return;
		}
		auto stop = edgeResource.mLift ? findLiftStop(*coordinator, request->mDestinationEndpoint)
			: edgeResource.mLiftStopIndex;
		if (stop >= coordinator->mLiftStops.size())
		{
			denyTraversalRequest(requestId);
			return;
		}
		auto boarding = request->mSourceSector != coordinator->mLiftSector
			&& request->mDestinationSector == coordinator->mLiftSector;
		auto disembarking = request->mSourceSector == coordinator->mLiftSector
			&& request->mDestinationSector != coordinator->mLiftSector;
		auto riding = request->mSourceSector == coordinator->mLiftSector
			&& request->mDestinationSector == coordinator->mLiftSector;

		if (boarding)
		{
			if (coordinator->mLiftPassenger && coordinator->mLiftPassenger != request->mOwner) return;
			if (!request->mPreparationRequested)
			{
				if (edgeResource.mControls.empty())
				{
					denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl);
					return;
				}
				auto interactionId = requestInteractionForTraversal(edgeResource.mControls.front(), request->mOwner);
				if (!interactionId) return;
				auto interaction = mInteractionRequests.find(interactionId);
				request->mPreparationRequested = true;
				if (interaction && !interaction->mOperations.empty())
					request->mPreparationOperation = interaction->mOperations.front().first;
				return;
			}
			auto operation = mDeviceOperations.find(request->mPreparationOperation);
			if (!operation || operation->mState == DeviceOperationState::Pending
				|| operation->mState == DeviceOperationState::Running) return;
			if (operation->mState != DeviceOperationState::Succeeded)
			{
				denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
				return;
			}
			if (coordinator->mLiftMoving || coordinator->mLiftCurrentStop != stop) return;
			if (!request->mPreparationLease)
				request->mPreparationLease = acquireDoorOpenLease(edgeResource,
					DoorOpenLeaseKind::Preparation, requestId);
			if (!edgeResource.mDoor->isOpen())
			{
				if (!edgeResource.mDoor->isOpening()) edgeResource.mDoor->handleAction(ControllableActionType::Open);
				return;
			}
			if (!coordinator->mLiftAdmissionReservation)
			{
				coordinator->mLiftAdmissionReservation = requestId;
				coordinator->mAdmissionReservations[0] = requestId;
				request->mCapacityPosition = 0;
			}
			if (coordinator->mLiftAdmissionReservation != requestId) return;
			auto lane = find(edgeResource.mCrossingOwners.begin(), edgeResource.mCrossingOwners.end(), TraversalRequestId{});
			if (lane == edgeResource.mCrossingOwners.end()) return;
			request->mCrossingLane = (uint32_t)distance(edgeResource.mCrossingOwners.begin(), lane);
			*lane = requestId;
			coordinator->mLiftCarDoorOpen = true;
			grantTraversalRequest(requestId);
			return;
		}

		if (riding)
		{
			if (coordinator->mLiftPassenger != request->mOwner) return;
			if (!request->mPreparationRequested)
			{
				if (stop >= coordinator->mControls.size()) { denyTraversalRequest(requestId); return; }
				coordinator->mLiftSelector = coordinator->mControls[stop];
				auto selector = mInteractionPoints.find(coordinator->mLiftSelector);
				auto actor = mAgents.find(request->mOwner);
				if (selector && actor) selector->mPosition = actor->getGlobalPosition();
				auto interactionId = requestInteractionForTraversal(coordinator->mLiftSelector, request->mOwner);
				if (!interactionId) return;
				auto interaction = mInteractionRequests.find(interactionId);
				request->mPreparationRequested = true;
				if (interaction && !interaction->mOperations.empty())
					request->mPreparationOperation = interaction->mOperations.front().first;
				return;
			}
			auto operation = mDeviceOperations.find(request->mPreparationOperation);
			if (!operation || operation->mState == DeviceOperationState::Pending
				|| operation->mState == DeviceOperationState::Running) return;
			if (operation->mState != DeviceOperationState::Succeeded)
			{
				denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
				return;
			}
			if (!coordinator->mLiftMoving && coordinator->mLiftCurrentStop == stop)
				grantTraversalRequest(requestId);
			return;
		}

		if (disembarking)
		{
			if (coordinator->mLiftPassenger != request->mOwner
				|| coordinator->mLiftMoving || coordinator->mLiftCurrentStop != stop) return;
			if (!request->mPreparationLease)
				request->mPreparationLease = acquireDoorOpenLease(edgeResource,
					DoorOpenLeaseKind::Preparation, requestId);
			if (!edgeResource.mDoor->isOpen())
			{
				if (!edgeResource.mDoor->isOpening()) edgeResource.mDoor->handleAction(ControllableActionType::Open);
				return;
			}
			auto lane = find(edgeResource.mCrossingOwners.begin(), edgeResource.mCrossingOwners.end(), TraversalRequestId{});
			if (lane == edgeResource.mCrossingOwners.end()) return;
			request->mCrossingLane = (uint32_t)distance(edgeResource.mCrossingOwners.begin(), lane);
			*lane = requestId;
			coordinator->mLiftCarDoorOpen = true;
			grantTraversalRequest(requestId);
			return;
		}
		denyTraversalRequest(requestId);
	}

	void Building::allocateRemoteDoorPreparation(TraversalRequestId requestId, TraversalResource& resource)
	{
		constexpr uint32_t MaximumPreparationAttempts = 2;
		constexpr uint64_t RetryDelayTicks = 3;

		auto request = mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending)
		{
			return;
		}

		auto applicableControl = [&](TraversalRequest const& candidate) -> InteractionPointId
		{
			for (auto pointId : resource.mControls)
			{
				auto point = mInteractionPoints.find(pointId);
				if (point && point->mSector == candidate.mSourceSector)
				{
					return pointId;
				}
			}
			return {};
		};

		if (!applicableControl(*request))
		{
			denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl);
			return;
		}

		bool failedPreparation = false;
		bool completedPreparation = false;
		if (resource.mActivePreparation)
		{
			auto active = mInteractionRequests.find(resource.mActivePreparation);
			if (active && active->mResult == InteractionResult::Pending)
			{
				bool interactionStarted = false;
				for (auto const& [operationId, requirement] : active->mOperations)
				{
					(void)requirement;
					if (auto operation = mDeviceOperations.find(operationId))
					{
						interactionStarted = interactionStarted || operation->mActivated;
						operation->mRequesters.insert(request->mOwner);
						if (!request->mPreparationOperation)
						{
							request->mPreparationOperation = operationId;
						}
					}
				}
				request->mPreparationRequested = true;

				// If some other source achieved the desired state before the operator
				// touched the control, release its physical reservation immediately.
				if (resource.mDoor->isOpen() && !interactionStarted)
				{
					cancelInteraction(resource.mActivePreparation);
					resource.mActivePreparation = {};
					resource.mPreparationOperator = {};
					resource.mSharedPreparationOperation = {};
					refreshDoorQueuePositions(resource);
					tryGrantDoorQueue(resource);
				}
				return;
			}

			if (active && active->mResult == InteractionResult::Rejected)
			{
				resource.mActivePreparation = {};
				resource.mPreparationOperator = {};
				resource.mSharedPreparationOperation = {};
				denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
				return;
			}
			if (active && (active->mResult == InteractionResult::Succeeded
				|| active->mResult == InteractionResult::SucceededWithBestEffortFailure))
			{
				completedPreparation = true;
				resource.mPreparationAttempts = 0;
			}
			if (active && active->mResult == InteractionResult::Failed)
			{
				failedPreparation = true;
				++resource.mPreparationAttempts;
				resource.mNextPreparationTick = mSimulationTick + RetryDelayTicks;
			}
			resource.mActivePreparation = {};
			resource.mPreparationOperator = {};
			resource.mSharedPreparationOperation = {};
			refreshDoorQueuePositions(resource);
		}

		if (resource.mDoor->isOpen() && !failedPreparation
			&& (resource.mPreparationAttempts == 0 || completedPreparation))
		{
			tryGrantDoorQueue(resource);
			return;
		}

		if (resource.mPreparationAttempts >= MaximumPreparationAttempts)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::PreparationFailed);
			return;
		}
		if (mSimulationTick < resource.mNextPreparationTick)
		{
			return; // A temporary block uses a stable, tick-based retry delay.
		}

		TraversalRequestId selected;
		InteractionPointId selectedControl;
		for (auto const& [candidateId, candidate] : mTraversalRequests.entries())
		{
			if (candidate->mResource != request->mResource
				|| candidate->mState != TraversalRequestState::Pending)
			{
				continue;
			}
			auto control = applicableControl(*candidate);
			if (control && (!selected || candidateId < selected))
			{
				selected = candidateId;
				selectedControl = control;
			}
		}
		if (selected != requestId)
		{
			return;
		}

		auto interactionId = requestInteractionForTraversal(selectedControl, request->mOwner);
		if (!interactionId)
		{
			// Another locomotion/interaction task can make the control temporarily
			// busy. Do not turn that scheduling condition into permanent rejection.
			resource.mNextPreparationTick = mSimulationTick + RetryDelayTicks;
			return;
		}
		auto interaction = mInteractionRequests.find(interactionId);
		if (!interaction || interaction->mOperations.empty())
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
			return;
		}

		resource.mActivePreparation = interactionId;
		resource.mPreparationOperator = requestId;
		refreshDoorQueuePositions(resource);
		resource.mSharedPreparationOperation = interaction->mOperations.front().first;
		for (auto const& [candidateId, candidate] : mTraversalRequests.entries())
		{
			(void)candidateId;
			if (candidate->mResource != request->mResource
				|| candidate->mState != TraversalRequestState::Pending)
			{
				continue;
			}
			candidate->mPreparationRequested = true;
			candidate->mPreparationOperation = resource.mSharedPreparationOperation;
			for (auto const& [operationId, requirement] : interaction->mOperations)
			{
				(void)requirement;
				if (auto operation = mDeviceOperations.find(operationId))
				{
					operation->mRequesters.insert(candidate->mOwner);
				}
			}
		}
	}

	void Building::allocateExtensiblePreparation(TraversalRequestId requestId, TraversalResource& resource)
	{
		auto request = mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending) return;
		if (!resource.mEnabled)
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			return;
		}
		if (resource.mExtensible->isExtended())
		{
			if (resource.mLadder)
			{
				attachLadderAdmissionRequest(requestId, resource);
				tryGrantLadderAdmissions(resource);
			}
			else grantTraversalRequest(requestId);
			return;
		}

		auto controlFor = [&](TraversalRequest const& candidate)
		{
			for (auto pointId : resource.mControls)
				if (auto point = mInteractionPoints.find(pointId); point && point->mSector == candidate.mSourceSector)
					return pointId;
			return InteractionPointId{};
		};
		if (!controlFor(*request))
		{
			denyTraversalRequest(requestId, TraversalFailureReason::NoReachableControl);
			return;
		}

		if (resource.mActivePreparation)
		{
			auto active = mInteractionRequests.find(resource.mActivePreparation);
			if (active && active->mResult == InteractionResult::Pending)
			{
				request->mPreparationRequested = true;
				for (auto const& [operationId, requirement] : active->mOperations)
				{
					(void)requirement;
					request->mPreparationOperation = operationId;
					if (auto operation = mDeviceOperations.find(operationId))
						operation->mRequesters.insert(request->mOwner);
				}
				return;
			}
			if (active && (active->mResult == InteractionResult::Failed
				|| active->mResult == InteractionResult::Rejected))
			{
				denyTraversalRequest(requestId, active->mResult == InteractionResult::Rejected
					? TraversalFailureReason::ControlRejected : TraversalFailureReason::PreparationFailed);
			}
			resource.mActivePreparation = {};
			resource.mPreparationOperator = {};
			resource.mSharedPreparationOperation = {};
			if (request->mState != TraversalRequestState::Pending) return;
			if (resource.mExtensible->isExtended())
			{
				if (resource.mLadder) { attachLadderAdmissionRequest(requestId, resource); tryGrantLadderAdmissions(resource); }
				else grantTraversalRequest(requestId);
				return;
			}
		}

		TraversalRequestId selected;
		for (auto const& [candidateId, candidate] : mTraversalRequests.entries())
			if (candidate->mResource == request->mResource
				&& candidate->mState == TraversalRequestState::Pending && controlFor(*candidate)
				&& (!selected || candidateId < selected)) selected = candidateId;
		if (selected != requestId) return;
		auto interactionId = requestInteractionForTraversal(controlFor(*request), request->mOwner);
		if (!interactionId) return;
		auto interaction = mInteractionRequests.find(interactionId);
		if (!interaction || interaction->mOperations.empty())
		{
			denyTraversalRequest(requestId, TraversalFailureReason::ControlRejected);
			return;
		}
		resource.mActivePreparation = interactionId;
		resource.mPreparationOperator = requestId;
		resource.mSharedPreparationOperation = interaction->mOperations.front().first;
		request->mPreparationRequested = true;
		request->mPreparationOperation = resource.mSharedPreparationOperation;
	}

	void Building::denyTraversalRequest(TraversalRequestId requestId, TraversalFailureReason reason)
	{
		auto request = mTraversalRequests.find(requestId);
		if (!request || request->mState != TraversalRequestState::Pending)
		{
			return;
		}
		request->mState = TraversalRequestState::Denied;
		request->mFailureReason = reason;
		if (auto resource = mTraversalResources.find(request->mResource); resource)
		{
			if (resource->mExtensible && resource->mExtensionRequestLeases.erase(requestId))
				resource->mExtensible->releaseExtensionLease();
			if (resource->mDoor)
			{
				if (request->mPreparationLease) releaseDoorOpenLease(*resource, request->mPreparationLease);
				request->mPreparationLease = {};
				releaseDoorQueueOwnership(requestId, *resource);
			}
			else if (resource->mLiftCoordinator)
			{
				if (auto lift = mTraversalResources.find(resource->mLiftCoordinator);
					lift && lift->mLiftAdmissionReservation == requestId)
				{
					lift->mLiftAdmissionReservation = {};
					lift->mAdmissionReservations[0] = {};
				}
			}
			else if (resource->mLadder || resource->mStaircase)
			{
				releaseLadderAdmission(requestId, *resource);
				tryGrantLadderAdmissions(*resource);
			}
		}

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalRequestChanged;
		event.phase = mCurrentPhase;
		event.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
		mEvents.push_back(std::move(event));
	}

	bool Building::commitTraversal(Agent& agent, TraversalRequestId requestId, TraversalPermitId permitId,
		shared_ptr<const Vertex> const& destination)
	{
		auto request = mTraversalRequests.find(requestId);
		auto permit = mTraversalPermits.find(permitId);
		auto owner = getAgentId(&agent);
		if (!request || !permit || !destination || request->mOwner != owner || permit->mOwner != owner
			|| permit->mRequest != requestId || request->mPermit != permitId
			|| request->mState != TraversalRequestState::Granted
			|| permit->mState != TraversalPermitState::Active
			|| agent.getGlobalPosition().distanceTo(request->mDestinationEndpoint) > 0.001f)
		{
			return false;
		}

		auto sourceSector = const_cast<Sector*>(agent.getSector());
		auto destinationSector = destination->getSector();
		if (!sourceSector
			|| request->mSourceSector != SectorId{ (uint64_t)sourceSector->getIndex() + 1 }
			|| request->mDestinationSector != SectorId{ (uint64_t)destinationSector->getIndex() + 1 })
		{
			return false;
		}

		auto ladderResource = mTraversalResources.find(request->mResource);
		if (ladderResource && (ladderResource->mLadder || ladderResource->mStaircase)
			&& isLadderAdmission(*request, *ladderResource))
		{
			if (request->mCapacityPosition >= ladderResource->mCapacity
				|| ladderResource->mAdmissionReservations[request->mCapacityPosition] != requestId
				|| ladderResource->mOccupants[request->mCapacityPosition])
			{
				return false;
			}
		}

		// The transfer is deliberately confined to the commit phase. Until this
		// point movement changed only the source-relative position.
		if (sourceSector != destinationSector.get())
		{
			sourceSector->exitAgent(&agent);
			destinationSector->enterAgent(&agent, destination);
		}

		if (auto landing = mTraversalResources.find(request->mResource);
			landing && landing->mLiftCoordinator)
		{
			auto lift = mTraversalResources.find(landing->mLiftCoordinator);
			if (!lift) return false;
			if (request->mSourceSector != lift->mLiftSector
				&& request->mDestinationSector == lift->mLiftSector)
			{
				if (lift->mLiftAdmissionReservation != requestId) return false;
				lift->mLiftAdmissionReservation = {};
				lift->mAdmissionReservations[0] = {};
				lift->mOccupants[0] = owner;
				lift->mLiftPassenger = owner;
				request->mCapacityPosition = ~0u;
				// The standing position is local to the car. Its vertical component is
				// refreshed from the car transform on every resource phase.
				auto local = agent.getLocalPosition();
				local.x = lift->mCapacityPositions[0].x;
				local.y = lift->mLiftPosition - destinationSector->getPosition().y;
				agent.setPosition({ destinationSector.get(), local });
			}
			else if (request->mSourceSector == lift->mLiftSector
				&& request->mDestinationSector != lift->mLiftSector)
			{
				lift->mLiftPassenger = {};
				lift->mOccupants[0] = {};
				lift->mLiftDestinationStop = ~0u;
			}
		}

		if (auto resource = ladderResource; resource && (resource->mLadder || resource->mStaircase))
		{
			if (resource->mLadder && request->mSourceSector != resource->mLadderSector
				&& request->mDestinationSector == resource->mLadderSector
				&& request->mCapacityPosition < resource->mCapacity)
			{
				auto position = request->mCapacityPosition;
				resource->mAdmissionReservations[position] = {};
				resource->mOccupants[position] = owner;
				if (resource->mExtensible)
				{
					if (resource->mExtensionRequestLeases.erase(requestId))
						resource->mExtensible->releaseExtensionLease();
					if (resource->mExtensionOccupantLeases.insert(owner).second)
						resource->mExtensible->acquireExtensionLease();
				}
				request->mCapacityPosition = ~0u;
			}
			else if (resource->mLadder && request->mSourceSector == resource->mLadderSector
				&& request->mDestinationSector != resource->mLadderSector)
			{
				releaseLadderOccupancy(owner, *resource);
				if (resource->mExtensible && resource->mExtensionOccupantLeases.erase(owner))
					resource->mExtensible->releaseExtensionLease();
			}
			else if (request->mCapacityPosition != ~0u)
			{
				// A ladder embedded in one location owns capacity only while its
				// vertical edge is active; there is no separate transit membership.
				releaseLadderAdmission(requestId, *resource);
			}
			tryGrantLadderAdmissions(*resource);
		}

		if (auto resource = mTraversalResources.find(request->mResource);
			resource && resource->mForceBridge
			&& resource->mExtensionRequestLeases.erase(requestId))
			resource->mExtensible->releaseExtensionLease();
		permit->mState = TraversalPermitState::Committed;
		request->mState = TraversalRequestState::Committed;

		SimulationEvent permitEvent;
		permitEvent.sequence = mNextEventSequence++;
		permitEvent.tick = mSimulationTick;
		permitEvent.type = SimulationEventType::TraversalPermitChanged;
		permitEvent.phase = mCurrentPhase;
		permitEvent.traversalPermit = makeTraversalPermitSnapshot(permitId, *permit);
		mEvents.push_back(std::move(permitEvent));

		SimulationEvent requestEvent;
		requestEvent.sequence = mNextEventSequence++;
		requestEvent.tick = mSimulationTick;
		requestEvent.type = SimulationEventType::TraversalRequestChanged;
		requestEvent.phase = mCurrentPhase;
		requestEvent.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
		mEvents.push_back(std::move(requestEvent));
		return true;
	}

	void Building::cancelTraversal(TraversalRequestId requestId, TraversalPermitId permitId)
	{
		if (auto request = mTraversalRequests.find(requestId))
		{
			if (auto resource = mTraversalResources.find(request->mResource);
				resource && resource->mExtensible && resource->mExtensionRequestLeases.erase(requestId))
				resource->mExtensible->releaseExtensionLease();
			if (auto resource = mTraversalResources.find(request->mResource);
				resource && resource->mPreparationOperator == requestId)
			{
				if (resource->mActivePreparation)
				{
					cancelInteraction(resource->mActivePreparation);
				}
				resource->mActivePreparation = {};
				resource->mPreparationOperator = {};
				resource->mSharedPreparationOperation = {};
				resource->mNextPreparationTick = mSimulationTick + 1;
			}
			if (auto resource = mTraversalResources.find(request->mResource); resource)
			{
				if (resource->mDoor) releaseDoorQueueOwnership(requestId, *resource);
				if (resource->mLiftCoordinator)
				{
					if (auto lift = mTraversalResources.find(resource->mLiftCoordinator);
						lift && lift->mLiftAdmissionReservation == requestId)
					{
						lift->mLiftAdmissionReservation = {};
						lift->mAdmissionReservations[0] = {};
					}
				}
				else if (resource->mLadder || resource->mStaircase)
				{
					releaseLadderAdmission(requestId, *resource);
					tryGrantLadderAdmissions(*resource);
				}
			}
			vector<InteractionRequestId> ownedInteractions;
			for (auto const& [interactionId, interaction] : mInteractionRequests.entries())
				if (interaction->mActor == request->mOwner
					&& interaction->mResult == InteractionResult::Pending)
					ownedInteractions.push_back(interactionId);
			for (auto interactionId : ownedInteractions) cancelInteraction(interactionId);
		}

		if (auto permit = mTraversalPermits.find(permitId);
			permit && permit->mState == TraversalPermitState::Active)
		{
			permit->mState = TraversalPermitState::Cancelled;
			SimulationEvent event;
			event.sequence = mNextEventSequence++;
			event.tick = mSimulationTick;
			event.type = SimulationEventType::TraversalPermitChanged;
			event.phase = mCurrentPhase;
			event.traversalPermit = makeTraversalPermitSnapshot(permitId, *permit);
			mEvents.push_back(std::move(event));
		}

		if (auto request = mTraversalRequests.find(requestId);
			request && request->mState != TraversalRequestState::Committed)
		{
			request->mState = TraversalRequestState::Cancelled;
			SimulationEvent event;
			event.sequence = mNextEventSequence++;
			event.tick = mSimulationTick;
			event.type = SimulationEventType::TraversalRequestChanged;
			event.phase = mCurrentPhase;
			event.traversalRequest = makeTraversalRequestSnapshot(requestId, *request);
			mEvents.push_back(std::move(event));
		}
	}

	void Building::releaseTraversal(TraversalRequestId requestId, TraversalPermitId permitId)
	{
		if (auto request = mTraversalRequests.find(requestId))
		{
			if (auto resource = mTraversalResources.find(request->mResource); resource)
			{
				if (resource->mExtensible && resource->mExtensionRequestLeases.erase(requestId))
					resource->mExtensible->releaseExtensionLease();
				if (resource->mDoor)
				{
					if (request->mPreparationLease) releaseDoorOpenLease(*resource, request->mPreparationLease);
					if (request->mCrossingLease) releaseDoorOpenLease(*resource, request->mCrossingLease);
					request->mPreparationLease = {};
					request->mCrossingLease = {};
					releaseDoorQueueOwnership(requestId, *resource);
				}
				else if (resource->mLadder || resource->mStaircase)
				{
					releaseLadderAdmission(requestId, *resource);
					tryGrantLadderAdmissions(*resource);
				}
			}
			if (request->mState == TraversalRequestState::Cancelled && request->mPreparationOperation)
			{
				if (auto resource = mTraversalResources.find(request->mResource);
					resource && resource->mActivePreparation)
				{
					if (auto interaction = mInteractionRequests.find(resource->mActivePreparation))
					{
						for (auto const& [operationId, requirement] : interaction->mOperations)
						{
							(void)requirement;
							cancelDeviceOperation(operationId, request->mOwner);
						}
					}
				}
				else
				{
					cancelDeviceOperation(request->mPreparationOperation, request->mOwner);
				}
			}
		}

		if (auto permit = mTraversalPermits.find(permitId))
		{
			auto snapshot = makeTraversalPermitSnapshot(permitId, *permit);
			mTraversalPermits.remove(permitId);
			SimulationEvent event;
			event.sequence = mNextEventSequence++;
			event.tick = mSimulationTick;
			event.type = SimulationEventType::TraversalPermitRemoved;
			event.phase = mCurrentPhase;
			event.traversalPermit = std::move(snapshot);
			mEvents.push_back(std::move(event));
		}

		if (auto request = mTraversalRequests.find(requestId))
		{
			auto snapshot = makeTraversalRequestSnapshot(requestId, *request);
			mTraversalRequests.remove(requestId);
			SimulationEvent event;
			event.sequence = mNextEventSequence++;
			event.tick = mSimulationTick;
			event.type = SimulationEventType::TraversalRequestRemoved;
			event.phase = mCurrentPhase;
			event.traversalRequest = std::move(snapshot);
			mEvents.push_back(std::move(event));
		}
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
		if (found.entity->getState() == Agent::State::WaitingForTraversal
			&& !found.entity->getTraversalPermitId())
		{
			// Removing a waiter is cancellation, not an exceptional state. Its
			// queue ticket and physical reservation are released by clearPath().
			found.entity->clearPath();
		}
		if (found.entity->getState() != Agent::State::Idle)
		{
			return { false, format("Agent handle {} is active and cannot be removed safely", id.value) };
		}

		vector<InteractionRequestId> ownedRequests;
		for (auto const& [requestId, request] : mInteractionRequests.entries())
		{
			if (request->getActor() == id && request->getResult() == InteractionResult::Pending)
			{
				ownedRequests.push_back(requestId);
			}
		}
		for (auto requestId : ownedRequests)
		{
			cancelInteraction(requestId);
		}

		vector<DeviceOperationId> ownedOperations;
		for (auto const& [operationId, operation] : mDeviceOperations.entries())
		{
			if (operation->getRequesters().contains(id))
			{
				ownedOperations.push_back(operationId);
			}
		}
		for (auto operationId : ownedOperations)
		{
			cancelDeviceOperation(operationId, id);
			if (auto operation = mDeviceOperations.find(operationId); operation && operation->getRequesters().empty())
			{
				(void)removeDeviceOperation(operationId);
			}
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

	InteractionPointId Building::createInteractionPoint(string const& name, SectorId sector,
		Vector2 position, float reach, float durationSeconds, vector<InteractionBinding> bindings)
	{
		if (!sector || sector.value > mSectors.size())
		{
			throw invalid_argument("An interaction point requires a valid sector");
		}
		if (reach < 0.0f || durationSeconds < 0.0f || bindings.empty())
		{
			throw invalid_argument("An interaction point requires non-negative timing and at least one binding");
		}
		for (auto const& binding : bindings)
		{
			bool validTarget = false;
			if (binding.command.type == DeviceCommandType::SetSectorLights)
				validTarget = binding.command.target && binding.command.target.value <= mSectors.size();
			else if (auto resource = mTraversalResources.find(binding.command.traversalResource))
				validTarget = binding.command.type == DeviceCommandType::OpenDoor ? resource->mDoor != nullptr
					: binding.command.type == DeviceCommandType::SetExtendedState ? resource->mExtensible != nullptr
					: (binding.command.type == DeviceCommandType::CallLift
						|| binding.command.type == DeviceCommandType::SelectLiftDestination)
						&& resource->mLift && binding.command.stopIndex < resource->mLiftStops.size();
			if (!validTarget)
			{
				throw invalid_argument("An interaction binding requires a valid command target");
			}
		}
		auto durationTicks = max<uint64_t>(1, (uint64_t)ceil(durationSeconds / getFixedTimestep()));
		auto id = mInteractionPoints.add(unique_ptr<InteractionPoint>(new InteractionPoint(
			name, sector, position, reach, durationTicks, std::move(bindings))));
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
		vector<InteractionRequestId> requests;
		for (auto const& [requestId, request] : mInteractionRequests.entries())
		{
			if (request->getPoint() == id && request->getResult() == InteractionResult::Pending)
			{
				requests.push_back(requestId);
			}
		}
		for (auto requestId : requests)
		{
			cancelInteraction(requestId);
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

	DeviceOperationId Building::findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester)
	{
		for (auto const& [id, operation] : mDeviceOperations.entries())
		{
			if (operation->mHasCommand && operation->mCommand == command
				&& (operation->mState == DeviceOperationState::Pending || operation->mState == DeviceOperationState::Running))
			{
				operation->mRequesters.insert(requester);
				return id;
			}
		}
		auto name = command.type == DeviceCommandType::SetSectorLights
			? string("Set sector lights ") + (command.desiredState ? "on" : "off")
			: command.type == DeviceCommandType::OpenDoor ? "Open door"
			: command.type == DeviceCommandType::SetExtendedState
				? string(command.desiredState ? "Extend resource" : "Retract resource")
			: command.type == DeviceCommandType::CallLift ? "Call lift"
			: command.type == DeviceCommandType::SelectLiftDestination ? "Select lift destination"
				: "Device command";
		auto id = mDeviceOperations.add(unique_ptr<DeviceOperation>(new DeviceOperation(name, requester, command)));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::DeviceOperationAdded;
		event.phase = mCurrentPhase;
		event.deviceOperation = makeDeviceOperationSnapshot(id, *mDeviceOperations.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	InteractionRequestId Building::requestInteraction(InteractionPointId pointId, AgentId actorId)
	{
		auto point = mInteractionPoints.find(pointId);
		auto actor = mAgents.find(actorId);
		if (!point || !actor || !point->mSector
			|| (actor->getState() != Agent::State::Idle
				&& actor->getState() != Agent::State::WaitingForTraversal)
			|| actor->getSector() != mSectors[(size_t)point->mSector.value - 1].get())
		{
			return {};
		}
		for (auto const& [id, request] : mInteractionRequests.entries())
		{
			if (request->mActor == actorId && request->mResult == InteractionResult::Pending)
			{
				return request->mPoint == pointId ? id : InteractionRequestId{};
			}
		}
		auto id = mInteractionRequests.add(unique_ptr<InteractionRequest>(new InteractionRequest(pointId, actorId)));
		auto request = mInteractionRequests.find(id);
		for (auto const& binding : point->mBindings)
		{
			request->mOperations.emplace_back(findOrCreateDeviceOperation(binding.command, actorId), binding.requirement);
		}
		point->mQueue.push_back(id);

		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::InteractionRequestAdded;
		event.phase = mCurrentPhase;
		event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
		mEvents.push_back(std::move(event));
		return id;
	}

	InteractionRequestId Building::requestInteractionForTraversal(InteractionPointId point, AgentId actor)
	{
		return requestInteraction(point, actor);
	}

	EntityLookup<InteractionRequest const> Building::lookupInteractionRequest(InteractionRequestId id) const
	{
		auto entity = mInteractionRequests.find(id);
		return entity ? EntityLookup<InteractionRequest const>{ entity, {} }
			: EntityLookup<InteractionRequest const>{ nullptr, format("InteractionRequest handle {} is invalid", id.value) };
	}

	void Building::detachInteractionRequester(InteractionRequest& request)
	{
		for (auto const& [operationId, requirement] : request.mOperations)
		{
			(void)requirement;
			if (auto operation = mDeviceOperations.find(operationId))
			{
				operation->mRequesters.erase(request.mActor);
				if (operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
					|| operation->mState == DeviceOperationState::Running))
				{
					operation->mState = DeviceOperationState::Cancelled;
				}
			}
		}
	}

	bool Building::cancelInteraction(InteractionRequestId id)
	{
		auto request = mInteractionRequests.find(id);
		if (!request || request->mResult != InteractionResult::Pending)
		{
			return false;
		}
		request->mResult = InteractionResult::Cancelled;
		detachInteractionRequester(*request);
		if (auto point = mInteractionPoints.find(request->mPoint))
		{
			if (point->mActiveRequest == id)
			{
				point->mActiveRequest = {};
				point->mInteractionTicksRemaining = 0;
			}
			point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), id), point->mQueue.end());
		}
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::InteractionRequestChanged;
		event.phase = mCurrentPhase;
		event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
		mEvents.push_back(std::move(event));
		return true;
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

	bool Building::cancelDeviceOperation(DeviceOperationId id, AgentId requester)
	{
		auto operation = mDeviceOperations.find(id);
		if (!operation || !operation->mRequesters.erase(requester))
		{
			return false;
		}
		vector<InteractionRequestId> affectedRequests;
		for (auto const& [requestId, request] : mInteractionRequests.entries())
		{
			if (request->mActor != requester || request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			if (find_if(request->mOperations.begin(), request->mOperations.end(), [id](auto const& binding)
				{ return binding.first == id; }) != request->mOperations.end())
			{
				affectedRequests.push_back(requestId);
			}
		}
		for (auto requestId : affectedRequests)
		{
			cancelInteraction(requestId);
		}
		if (operation->mRequesters.empty() && (operation->mState == DeviceOperationState::Pending
			|| operation->mState == DeviceOperationState::Running))
		{
			operation->mState = DeviceOperationState::Cancelled;
		}
		return true;
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

	TraversalResourceId Building::createDoorTraversalResource(string const& name,
		shared_ptr<Door> door, DoorActivationMode mode, float holdOpenSeconds)
	{
		if (!door || holdOpenSeconds < 0.0f)
		{
			throw invalid_argument("A door traversal resource requires a Door and non-negative hold time");
		}
		auto holdTicks = (uint64_t)ceil(holdOpenSeconds / getFixedTimestep());
		auto laneCount = max(1u, door->getCellsWide());
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(
			new TraversalResource(name, std::move(door), mode, holdTicks)));
		mTraversalResources.find(id)->mCrossingOwners.resize(laneCount);
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId Building::createLadderTraversalResource(string const& name,
		shared_ptr<Ladder> ladder, SectorId ladderSector, float agentSpacing,
		uint32_t directionalBatchLimit)
	{
		if (!ladder || !ladderSector || ladderSector.value > mSectors.size()
			|| agentSpacing <= 0.0f || directionalBatchLimit == 0)
		{
			throw invalid_argument("A ladder traversal resource requires a Ladder, sector, and positive spacing");
		}
		auto usableLength = ladder->getUsableLength();
		auto capacity = max(1u, (uint32_t)floor(usableLength / agentSpacing));
		vector<Vector2> positions;
		positions.reserve(capacity);
		auto origin = ladder->getPosition();
		auto x = origin.x + ladder->getSize().x * 0.5f;
		for (uint32_t i = 0; i < capacity; ++i)
		{
			positions.push_back({ x, origin.y + (agentSpacing >= usableLength
				? usableLength * 0.5f : agentSpacing * ((float)i + 0.5f)) });
		}

		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, ladder, ladder->isExtensible() ? static_pointer_cast<ExtensibleObject>(ladder) : nullptr,
			ladderSector, agentSpacing, capacity, directionalBatchLimit, std::move(positions))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId Building::createLiftTraversalResource(string const& name,
		shared_ptr<Lift> lift, SectorId liftSector, vector<LiftStop> stops)
	{
		if (!lift || !liftSector || liftSector.value > mSectors.size() || stops.size() < 2)
		{
			throw invalid_argument("A lift traversal resource requires a Lift, transit sector, and at least two stops");
		}
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, std::move(lift), liftSector, std::move(stops))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId Building::createStaircaseTraversalResource(string const& name,
		shared_ptr<Staircase> staircase, SectorId staircaseSector, uint32_t capacity,
		uint32_t directionalBatchLimit)
	{
		if (!staircase || !staircaseSector || staircaseSector.value > mSectors.size()
			|| capacity == 0 || directionalBatchLimit == 0)
		{
			throw invalid_argument("A narrow staircase resource requires a Staircase, sector, capacity, and batch limit");
		}
		vector<Vector2> positions;
		positions.reserve(capacity);
		auto origin = staircase->getPosition();
		for (uint32_t i = 0; i < capacity; ++i)
		{
			positions.push_back({ origin.x + ((float)i + 0.5f) * staircase->getSize().x / capacity,
				origin.y + staircase->getSize().y * 0.5f });
		}
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, staircase, staircaseSector, capacity, directionalBatchLimit, std::move(positions))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId Building::createForceBridgeTraversalResource(string const& name,
		shared_ptr<ForceBridge> forceBridge)
	{
		if (!forceBridge) throw invalid_argument("A force bridge traversal resource requires a ForceBridge");
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, forceBridge, forceBridge->isExtensible()
				? static_pointer_cast<ExtensibleObject>(forceBridge) : nullptr)));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	bool Building::configureDoorQueueLane(TraversalResourceId resourceId, SectorId sectorId,
		Vector2 origin, Vector2 direction, float extent)
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || !sectorId || sectorId.value > mSectors.size()
			|| extent < 0.0f || direction.length() < 0.001f)
		{
			throw invalid_argument("A door queue lane requires a door, source sector, direction, and non-negative extent");
		}
		direction.normalise();
		auto sector = mSectors[(size_t)sectorId.value - 1];
		auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
		auto positionFits = [&](Vector2 const& position)
		{
			if (position.x - halfWidth < sector->getCellX0() - 0.001f
				|| position.x + halfWidth > sector->getCellX1() + 1.0f + 0.001f
				|| position.y < sector->getCellY0() - 0.001f
				|| position.y + CORE_AGENT_MAX_HEIGHT > sector->getCellY1() + 1.0f + 0.001f)
			{
				return false;
			}
			auto const cellX = min(sector->getCellX1(), (uint32_t)floor(position.x));
			auto const cellY = min(sector->getCellY1(), (uint32_t)floor(position.y));
			return mLayers[sector->getLayerIndex()]->getCellDefinition(cellX, cellY).isTraversableOnFoot();
		};
		if (!positionFits(origin) || !positionFits(origin + direction * extent))
		{
			throw invalid_argument("Door queue lane does not fit inside its source sector");
		}

		DoorQueueLane* lane = nullptr;
		for (auto& candidate : resource->mQueueLanes)
		{
			if (candidate.sector == sectorId)
			{
				lane = &candidate;
				break;
			}
			if (!candidate.sector && !lane)
			{
				lane = &candidate;
			}
		}
		if (!lane)
		{
			throw invalid_argument("A door traversal resource supports exactly two approach lanes");
		}
		if (!lane->queue.empty())
		{
			throw invalid_argument("An active door queue lane cannot be reconfigured");
		}
		lane->sector = sectorId;
		lane->origin = origin;
		lane->direction = direction;
		lane->extent = extent;
		lane->positions.clear();
		lane->positionOwners.clear();
		auto const spacing = (float)CORE_DOOR_QUEUE_STOP_WIDTH;
		for (float distance = 0.0f; distance <= extent + 0.001f; distance += spacing)
		{
			auto position = origin + direction * distance;
			if (!positionFits(position))
			{
				throw invalid_argument("A generated door queue position is outside its source sector");
			}
			lane->positions.push_back(position);
			lane->positionOwners.push_back({});
		}
		return true;
	}

	bool Building::configureDoorCrossingLanes(TraversalResourceId resourceId, uint32_t laneCount)
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || laneCount == 0)
		{
			throw invalid_argument("A door crossing requires at least one lane");
		}
		if (laneCount > resource->mDoor->getCellsWide())
		{
			throw invalid_argument("Door crossing lane count exceeds usable threshold width");
		}
		if (any_of(resource->mCrossingOwners.begin(), resource->mCrossingOwners.end(),
			[](TraversalRequestId owner) { return (bool)owner; }))
		{
			return false;
		}
		resource->mCrossingOwners.assign(laneCount, {});
		return true;
	}

	DoorOpenLeaseId Building::acquireDoorOpenLease(TraversalResource& resource,
		DoorOpenLeaseKind kind, TraversalRequestId request)
	{
		auto id = DoorOpenLeaseId{ mNextDoorOpenLeaseValue++ };
		resource.mOpenLeases.emplace(id, DoorOpenLease{ kind, request });
		resource.mDoor->acquireOpenLease();
		// Safety and locally activated preparation have priority over a close.
		// Remote preparation still has to reach its configured physical control.
		if (resource.mDoor->isClosing()
			&& (kind != DoorOpenLeaseKind::Preparation
				|| resource.mDoorActivationMode != DoorActivationMode::RemoteControlled))
		{
			resource.mDoor->handleAction(ControllableActionType::Open);
		}
		return id;
	}

	bool Building::releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease)
	{
		if (!lease || resource.mOpenLeases.erase(lease) == 0) return false;
		resource.mDoor->releaseOpenLease();
		return true;
	}

	DoorOpenLeaseId Building::acquireDoorOpenLease(TraversalResourceId resourceId, DoorOpenLeaseKind kind)
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || !resource->mEnabled) return {};
		return acquireDoorOpenLease(*resource, kind);
	}

	bool Building::releaseDoorOpenLease(TraversalResourceId resourceId, DoorOpenLeaseId lease)
	{
		auto resource = mTraversalResources.find(resourceId);
		return resource && resource->mDoor && releaseDoorOpenLease(*resource, lease);
	}

	bool Building::setDoorSensorObservation(TraversalResourceId resourceId, DoorSensorId sensor,
		DoorSensorObservation observation)
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mDoor || !sensor) return false;
		if (observation == DoorSensorObservation::Clear) resource->mSensorObservations.erase(sensor);
		else resource->mSensorObservations[sensor] = observation;
		return true;
	}

	bool Building::setTraversalResourceEnabled(TraversalResourceId resourceId, bool enabled)
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return false;
		resource->mEnabled = enabled;
		return true;
	}

	bool Building::addTraversalControl(TraversalResourceId resourceId, InteractionPointId controlId)
	{
		auto resource = mTraversalResources.find(resourceId);
		auto control = mInteractionPoints.find(controlId);
		if (!resource || (!resource->mDoor && !resource->mExtensible) || !control)
		{
			return false;
		}
		if (find(resource->mControls.begin(), resource->mControls.end(), controlId) == resource->mControls.end())
		{
			resource->mControls.push_back(controlId);
			if (resource->mExtensible) resource->mExtensible->addExtensionControlSector(control->mSector);
			sort(resource->mControls.begin(), resource->mControls.end());
		}
		return true;
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

	EntityLookup<TraversalRequest const> Building::lookupTraversalRequest(TraversalRequestId id) const
	{
		auto entity = mTraversalRequests.find(id);
		return entity ? EntityLookup<TraversalRequest const>{ entity, {} }
			: EntityLookup<TraversalRequest const>{ nullptr, format("TraversalRequest handle {} is invalid or has been released", id.value) };
	}

	EntityLookup<TraversalPermit const> Building::lookupTraversalPermit(TraversalPermitId id) const
	{
		auto entity = mTraversalPermits.find(id);
		return entity ? EntityLookup<TraversalPermit const>{ entity, {} }
			: EntityLookup<TraversalPermit const>{ nullptr, format("TraversalPermit handle {} is invalid or has been released", id.value) };
	}

	TraversalWaitingPolicy const& Building::getTraversalWaitingPolicy() const
	{
		return mTraversalWaitingPolicy;
	}

	void Building::setTraversalWaitingPolicy(TraversalWaitingPolicy policy)
	{
		if (policy.localGoalTimeoutTicks == 0 || policy.permitProgressTimeoutTicks == 0
			|| policy.replanIntervalTicks == 0 || policy.queueDelayPerAgentSeconds < 0.0f
			|| policy.replanEtaMarginSeconds < 0.0f)
		{
			throw invalid_argument("Traversal waiting policy durations must be positive and costs non-negative");
		}
		mTraversalWaitingPolicy = policy;
	}

	float Building::estimateTraversalDelay(TraversalResourceId resourceId, SectorId sourceSector) const
	{
		(void)sourceSector; // Future access-zone resources may scope demand by origin.
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return 0.0f;
		size_t ahead = 0;
		for (auto const& lane : resource->mQueueLanes)
		{
			ahead += lane.queue.size();
		}
		auto lanes = max<size_t>(1, resource->mCrossingOwners.size());
		return (float)((ahead + lanes - 1) / lanes)
			* mTraversalWaitingPolicy.queueDelayPerAgentSeconds;
	}

	SimulationSnapshot Building::getSimulationSnapshot() const
	{
		SimulationSnapshot result;
		result.tick = mSimulationTick;
		result.agents.reserve(mAgents.entries().size());
		result.interactionPoints.reserve(mInteractionPoints.entries().size());
		result.interactionRequests.reserve(mInteractionRequests.entries().size());
		result.deviceOperations.reserve(mDeviceOperations.entries().size());
		result.traversalResources.reserve(mTraversalResources.entries().size());
		result.traversalRequests.reserve(mTraversalRequests.entries().size());
		result.traversalPermits.reserve(mTraversalPermits.entries().size());

		for (auto const& [id, agent] : mAgents.entries())
		{
			(void)id;
			result.agents.push_back(makeAgentSnapshot(agent.get()));
		}
		for (auto const& [id, point] : mInteractionPoints.entries())
		{
			result.interactionPoints.push_back(makeInteractionPointSnapshot(id, *point));
		}
		for (auto const& [id, request] : mInteractionRequests.entries())
		{
			result.interactionRequests.push_back(makeInteractionRequestSnapshot(id, *request));
		}
		for (auto const& [id, operation] : mDeviceOperations.entries())
		{
			result.deviceOperations.push_back(makeDeviceOperationSnapshot(id, *operation));
		}
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			result.traversalResources.push_back(makeTraversalResourceSnapshot(id, *resource));
		}
		for (auto const& [id, request] : mTraversalRequests.entries())
		{
			result.traversalRequests.push_back(makeTraversalRequestSnapshot(id, *request));
		}
		for (auto const& [id, permit] : mTraversalPermits.entries())
		{
			result.traversalPermits.push_back(makeTraversalPermitSnapshot(id, *permit));
		}

		return result;
	}

	void Building::advanceLiftResources()
	{
		for (auto const& [resourceId, resourcePtr] : mTraversalResources.entries())
		{
			(void)resourceId;
			auto& resource = *resourcePtr;
			if (!resource.mLift || resource.mLiftStops.empty()
				|| resource.mLiftTargetStop >= resource.mLiftStops.size()) continue;
			auto target = resource.mLiftStops[resource.mLiftTargetStop].globalPosition;
			if (abs(resource.mLiftPosition - target) < 0.001f)
			{
				resource.mLiftPosition = target;
				resource.mLiftCurrentStop = resource.mLiftTargetStop;
				resource.mLiftMoving = false;
			}
			else
			{
				bool interlocked = false;
				for (auto const& stop : resource.mLiftStops)
				{
					auto landing = mTraversalResources.find(stop.landingResource);
					if (!landing || !landing->mDoor) continue;
					if (!landing->mOpenLeases.empty()) interlocked = true;
					if (!landing->mDoor->isClosed())
					{
						interlocked = true;
						if (landing->mOpenLeases.empty() && !landing->mDoor->isClosing())
							landing->mDoor->handleAction(ControllableActionType::Close);
					}
				}
				resource.mLiftCarDoorOpen = interlocked;
				if (!interlocked)
				{
					resource.mLiftMoving = true;
					auto amount = CORE_LIFT_SPEED * getFixedTimestep();
					if (target > resource.mLiftPosition)
						resource.mLiftPosition = min(target, resource.mLiftPosition + amount);
					else resource.mLiftPosition = max(target, resource.mLiftPosition - amount);
					if (resource.mLiftPosition == target)
					{
						resource.mLiftCurrentStop = resource.mLiftTargetStop;
						resource.mLiftMoving = false;
					}
				}
			}
			resource.mLift->setCoordinatedPosition(resource.mLiftPosition);
			if (auto passenger = mAgents.find(resource.mLiftPassenger))
			{
				auto transit = mSectors[(size_t)resource.mLiftSector.value - 1].get();
				auto local = passenger->getLocalPosition();
				local.y = resource.mLiftPosition - transit->getPosition().y;
				passenger->setPosition({ transit, local });
			}
		}
	}

	void Building::advanceDoorResources()
	{
		for (auto const& [resourceId, resourcePtr] : mTraversalResources.entries())
		{
			auto& resource = *resourcePtr;
			if (!resource.mDoor) continue;
			bool presence = false;
			bool obstruction = false;
			for (auto const& [sensor, observation] : resource.mSensorObservations)
			{
				(void)sensor;
				presence = presence || observation == DoorSensorObservation::Presence;
				obstruction = obstruction || observation == DoorSensorObservation::Obstruction;
			}
			resource.mDoor->setObstructed(obstruction);
			if ((obstruction || (presence && resource.mDoorActivationMode == DoorActivationMode::Automatic))
				&& resource.mEnabled && !resource.mDoor->isOpen() && !resource.mDoor->isOpening())
			{
				resource.mDoor->handleAction(ControllableActionType::Open);
			}

			bool activeCrossing = any_of(resource.mCrossingOwners.begin(), resource.mCrossingOwners.end(),
				[](TraversalRequestId owner) { return (bool)owner; });
			if (!resource.mEnabled && !activeCrossing)
			{
				vector<TraversalRequestId> pending;
				for (auto const& [requestId, request] : mTraversalRequests.entries())
				{
					if (request->mResource == resourceId && request->mState == TraversalRequestState::Pending)
						pending.push_back(requestId);
				}
				for (auto requestId : pending) denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
			}
		}
	}

	void Building::advanceDeviceOperations()
	{
		for (auto const& [id, operation] : mDeviceOperations.entries())
		{
			(void)id;
			if (!operation->mHasCommand || !operation->mActivated)
			{
				continue;
			}
			if (operation->mState == DeviceOperationState::Pending)
			{
				operation->mState = DeviceOperationState::Running;
				if (operation->mCommand.type == DeviceCommandType::OpenDoor)
				{
					auto resource = mTraversalResources.find(operation->mCommand.traversalResource);
					auto action = operation->mCommand.desiredState
						? ControllableActionType::Open : ControllableActionType::Close;
					if (!resource || !resource->mDoor || !resource->mEnabled
						|| resource->mDoor->handleAction(action) == ~0u)
					{
						operation->mState = DeviceOperationState::Rejected;
					}
				}
				else if (operation->mCommand.type == DeviceCommandType::SetExtendedState)
				{
					auto resource = mTraversalResources.find(operation->mCommand.traversalResource);
					if (!resource || !resource->mExtensible || !resource->mExtensible->isExtensible())
					{
						operation->mState = DeviceOperationState::Rejected;
					}
					else if (operation->mCommand.desiredState)
					{
						resource->mRetractionPending = false;
						resource->mEnabled = true;
						if (resource->mExtensible->handleAction(ControllableActionType::Extend) == ~0u)
							operation->mState = DeviceOperationState::Rejected;
					}
					else
					{
						// Accepting a safe retract closes admission immediately. Physical
						// retraction starts only after every independently-owned lease drains.
						resource->mRetractionPending = true;
						resource->mEnabled = false;
					}
				}
				continue;
			}
			if (operation->mState != DeviceOperationState::Running)
			{
				continue;
			}
			if (operation->mCommand.type == DeviceCommandType::SetSectorLights
				&& operation->mCommand.target
				&& operation->mCommand.target.value <= mSectors.size())
			{
				auto sector = mSectors[(size_t)operation->mCommand.target.value - 1];
				bool succeeded = operation->mCommand.desiredState ? sector->lightsOn() : sector->lightsOff();
				operation->mState = succeeded ? DeviceOperationState::Succeeded : DeviceOperationState::Failed;
			}
			else if (operation->mCommand.type == DeviceCommandType::SetExtendedState)
			{
				auto resource = mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || !resource->mExtensible)
				{
					operation->mState = DeviceOperationState::Failed;
				}
				else if (operation->mCommand.desiredState && resource->mExtensible->isExtended())
				{
					operation->mState = DeviceOperationState::Succeeded;
				}
				else if (!operation->mCommand.desiredState)
				{
					if (resource->mExtensionRequestLeases.empty()
						&& resource->mExtensionOccupantLeases.empty())
					{
						if (!resource->mExtensible->isRetracted() && !resource->mExtensible->isRetracting())
							resource->mExtensible->handleAction(ControllableActionType::Retract);
						if (resource->mExtensible->isRetracted())
						{
							resource->mRetractionPending = false;
							operation->mState = DeviceOperationState::Succeeded;
						}
					}
				}
			}
			else if (operation->mCommand.type == DeviceCommandType::CallLift
				|| operation->mCommand.type == DeviceCommandType::SelectLiftDestination)
			{
				auto resource = mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || !resource->mLift || !resource->mEnabled
					|| operation->mCommand.stopIndex >= resource->mLiftStops.size())
				{
					operation->mState = DeviceOperationState::Rejected;
				}
				else
				{
					resource->mLiftTargetStop = operation->mCommand.stopIndex;
					if (operation->mCommand.type == DeviceCommandType::SelectLiftDestination)
						resource->mLiftDestinationStop = operation->mCommand.stopIndex;
					operation->mState = DeviceOperationState::Succeeded;
				}
			}
			else if (operation->mCommand.type == DeviceCommandType::OpenDoor)
			{
				auto resource = mTraversalResources.find(operation->mCommand.traversalResource);
				if (!resource || !resource->mDoor)
				{
					operation->mState = DeviceOperationState::Failed;
				}
				else if (!resource->mEnabled)
				{
					operation->mState = DeviceOperationState::Rejected;
				}
				else if ((operation->mCommand.desiredState && resource->mDoor->isOpen())
					|| (!operation->mCommand.desiredState && resource->mDoor->isClosed()))
				{
					operation->mState = DeviceOperationState::Succeeded;
				}
			}
			else
			{
				operation->mState = DeviceOperationState::Failed;
			}
		}
	}

	void Building::allocateInteractions()
	{
		for (auto const& [pointId, point] : mInteractionPoints.entries())
		{
			(void)pointId;
			if (point->mActiveRequest)
			{
				auto active = mInteractionRequests.find(point->mActiveRequest);
				if (active && active->mResult == InteractionResult::Pending)
				{
					continue;
				}
				point->mActiveRequest = {};
				point->mInteractionTicksRemaining = 0;
			}
			while (!point->mQueue.empty())
			{
				auto requestId = point->mQueue.front();
				auto request = mInteractionRequests.find(requestId);
				if (!request || request->mResult != InteractionResult::Pending)
				{
					point->mQueue.erase(point->mQueue.begin());
					continue;
				}
				bool reusedActiveWork = false;
				for (auto const& [operationId, requirement] : request->mOperations)
				{
					(void)requirement;
					if (auto operation = mDeviceOperations.find(operationId); operation && operation->mActivated)
					{
						reusedActiveWork = true;
						break;
					}
				}
				if (reusedActiveWork)
				{
					point->mQueue.erase(point->mQueue.begin());
					continue;
				}
				point->mActiveRequest = requestId;
				point->mInteractionTicksRemaining = point->mDurationTicks;
				break;
			}
		}
	}

	void Building::moveInteractions(float frameTime)
	{
		for (auto const& [pointId, point] : mInteractionPoints.entries())
		{
			(void)pointId;
			auto request = mInteractionRequests.find(point->mActiveRequest);
			if (!request || request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			auto actor = mAgents.find(request->mActor);
			if (!actor || actor->getSector() != mSectors[(size_t)point->mSector.value - 1].get())
			{
				cancelInteraction(point->mActiveRequest);
				continue;
			}
			if (actor->getGlobalPosition().distanceTo(point->mPosition) > point->mReach)
			{
				actor->moveToPosition(point->mPosition, frameTime);
				continue;
			}
			if (point->mInteractionTicksRemaining > 0)
			{
				--point->mInteractionTicksRemaining;
			}
			if (point->mInteractionTicksRemaining == 0)
			{
				for (auto const& [operationId, requirement] : request->mOperations)
				{
					(void)requirement;
					if (auto operation = mDeviceOperations.find(operationId);
						operation && operation->mState == DeviceOperationState::Pending)
					{
						operation->mActivated = true;
					}
				}
				point->mQueue.erase(remove(point->mQueue.begin(), point->mQueue.end(), point->mActiveRequest), point->mQueue.end());
				point->mActiveRequest = {};
			}
		}
	}

	void Building::updateInteractionResults()
	{
		for (auto const& [id, request] : mInteractionRequests.entries())
		{
			if (request->mResult != InteractionResult::Pending)
			{
				continue;
			}
			bool waiting = false;
			bool requiredFailure = false;
			bool requiredRejection = false;
			bool bestEffortFailure = false;
			for (auto const& [operationId, requirement] : request->mOperations)
			{
				auto operation = mDeviceOperations.find(operationId);
				if (operation && operation->mState == DeviceOperationState::Rejected)
				{
					if (requirement == InteractionBindingRequirement::Required)
					{
						requiredRejection = true;
					}
					else
					{
						bestEffortFailure = true;
					}
				}
				else if (!operation || operation->mState == DeviceOperationState::Failed
					|| operation->mState == DeviceOperationState::Cancelled)
				{
					(requirement == InteractionBindingRequirement::Required ? requiredFailure : bestEffortFailure) = true;
				}
				else if (operation->mState == DeviceOperationState::Pending || operation->mState == DeviceOperationState::Running)
				{
					waiting = true;
				}
			}
			if (requiredRejection)
			{
				request->mResult = InteractionResult::Rejected;
			}
			else if (requiredFailure)
			{
				request->mResult = InteractionResult::Failed;
			}
			else if (!waiting)
			{
				request->mResult = bestEffortFailure ? InteractionResult::SucceededWithBestEffortFailure : InteractionResult::Succeeded;
			}
			if (request->mResult != InteractionResult::Pending)
			{
				SimulationEvent event;
				event.sequence = mNextEventSequence++;
				event.tick = mSimulationTick;
				event.type = SimulationEventType::InteractionRequestChanged;
				event.phase = mCurrentPhase;
				event.interactionRequest = makeInteractionRequestSnapshot(id, *request);
				mEvents.push_back(std::move(event));
			}
		}
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
			advanceLiftResources();
			advanceDoorResources();
			advanceDeviceOperations();
			break;

		case SimulationPhase::IntentCollection:
			for (auto const& [id, agent] : mAgents.entries())
			{
				(void)id;
				agent->collectTraversalIntent();
			}
			break;

		case SimulationPhase::Allocation:
			for (auto const& [id, agent] : mAgents.entries())
			{
				(void)id;
				agent->allocateTraversal();
			}
			allocateInteractions();
			break;

		case SimulationPhase::Movement:
			for (auto const& [id, agent] : mAgents.entries())
			{
				(void)id;
				agent->update(timestep);
			}
			moveInteractions(timestep);

			for (auto const& vertexController : mVertexControllers)
			{
				vertexController->update(timestep);
			}
			break;

		case SimulationPhase::Commit:
			for (auto const& [id, agent] : mAgents.entries())
			{
				(void)id;
				agent->commitTraversal();
			}
			break;

		case SimulationPhase::CleanupAndEventPublication:
			updateTraversalProgressAndTimeouts();
			for (auto const& [id, agent] : mAgents.entries())
			{
				(void)id;
				agent->cleanupTraversal();
			}
			updateInteractionResults();
			break;

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
				|| current.pathNodeCount != previous.pathNodeCount
				|| current.hasLocomotionTask != previous.hasLocomotionTask
				|| current.traversalRequest != previous.traversalRequest
				|| current.traversalPermit != previous.traversalPermit
				|| current.interactionRequest != previous.interactionRequest;

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