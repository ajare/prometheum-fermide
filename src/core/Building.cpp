#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/Defines.h"
#include "core/Building.h"
#include "core/Background.h"
#include "core/Location.h"
#include "core/SectorType.h"
#include "core/SectorObjectType.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/ButtonSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/PlatformLift.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	// The shuttle door-offset helper - expanding a carriage's door mask into the cells
	// its doors occupy - lives in SimulationCoordinator with the shuttle door
	// assignment family that consumes that indexing (ADR 0004).

	Building::CreateDoorOptions Building::ManualDoor1Options{ 1, { false, false }, DoorActivationMode::Manual };
	Building::CreateDoorOptions Building::RemoteControlledDoor1Options{ 1, { true, true }, DoorActivationMode::RemoteControlled };
	Building::CreateDoorOptions Building::UnavailableDoor1Options{ 1, { false, false }, DoorActivationMode::Unavailable };
	Building::CreateDoorOptions Building::ManualDoor2Options{ 2, { false, false }, DoorActivationMode::Manual };
	Building::CreateDoorOptions Building::RemoteControlledDoor2Options{ 2, { true, true }, DoorActivationMode::RemoteControlled };
	Building::CreateDoorOptions Building::UnavailableDoor2Options{ 2, { false, false }, DoorActivationMode::Unavailable };

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
		, mLayers(2)
		, mLayerNames{ defaultLayerName(0), defaultLayerName(1) }
		, mSimulationCoordinator(*this)
	{
		for (uint32_t i = 0; i < mLayers.size(); ++i)
		{
			mLayers[i] = make_shared<Layer>(this, cellsWide, decksHigh, i);
		}

		mGraph = make_shared<Graph>(this);
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

	uint32_t Building::getLayerCount() const
	{
		return static_cast<uint32_t>(mLayers.size());
	}

	string const& Building::getLayerName(uint32_t layerIndex) const
	{
		validateLayer("Building::getLayerName", layerIndex);
		return mLayerNames[layerIndex];
	}

	void Building::setLayerName(uint32_t layerIndex, std::string name)
	{
		validateLayer("Building::setLayerName", layerIndex);
		mLayerNames[layerIndex] = std::move(name);
		modify();
	}

	string Building::defaultLayerName(uint32_t layer)
	{
		return format("Layer {}", layer);
	}

	uint32_t Building::addLayer()
	{
		auto const layerIndex = static_cast<uint32_t>(mLayers.size());

		if (layerIndex >= CORE_MAX_LAYERS)
		{
			throw BuildingException(this,
				format("Building cannot have more than {} layers", CORE_MAX_LAYERS));
		}

		mLayers.push_back(make_shared<Layer>(this, mCellsWide, mDecksHigh, layerIndex));
		mLayerNames.push_back(defaultLayerName(layerIndex));
		modify();

		return layerIndex;
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

	void Building::validateCellHasNoObject(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.hasObject())
		{
			throw BuildingException(this, format("{} - cell at {},{} has an object.", caller, x, y));
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

	void Building::validateCellHasNoPhysicalControl(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, int side) const
	{
		auto layer = getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.controls[side] != ~0u)
		{
			throw BuildingException(this, format("{} - cell at {},{} (side {}) has a physical control.", caller, x, y, side));
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
		if (layerIndex >= mLayers.size())
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

		if (cellsWide > mCellsWide - x)
		{
			throw BuildingException(this, format("{} - cellsWide={} is out of bounds", caller, cellsWide));
		}

		if (y >= mDecksHigh)
		{
			throw BuildingException(this, format("{} - y={} is out of bounds", caller, y));
		}

		if (decksHigh > mDecksHigh - y)
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
				validateCellUnoccupied(caller, layerIndex, ix, iy);
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

	void Building::validateObjectAllowedInSectorAsLookTarget(string const& caller, SectorObjectType type, uint32_t sectorIndex) const
	{
		auto sector = getSector(sectorIndex);

		if (!sector->sectorSupportsObjectAsLookTarget(type))
		{
			throw BuildingException(this, format("{} - Sector type '{}' does not support SectorObject type '{}'", caller, getSectorTypeString(sector->getType()), getSectorObjectTypeString(type)));
		}
	}

	void Building::validateSpaceOnlyInOneSector(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, bool allowAllBackgroundSpan) const
	{
		auto layer = getLayer(layerIndex);

		// A Window's back Layer is the one place where several Sectors behind one
		// aperture make sense: Backgrounds are seen, never entered, so a wide Window
		// looking across the seam between two of them still sees an unbroken view.
		// Half room-interior and half sky is a different matter - there should be a
		// wall where the room ends - so a mixed span is refused outright.
		if (allowAllBackgroundSpan)
		{
			bool seesBackground = false;
			bool seesOccupiedOther = false;
			for (uint32_t iy = y; iy < y + decksHigh; ++iy)
				for (uint32_t ix = x; ix < x + cellsWide; ++ix)
				{
					auto const& cellDef = layer->getCellDefinition(ix, iy);
					if (cellDef.sectorIndex == ~0u) continue;
					if (mSectors[cellDef.sectorIndex]->getType() == SectorType::Background)
						seesBackground = true;
					else
						seesOccupiedOther = true;
				}
			if (seesBackground && !seesOccupiedOther) return;
			if (seesBackground && seesOccupiedOther)
				throw BuildingException(this, format(
					"{} - bounds {},{} -> {},{} mix a Background with a Location or Transit on Layer {}; a Window cannot look half into a room and half into a Background, since there should be a wall where the room ends",
					caller, x, y, x + cellsWide, y + decksHigh, layerIndex));
		}

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
		if (options.holdOpenSeconds < 0.0f)
		{
			throw BuildingException(this, format("{} - Door hold-open time cannot be negative.", caller));
		}
	}

	void Building::validateSectorForceBridgeOptions(string const& caller, CreateForceBridgeOptions const& options) const
	{
		if (options.width == 0 || options.width > CORE_FORCEBRIDGE_MAX_SIZE)
			throw BuildingException(this, format("{} - ForceBridge width must be [1,{}], not {}",
				caller, CORE_FORCEBRIDGE_MAX_SIZE, options.width));
		if (options.extensible && (options.controlCount < 1 || options.controlCount > 2))
			throw BuildingException(this, format("{} - Physical control count must be [1,2] for a controlled ForceBridge, not {}", caller, options.controlCount));
		if (!options.extensible && (options.controlCount != 0 || !options.startExtended))
			throw BuildingException(this, format("{} - A non-extensible ForceBridge must be permanently extended and have no controls", caller));
	}

	void Building::validateSectorLadderOptions(string const& caller, CreateLadderOptions const& options) const
	{
		if (options.directionalBatchLimit == 0)
		{
			throw BuildingException(this, format("{} - Ladder directional batch limit must be positive.", caller));
		}
	}

	void Building::validateLiftOptions(string const& caller, CreateLiftOptions const& options) const
	{
		if (options.cellsWide == 0)
		{
			throw BuildingException(this, format("{} - Lift width must be positive.", caller));
		}
		if (options.stopOffsets.size() < 2)
		{
			throw BuildingException(this, format("{} - Lift must have at least 2 stops.", caller));
		}
		for (size_t i = 1; i < options.stopOffsets.size(); ++i)
		{
			if (options.stopOffsets[i] <= options.stopOffsets[i - 1])
			{
				throw BuildingException(this, format("{} - Lift stop offsets must be strictly increasing; stop {} ({}) is not above stop {} ({}).",
					caller, i, options.stopOffsets[i], i - 1, options.stopOffsets[i - 1]));
			}
		}
		if (options.capacity == 0)
		{
			throw BuildingException(this, format("{} - Lift capacity must be positive.", caller));
		}
		if (options.initialStop >= options.stopOffsets.size())
		{
			throw BuildingException(this, format("{} - Lift initial stop is out of range.", caller));
		}
		auto representablePositions = (uint32_t)floor((float)options.cellsWide / CORE_AGENT_MAX_WIDTH);
		if (options.capacity > representablePositions)
		{
			throw BuildingException(this, format("{} - Lift capacity {} exceeds {} representable interior standing positions.",
				caller, options.capacity, representablePositions));
		}
		if (options.minimumDwellSeconds < 0.0f || options.maximumBoardingSeconds < 0.0f
			|| options.maximumBoardingSeconds < options.minimumDwellSeconds)
		{
			throw BuildingException(this, format("{} - Lift timing requires 0 <= minimum dwell <= maximum boarding time.", caller));
		}
	}

	void Building::validateShuttleOptions(string const& caller, CreateShuttleOptions const& options) const
	{
		if (options.numCars == 0)
			throw BuildingException(this, format("{} - Shuttle must have at least one carriage.", caller));
		if (options.carWidth < 3 || options.carWidth > 5)
		{
			throw BuildingException(this, format("{} - Shuttle car width must be between 3 and 5.", caller));
		}
		if (options.doorMask == 0 || (options.doorMask >> options.carWidth) != 0)
		{
			throw BuildingException(this, format("{} - Shuttle carriage door layout must select at least one cell and remain within the carriage width.", caller));
		}

		if (options.stopOffsets.size() < 2)
			throw BuildingException(this, format("{} - Shuttle must have at least two stops.", caller));
		if (options.initialStop >= (uint32_t)options.stopOffsets.size())
		{
			throw BuildingException(this, format("{} - Shuttle initialStop parameter out of bounds.", caller));
		}
		for (size_t i = 1; i < options.stopOffsets.size(); ++i)
			if (options.stopOffsets[i] <= options.stopOffsets[i - 1])
				throw BuildingException(this, format("{} - Shuttle stop offsets must be strictly increasing.", caller));
		if (options.capacity == 0
			|| options.capacity > (uint32_t)floor((float)options.carWidth / CORE_AGENT_MAX_WIDTH))
			throw BuildingException(this, format("{} - Shuttle capacity cannot be represented by carriage standing positions.", caller));
		if (options.minimumDwellSeconds < 0.0f
			|| options.maximumBoardingSeconds < options.minimumDwellSeconds)
			throw BuildingException(this, format("{} - Shuttle timing requires 0 <= minimum dwell <= maximum boarding time.", caller));
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

	uint32_t Building::createLocation(string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, bool isCorridor)
	{
		auto sectorIndex = (uint32_t)mSectors.size();

		auto location = make_shared<Location>(name, type, layerIndex, sectorIndex, x, y, cellsWide, decksHigh, topDeckHeight, ~0u, isCorridor);
		
		mSectors.push_back(location);
		return sectorIndex;
	}

	uint32_t Building::createLadder(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options)
	{
		auto y0 = y;
		auto y1 = y + options.decksHigh - 1;

		// Get Locations this Ladder connects.  A Transit on layerIndex lands on the
		// Layer directly in front of it, never on a Layer of its own choosing.
		auto const& landing = mLayers[layerInFront(layerIndex)];
		auto const& cellDef0 = landing->getCellDefinition(x, y0);
		auto const& cellDef1 = landing->getCellDefinition(x, y1);
		
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
		auto ladder = make_shared<LadderTransit>(sectorIndex, layerIndex, x, y, options.decksHigh, stops, options.extensible, options.startExtended);

		mSectors.push_back(ladder);
		return sectorIndex;
	}

	uint32_t Building::createStairwell(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t decksHigh, int mountSide)
	{
		ASSERT_SIDE_OK(mountSide);

		// Get Locations this Stairwell connects.  Because a Stairwell is two cells wide, we
		// just check the first horizontal cell, ie xOffset==0.  Every landing sits on the
		// Layer directly in front of the Transit.
		vector<TransitStop> stops;
		auto const& landing = mLayers[layerInFront(layerIndex)];

		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			auto const& cellDef = landing->getCellDefinition(x, iy);
			auto sector = getSector(cellDef.sectorIndex);

			stops.push_back({ 
				sector,
				(int)x - (int)sector->getCellX(),
				(int)iy - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();
		auto stairwell = make_shared<StairwellTransit>(sectorIndex, layerIndex, x, y, decksHigh, mountSide, stops);

		mSectors.push_back(stairwell);
		return sectorIndex;
	}

	uint32_t Building::createStaircase(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
		int riseSide, float speed)
	{
		ASSERT_SIDE_OK(riseSide);
		uint32_t const lowerX = riseSide == CORE_SIDE_RIGHT ? x : x + cellsWide - 1;
		uint32_t const upperX = riseSide == CORE_SIDE_RIGHT ? x + cellsWide - 1 : x;
		auto const& landing = mLayers[layerInFront(layerIndex)];
		auto const& lowerCell = landing->getCellDefinition(lowerX, y);
		auto const& upperCell = landing->getCellDefinition(upperX, y + 1);
		auto lower = getSector(lowerCell.sectorIndex);
		auto upper = getSector(upperCell.sectorIndex);
		vector<TransitStop> stops{
			{ lower, (int)lowerX - (int)lower->getCellX(), (int)y - (int)lower->getCellY() },
			{ upper, (int)upperX - (int)upper->getCellX(), (int)(y + 1) - (int)upper->getCellY() }
		};
		auto sectorIndex = (uint32_t)mSectors.size();
		mSectors.push_back(make_shared<StaircaseTransit>(sectorIndex, layerIndex, x, y, cellsWide,
			riseSide, speed, stops));
		return sectorIndex;
	}

	Building::CreateObjectResult Building::createLift(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
		uint32_t decksHigh, vector<uint32_t> const& stopOffsets)
	{
		// Get Locations this Lift connects, all on the Layer directly in front.
		vector<TransitStop> stops;
		auto const& landing = mLayers[layerInFront(layerIndex)];

		for (auto stopOffset : stopOffsets)
		{
			uint32_t iy = y + stopOffset;

			auto const& cellDef = landing->getCellDefinition(x, iy);
			auto sector = getSector(cellDef.sectorIndex);

			stops.push_back({
				sector,
				(int)x - (int)sector->getCellX(),
				(int)iy - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();
		auto lift = make_shared<LiftTransit>(sectorIndex, layerIndex, x, y, cellsWide, decksHigh, stops);

		mSectors.push_back(lift);
		
		return {
			~0u,
			SectorObjectType::Lift,
			lift
		};
	}

	Building::CreateObjectResult Building::createShuttle(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth, vector<uint32_t> const& stopOffsets)
	{
		// Get Locations this Shuttle connects, all on the Layer directly in front.
		vector<TransitStop> stops;
		auto const& landing = mLayers[layerInFront(layerIndex)];

		for (auto stopOffset : stopOffsets)
		{
			uint32_t ix = x + stopOffset;
			shared_ptr<const Sector> sector;
			for (uint32_t car = 0; car < numCars && !sector; ++car)
			{
				uint32_t cx = ix + car * (carWidth + 1) + 1;
				auto const& cellDef = landing->getCellDefinition(cx, y);
				bool supported = cellDef.sectorIndex != ~0u;
				if (supported && carWidth == 4)
					supported = landing->getCellDefinition(cx + 1, y).sectorIndex
						== cellDef.sectorIndex;
				if (supported) sector = getSector(cellDef.sectorIndex);
			}
			assert(sector);
			stops.push_back({
				sector,
				(int)ix - (int)sector->getCellX(),
				(int)y - (int)sector->getCellY(),
			});
		}

		auto sectorIndex = (uint32_t)mSectors.size();

		// Need to account for proper track dimensions, width is not necessarily
		// the last stop offset
		auto shuttle = make_shared<ShuttleTransit>(sectorIndex, layerIndex, x, y, cellsWide, numCars, carWidth, stops);

		mSectors.push_back(shuttle);
		
		return {
			~0u,
			SectorObjectType::Shuttle,
			shuttle
		};
	}

	uint32_t Building::addLocation(string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, bool isCorridor)
	{
		string caller = format("Building::addLocation({}, {}, {}, {}, {}, {}, {} {})", name, getSectorTypeString(type), layerIndex, x, y, cellsWide, decksHigh, topDeckHeight);
		
		validateBounds(caller, x, y, cellsWide, decksHigh);
		validateLayerSpace(caller, layerIndex, x, y, cellsWide, decksHigh);

		// Create sector
		auto sectorIndex = createLocation(name, type, layerIndex, x, y, cellsWide, decksHigh, topDeckHeight, isCorridor);

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

	Building::CreateObjectResult Building::createDoor(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t* vertexIdentifier)
	{	
		string caller = format("Building::createDoor({}, {}, {}, {})", layerIndex, x, y, cellsWide);

		// A Door is authored on the front Layer of its pair and opens into the Layer
		// directly behind it.
		auto const backLayer = layerBehind(layerIndex);

		validateCellOccupied(caller, layerIndex, x, y);
		validateCellOccupied(caller, backLayer, x, y);
		validateCellHasNoDoor(caller, layerIndex, x, y);
		validateCellHasNoDoor(caller, backLayer, x, y);

		// Find Sectors that the Door is connecting.
		auto foreSector = _getSector(mLayers[layerIndex]->getCellDefinition(x, y).sectorIndex);
		auto backSector = _getSector(mLayers[backLayer]->getCellDefinition(x, y).sectorIndex);

		// A Door's front Sector is a Location or a Facade: a Facade follows
		// the Room hosting rule (ADR 0003, ticket #48).
		assert(isLocationLike(foreSector->getType()));

		// Create door in Fore Location and add to Back.
		uint32_t doorIndex = foreSector->createDoor(foreSector, backSector, x, y, cellsWide, vertexIdentifier);
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

		// A Window is authored on the front Layer of the pair it crosses.  The Layer
		// behind it is only absent when a map written before the back-most Layer rule
		// is replayed: authoring such a Window is refused by canAddSectorWindow(), and a
		// Layer deletion removes one it would strand there.
		bool const hasBack = layerIndex + 1 < getLayerCount();

		validateCellOccupied(caller, layerIndex, x, y);
		if (hasBack)
			validateCellOccupied(caller, layerBehind(layerIndex), x, y);

		// Find sectors that the Window is connecting, if any.
		shared_ptr<Sector> foreSector{ nullptr };
		shared_ptr<Sector> backSector{ nullptr };

		foreSector = _getSector(mLayers[layerIndex]->getCellDefinition(x, y).sectorIndex);
		if (hasBack)
		{
			auto const& cellDef1 = mLayers[layerBehind(layerIndex)]->getCellDefinition(x, y);

			// The back Sector is the Sector behind the Window's first cell.  A Window
			// may span several Backgrounds (#36), so for such a Window this is the
			// first Background in the span and is non-authoritative; the cell grid
			// holds the truth about what the whole aperture looks into.
			backSector = cellDef1.sectorIndex != ~0u ? _getSector(cellDef1.sectorIndex) : nullptr;
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

	Building::CreateObjectResult Building::createPhysicalControl(string const& name,
		uint32_t layerIndex, uint32_t x, uint32_t y, int side, uint32_t flags,
		uint32_t* vertexIdentifier, uint32_t alternateX, int alternateSide)
	{
		string caller = format("Building::createPhysicalControl({}, {}, {}, {}, {})", layerIndex, x, y, side, flags);
		vector<PhysicalControlCandidate> candidates{ { x, side } };
		if (alternateX != ~0u && alternateSide >= CORE_SIDE_LEFT
			&& alternateSide <= CORE_SIDE_MIDDLE
			&& (alternateX != x || alternateSide != side))
		{
			candidates.push_back({ alternateX, alternateSide });
		}

		uint32_t initialCandidate = ~0u;
		uint32_t sectorIndex = ~0u;
		for (uint32_t i = 0; i < candidates.size(); ++i)
		{
			auto const& candidate = candidates[i];
			validateCellOccupied(caller, layerIndex, candidate.cellX, y);
			auto const& cell = mLayers[layerIndex]->getCellDefinition(candidate.cellX, y);
			if (sectorIndex == ~0u) sectorIndex = cell.sectorIndex;
			else if (sectorIndex != cell.sectorIndex)
				throw BuildingException(this, format("{} - candidate positions cross Sector boundaries", caller));
			if (cell.controls[candidate.side] == ~0u && initialCandidate == ~0u)
				initialCandidate = i;
		}
		if (initialCandidate == ~0u)
		{
			// Include the not-yet-created control in the row optimization. This can
			// move a flexible existing control out of the required slot before the
			// new object is constructed.
			mPhysicalControlPlacements.push_back({ layerIndex, sectorIndex, ~0u, y,
				candidates, 0, 0 });
			try
			{
				reflowPhysicalControls(layerIndex, sectorIndex, y);
				initialCandidate = mPhysicalControlPlacements.back().currentCandidate;
			}
			catch (...)
			{
				mPhysicalControlPlacements.pop_back();
				throw;
			}
			mPhysicalControlPlacements.pop_back();
		}

		auto const& initial = candidates[initialCandidate];
		validateCellHasNoPhysicalControl(caller, layerIndex, initial.cellX, y, initial.side);
		auto& cellDef = mLayers[layerIndex]->getCellDefinition(initial.cellX, y);
		auto sector = _getSector(cellDef.sectorIndex);

		float xOffset = initial.side == CORE_SIDE_MIDDLE
			? 0.5f - CORE_BUTTON_SIZE * 0.5f
			: initial.side - CORE_BUTTON_SIZE * 0.5f;
		auto controlIndex = sector->createPhysicalControl(sector, name, initial.cellX, y,
			xOffset, CORE_BUTTON_Y_OFFSET, flags, vertexIdentifier);
		cellDef.controls[initial.side] = controlIndex;
		mPhysicalControlPlacements.push_back({ layerIndex, sector->getIndex(), controlIndex, y,
			std::move(candidates), 0, initialCandidate });
		reflowPhysicalControls(layerIndex, sector->getIndex(), y);

		return { controlIndex, SectorObjectType::InteractionPoint, sector };
	}

	void Building::reflowPhysicalControls(uint32_t layerIndex, uint32_t sectorIndex, uint32_t y)
	{
		vector<uint32_t> row;
		for (uint32_t i = 0; i < mPhysicalControlPlacements.size(); ++i)
		{
			auto const& placement = mPhysicalControlPlacements[i];
			if (placement.layerIndex == layerIndex && placement.sectorIndex == sectorIndex
				&& placement.cellY == y)
				row.push_back(i);
		}
		if (row.empty()) return;

		auto centerKey = [](PhysicalControlCandidate const& candidate)
		{
			return static_cast<int64_t>(candidate.cellX) * 2
				+ (candidate.side == CORE_SIDE_RIGHT ? 2
					: candidate.side == CORE_SIDE_MIDDLE ? 1 : 0);
		};
		auto connected = [&](uint32_t left, uint32_t right)
		{
			for (auto const& a : mPhysicalControlPlacements[left].candidates)
				for (auto const& b : mPhysicalControlPlacements[right].candidates)
					if (centerKey(a) == centerKey(b)) return true;
			return false;
		};

		vector<bool> visited(row.size(), false);
		for (uint32_t root = 0; root < row.size(); ++root)
		{
			if (visited[root]) continue;
			vector<uint32_t> component;
			vector<uint32_t> pending{ root };
			visited[root] = true;
			while (!pending.empty())
			{
				auto local = pending.back();
				pending.pop_back();
				component.push_back(row[local]);
				for (uint32_t other = 0; other < row.size(); ++other)
				{
					if (!visited[other] && connected(row[local], row[other]))
					{
						visited[other] = true;
						pending.push_back(other);
					}
				}
			}
			sort(component.begin(), component.end());

			vector<uint32_t> choice(component.size()), bestChoice;
			set<pair<uint32_t, int>> occupiedSlots;
			bool haveBest = false;
			uint32_t bestUnique = 0, bestMoved = 0, bestDefaults = 0;
			function<void(uint32_t)> search = [&](uint32_t depth)
			{
				if (depth != component.size())
				{
					auto const& placement = mPhysicalControlPlacements[component[depth]];
					for (uint32_t candidateIndex = 0; candidateIndex < placement.candidates.size(); ++candidateIndex)
					{
						auto const& candidate = placement.candidates[candidateIndex];
						auto slot = make_pair(candidate.cellX, candidate.side);
						if (!occupiedSlots.insert(slot).second) continue;
						choice[depth] = candidateIndex;
						search(depth + 1);
						occupiedSlots.erase(slot);
					}
					return;
				}

				set<int64_t> centers;
				uint32_t moved = 0, defaults = 0;
				for (uint32_t i = 0; i < component.size(); ++i)
				{
					auto const& placement = mPhysicalControlPlacements[component[i]];
					centers.insert(centerKey(placement.candidates[choice[i]]));
					moved += choice[i] != placement.currentCandidate;
					defaults += choice[i] == placement.defaultCandidate;
				}
				auto unique = static_cast<uint32_t>(centers.size());
				bool better = !haveBest || unique > bestUnique
					|| (unique == bestUnique && moved < bestMoved);
				if (!better && haveBest && unique == bestUnique && moved == bestMoved)
				{
					for (uint32_t i = 0; i < component.size(); ++i)
					{
						auto const& placement = mPhysicalControlPlacements[component[i]];
						bool retained = choice[i] == placement.currentCandidate;
						bool bestRetained = bestChoice[i] == placement.currentCandidate;
						if (retained != bestRetained) { better = retained; break; }
					}
					if (!better)
					{
						bool sameRetention = true;
						for (uint32_t i = 0; i < component.size(); ++i)
						{
							auto const& placement = mPhysicalControlPlacements[component[i]];
							if ((choice[i] == placement.currentCandidate)
								!= (bestChoice[i] == placement.currentCandidate))
							{ sameRetention = false; break; }
						}
						if (sameRetention && (defaults > bestDefaults
							|| (defaults == bestDefaults && choice < bestChoice))) better = true;
					}
				}
				if (better)
				{
					haveBest = true;
					bestUnique = unique;
					bestMoved = moved;
					bestDefaults = defaults;
					bestChoice = choice;
				}
			};
			search(0);
			if (!haveBest) throw BuildingException(this, "Physical-control placement constraints cannot be satisfied");
			for (uint32_t i = 0; i < component.size(); ++i)
				mPhysicalControlPlacements[component[i]].currentCandidate = bestChoice[i];
		}

		// Replace cell-side registrations atomically after all assignments are known.
		for (auto index : row)
		{
			auto const& placement = mPhysicalControlPlacements[index];
			if (placement.objectIndex == ~0u) continue;
			for (auto const& candidate : placement.candidates)
			{
				auto& slot = mLayers[layerIndex]->getCellDefinition(candidate.cellX, y).controls[candidate.side];
				if (slot == placement.objectIndex) slot = ~0u;
			}
		}
		map<int64_t, vector<uint32_t>> collisions;
		for (auto index : row)
		{
			auto& placement = mPhysicalControlPlacements[index];
			auto const& candidate = placement.candidates[placement.currentCandidate];
			if (placement.objectIndex != ~0u)
			{
				auto& slot = mLayers[layerIndex]->getCellDefinition(candidate.cellX, y).controls[candidate.side];
				if (slot != ~0u) throw BuildingException(this, "Physical-control slot assignment collided with an existing control");
				slot = placement.objectIndex;
			}
			collisions[centerKey(candidate)].push_back(index);
		}

		for (auto const& [center, controls] : collisions)
		{
			(void)center;
			for (auto index : controls)
			{
				auto& placement = mPhysicalControlPlacements[index];
				if (placement.objectIndex == ~0u) continue;
				auto const& candidate = placement.candidates[placement.currentCandidate];
				float centerX = candidate.cellX + (candidate.side == CORE_SIDE_RIGHT ? 1.0f
					: candidate.side == CORE_SIDE_MIDDLE ? 0.5f : 0.0f);
				if (candidate.side == CORE_SIDE_LEFT) centerX += placement.edgeInset;
				else if (candidate.side == CORE_SIDE_RIGHT) centerX -= placement.edgeInset;
				float adjustment = 0.0f;
				if (controls.size() > 1)
				{
					if (candidate.side == CORE_SIDE_RIGHT) adjustment = 0.025f;
					else if (candidate.side == CORE_SIDE_LEFT) adjustment = -0.025f;
				}
				auto control = static_pointer_cast<ButtonSectorObject>(
					mSectors[sectorIndex]->_getObject(placement.objectIndex));
				control->_setCellPosition(candidate.cellX, y);
				auto button = static_pointer_cast<Button>(control->_getObject());
				button->_setPlacement(centerX, y + CORE_BUTTON_Y_OFFSET, adjustment);
				if (placement.hasInteractionOffset)
				{
					auto point = mInteractionPoints.find(button->getInteractionPointId());
					if (point)
						point->mPosition = button->getPosition() + button->getSize() * 0.5f
							+ placement.interactionOffset;
				}
			}
		}
	}

	void Building::bindPhysicalControl(CreateObjectResult& control, InteractionPointId point)
	{
		control.interactionPoint = point;
		auto object = control.sector->getObject(control.index)->_getObject();
		auto button = dynamic_pointer_cast<Button>(object);
		if (!button) throw logic_error("Physical control is not backed by a Button");
		button->_setInteractionPointId(point);
		for (auto& placement : mPhysicalControlPlacements)
		{
			if (placement.sectorIndex != control.sector->getIndex()
				|| placement.objectIndex != control.index) continue;
			auto interaction = mInteractionPoints.find(point);
			if (interaction)
			{
				placement.interactionOffset = interaction->mPosition
					- (button->getPosition() + button->getSize() * 0.5f);
				placement.hasInteractionOffset = true;
			}
			break;
		}
	}

	InteractionPointId Building::createPhysicalControlInteractionPoint(string const& name,
		CreateObjectResult& control, float standingY, float reach,
		float durationSeconds, vector<InteractionBinding> bindings)
	{
		if (!control.sector) throw invalid_argument("A physical control requires an owning sector");
		auto sectorObject = control.sector->_getObject(control.index);
		auto button = sectorObject ? dynamic_pointer_cast<Button>(sectorObject->_getObject()) : nullptr;
		if (!button) throw logic_error("Physical control is not backed by a Button");
		auto position = button->getPosition() + button->getSize() * 0.5f;
		position.y = standingY;
		auto point = createInteractionPoint(name,
			SectorId{ (uint64_t)control.sector->getIndex() + 1 }, position,
			reach, durationSeconds, std::move(bindings));
		bindPhysicalControl(control, point);
		return point;
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

		return {
			// LiftSectorObject and Lift both expect offsets relative to the global
			// base y. Passing global stop positions here would apply y twice and make
			// PlatformLifts in offset Rooms start above their ground floor.
			sector->createPlatformLift(sector, x, y, options.cellsWide,
				options.stopOffsets, vertexIdentifier),
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
		int cellY1 = min((int)((y + height) / CORE_DECK_HEIGHT_PIXELS), (int)mDecksHigh - 1);

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
		return addCorridor(0, y, x, cellsWide, decksHigh);
	}

	uint32_t Building::addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t decksHigh)
	{
		string const caller = format("Building::addCorridor({}, {}, {}, {}, {})", layerIndex, y, x, cellsWide, decksHigh);
		// Every rejecting check runs before beginStructuralEdit() so a refused
		// call stays a true no-op: no modified flag, no topology invalidation
		// (ticket #93).
		validateLayer(caller, layerIndex);
		// Minimum (1,1). A zero-sized Location would pass the bounds checks by covering
		// nothing, leaving a Sector no cell references: invisible, unselectable, and
		// unreachable forever (ticket #64).
		if (cellsWide == 0)
			throw BuildingException(this, format("{} - a Corridor must be at least one cell wide", caller));
		if (decksHigh == 0)
			throw BuildingException(this, format("{} - a Corridor must be at least one deck high", caller));
		beginStructuralEdit("addCorridor");
		auto const result = addLocation("Corridor", SectorType::Location, layerIndex, x, y, cellsWide, decksHigh, CORE_CORRIDOR_HEIGHT, true);
		ConstructionRecord record{ ConstructionType::Corridor };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = decksHigh;
		recordConstruction(std::move(record));
		return result;
	}

	uint32_t Building::addRoom(string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight)
	{
		string const caller = format("Building::addRoom({}, {}, {}, {}, {}, {}, {})", name, layerIndex, y, x, cellsWide, decksHigh, topDeckHeight);
		// Every rejecting check runs before beginStructuralEdit() so a refused
		// call stays a true no-op: no modified flag, no topology invalidation
		// (ticket #93).
		// Written as a negated in-range test so a NaN topDeckHeight is rejected
		// too: NaN fails both comparisons, so the plain < / > pair would let it
		// through (ticket #55).
		if (!(topDeckHeight >= CORE_ROOM_MIN_HEIGHT && topDeckHeight <= CORE_ROOM_MAX_HEIGHT))
		{
			throw BuildingException(this, format("{} - topDeckHeight={} is out of range", caller, topDeckHeight));
		}
		// Minimum (1,1), the same invariant Background and Facade enforce: a
		// zero-sized Room would pass the bounds checks by covering nothing,
		// leaving a Sector no cell references (ticket #64).
		if (cellsWide == 0)
			throw BuildingException(this, format("{} - a Room must be at least one cell wide", caller));
		if (decksHigh == 0)
			throw BuildingException(this, format("{} - a Room must be at least one deck high", caller));

		beginStructuralEdit("addRoom");

		auto const result = addLocation(name, SectorType::Location, layerIndex, x, y, cellsWide, decksHigh, topDeckHeight, false);
		ConstructionRecord record{ ConstructionType::Room };
		record.name = name;
		record.a = layerIndex; record.b = y; record.c = x; record.d = cellsWide; record.e = decksHigh;
		record.x = topDeckHeight;
		recordConstruction(std::move(record));
		return result;
	}

	bool Building::canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t decksHigh, string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		string caller = format("Building::addBackground({}, {}, {}, {}, {})",
			layerIndex, y, x, cellsWide, decksHigh);

		try
		{
			validateLayer(caller, layerIndex);

			// Minimum (1,1). A zero-sized block would pass the bounds checks by covering
			// nothing, which is not a Background worth authoring.
			if (cellsWide == 0)
				throw BuildingException(this, format("{} - a Background must be at least one cell wide", caller));
			if (decksHigh == 0)
				throw BuildingException(this, format("{} - a Background must be at least one deck high", caller));

			validateBounds(caller, x, y, cellsWide, decksHigh);

			// Every cell must be unoccupied on the Background's own Layer, which is the
			// same rule a Location plays by. Any Layer is legal, front-most and back-most
			// included: a "back layers only" rule would re-introduce exactly the
			// Fore/Back special-casing that ADR 0002 removed.
			validateLayerSpace(caller, layerIndex, x, y, cellsWide, decksHigh);
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		return true;
	}

	uint32_t Building::addBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t decksHigh, BackgroundColour const& colour)
	{
		string diagnostic;
		if (!canAddBackground(layerIndex, y, x, cellsWide, decksHigh, &diagnostic))
			throw BuildingException(this, diagnostic);

		beginStructuralEdit("addBackground");

		auto sectorIndex = (uint32_t)mSectors.size();
		mSectors.push_back(make_shared<Background>("Background", layerIndex, sectorIndex,
			x, y, cellsWide, decksHigh, colour));

		auto layer = getLayer(layerIndex);
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);
				cellDef.sectorIndex = sectorIndex;
				// A Background owns no walkable floor and hosts no object, so its cells
				// carry no floor and no SectorObject.
				cellDef.floorType = Background::cellFloorType();
				cellDef.sectorObjectType = SectorObjectType::None;
			}
		}

		ConstructionRecord record{ ConstructionType::Background };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = decksHigh;
		// The whole Background appearance fits one integer, so no new record field is
		// needed to persist its colour.
		record.f = packBackgroundColour(colour);
		recordConstruction(std::move(record));

		return sectorIndex;
	}

	bool Building::canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t decksHigh, float topDeckHeight, string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		string caller = format("Building::addFacade({}, {}, {}, {}, {})",
			layerIndex, y, x, cellsWide, decksHigh);

		try
		{
			validateLayer(caller, layerIndex);

			// A Facade is placed exactly as a Room is: minimum (1,1), inside the
			// Building bounds, and on cells unoccupied on its own Layer.
			if (cellsWide == 0)
				throw BuildingException(this, format("{} - a Facade must be at least one cell wide", caller));
			if (decksHigh == 0)
				throw BuildingException(this, format("{} - a Facade must be at least one deck high", caller));
			// Same negated in-range test as addRoom: a NaN topDeckHeight must
			// not sail through the < / > pair (ticket #55).
			if (!(topDeckHeight >= CORE_ROOM_MIN_HEIGHT && topDeckHeight <= CORE_ROOM_MAX_HEIGHT))
				throw BuildingException(this, format("{} - topDeckHeight={} is out of range", caller, topDeckHeight));

			validateBounds(caller, x, y, cellsWide, decksHigh);
			validateLayerSpace(caller, layerIndex, x, y, cellsWide, decksHigh);
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		return true;
	}

	uint32_t Building::addFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t decksHigh, float topDeckHeight, BackgroundColour const& colour)
	{
		return addFacade(Facade::defaultName(), layerIndex, y, x, cellsWide, decksHigh,
			topDeckHeight, colour);
	}

	uint32_t Building::addFacade(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, BackgroundColour const& colour)
	{
		string diagnostic;
		if (!canAddFacade(layerIndex, y, x, cellsWide, decksHigh, topDeckHeight, &diagnostic))
			throw BuildingException(this, diagnostic);

		beginStructuralEdit("addFacade");

		auto sectorIndex = (uint32_t)mSectors.size();
		mSectors.push_back(make_shared<Facade>(name, layerIndex, sectorIndex,
			x, y, cellsWide, decksHigh, topDeckHeight, colour));

		auto layer = getLayer(layerIndex);
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);
				cellDef.sectorIndex = sectorIndex;
				// A Facade owns walkable floor exactly as a Location does: ground
				// on the bottom deck, upper decks reached by Walkways.
				cellDef.floorType = iy == y ? CellFloorType::Ground : CellFloorType::None;
			}
		}

		ConstructionRecord record{ ConstructionType::Facade };
		record.name = name;
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = decksHigh;
		record.x = topDeckHeight;
		// The Facade reuses Background's colour packing, so the editor and the
		// headless checks round-trip the same arithmetic (ADR 0003).
		record.f = packBackgroundColour(colour);
		recordConstruction(std::move(record));

		return sectorIndex;
	}

	bool Building::canAddLadder(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t decksHigh,
		string* diagnostic) const
	{
		auto reject = [&](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (diagnostic) diagnostic->clear();
		if (layerIndex == 0 || layerIndex >= getLayerCount())
			return reject("A Ladder must sit on a Layer that has a Layer in front of it to land on");
		if (decksHigh < 2) return reject("A Ladder must span at least two decks");
		if (x >= mCellsWide || y >= mDecksHigh || y + decksHigh > mDecksHigh)
			return reject("The Ladder is outside the Building bounds");
		auto const& transitLayer = mLayers[layerIndex];
		auto const& landing = mLayers[layerInFront(layerIndex)];
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
			if (transitLayer->getCellDefinition(x, iy).occupied())
				return reject(format("A Sector at {},{} blocks the Ladder", x, iy));

		auto upperY = y + decksHigh - 1;
		auto const& lower = landing->getCellDefinition(x, y);
		auto const& upper = landing->getCellDefinition(x, upperY);
		if (lower.sectorIndex == ~0u)
			return reject(format("A landing Location is required at {},{}", x, y));
		if (upper.sectorIndex == ~0u)
			return reject(format("A landing Location is required at {},{}", x, upperY));
		if (lower.sectorIndex == upper.sectorIndex)
			return reject("A Ladder must connect two different landing Locations");
		auto lowerSector = mSectors[lower.sectorIndex];
		auto upperSector = mSectors[upper.sectorIndex];
		// A Ladder lands on any location-like Sector: a Room, a Corridor, or a
		// Facade.  A Facade takes part in traversal exactly as a Room does
		// (ADR 0003, ticket #52), so it is a valid Ladder landing.
		if (!lowerSector || !isLocationLike(lowerSector->getType()))
			return reject(format("A landing Location is required at {},{}", x, y));
		if (!upperSector || !isLocationLike(upperSector->getType()))
			return reject(format("A landing Location is required at {},{}", x, upperY));
		if (!lower.isTraversableOnFoot())
			return reject(format("The landing floor at {},{} is not traversable", x, y));
		if (!upper.isTraversableOnFoot())
			return reject(format("The landing floor at {},{} is not traversable", x, upperY));
		return true;
	}

	Building::CreateLadderResult Building::addLadder(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLadderOptions const& options)
	{
		beginStructuralEdit("addLadder");
		// A Ladder Transit sits on layerIndex and lands on the Layer directly in front.
		auto const landingLayer = layerInFront(layerIndex);
		auto transitLayer = getLayer(layerIndex);

		// Checks
		string caller = format("Building::addLadder({}, {}, {}, {}, {})", layerIndex, y, x, options.decksHigh, options.startExtended);

		validateLayer(caller, layerIndex);
		if (isFrontMostLayer(layerIndex))
		{
			throw BuildingException(this, format("{} - a Ladder cannot be placed on the front-most Layer, because it has no Layer in front to land on", caller));
		}

		if (options.decksHigh < 2)
		{
			throw BuildingException(this, format("{} - Ladder at {},{} must be at least 2 decks high", caller, x, y));
		}

		validateBounds(caller, x, y, 1, options.decksHigh);
		validateLayerSpace(caller, layerIndex, x, y, 1, options.decksHigh);

		auto y0 = y;
		auto y1 = y + options.decksHigh - 1;

		// Make sure the landing cells have a Sector
		auto const& cellDef0 = mLayers[landingLayer]->getCellDefinition(x, y0);
		auto const& cellDef1 = mLayers[landingLayer]->getCellDefinition(x, y1);
		auto foreSectorIndex0 = cellDef0.sectorIndex;
		auto foreSectorIndex1 = cellDef1.sectorIndex;

		if (foreSectorIndex0 == ~0u)
		{
			throw BuildingException(this, format("{} - landing cell at {},{} is not occupied, which blocks ladder being placed", caller, x, y0));
		}
		if (foreSectorIndex1 == ~0u)
		{
			throw BuildingException(this, format("{} - landing cell at {},{} is not occupied, which blocks ladder being placed", caller, x, y1));
		}
		if (foreSectorIndex0 == foreSectorIndex1)
		{
			throw BuildingException(this, format("{} - landing cells from {},{} to {},{} are the same sector, which blocks ladder being placed", caller, x, y0, x, y1));
		}

		// Ladders can only connect location-like Sectors: Rooms, Corridors, and
		// Facades (ADR 0003, ticket #52).
		auto const& foreSector0 = _getSector(foreSectorIndex0);
		auto const& foreSector1 = _getSector(foreSectorIndex1);

		if (!isLocationLike(foreSector0->getType()))
		{
			throw BuildingException(this, format("{} - landing cell at {},{} is not a Location, which blocks ladder being placed", caller, x, y0));
		}
		if (!isLocationLike(foreSector1->getType()))
		{
			throw BuildingException(this, format("{} - landing cell at {},{} is not a Location, which blocks ladder being placed", caller, x, y1));
		}

		// Ladders ends must not be in the air
		validateCellTraversableOnFoot(caller, "Ladder", landingLayer, x, y0);
		validateCellTraversableOnFoot(caller, "Ladder", landingLayer, x, y1);

		validateSectorLadderOptions(caller, options);

		// Create ladder
		auto sectorIndex = createLadder(layerIndex, x, y, options);

		// Set layers
		for (uint32_t iy = y; iy < y + options.decksHigh; ++iy)
		{
			auto& cellDef = transitLayer->getCellDefinition(x, iy);

			cellDef.sectorIndex = sectorIndex;
		}

		auto ladderSector = _getSector(sectorIndex);
		auto ladderTransit = dynamic_pointer_cast<LadderTransit>(ladderSector);
		auto ladder = ladderTransit->getLadder();
		auto traversalResource = createLadderTraversalResource("Ladder capacity", ladder,
			SectorId{ (uint64_t)sectorIndex + 1 }, options.directionalBatchLimit);
		configureLadderQueueLanes(traversalResource,
			{ SectorId{ (uint64_t)foreSector0->getIndex() + 1 },
				SectorId{ (uint64_t)foreSector1->getIndex() + 1 } },
			{ Vector2{ (float)x + 0.5f, (float)y0 },
				Vector2{ (float)x + 0.5f, (float)y1 } });
		ladder->configureTraversal(traversalResource);
		auto registerExtensionControl = [&](CreateObjectResult& control)
		{
			auto object = control.sector->_getObject(control.index);
			DeviceCommand command;
			command.type = DeviceCommandType::SetExtendedState;
			command.desiredState = true;
			command.traversalResource = traversalResource;
			auto point = createPhysicalControlInteractionPoint("Ladder extension control",
				control, (float)object->getCellY(), 0.15f, getFixedTimestep(),
				{ { command, InteractionBindingRequirement::Required } });
			addTraversalControl(traversalResource, point);
		};

		CreateObjectResult createdControls[2];
		if (options.extensible)
		{
			auto inwardSide = [x](shared_ptr<const Sector> const& sector)
			{
				return x == sector->getCellX1() ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;
			};
			createdControls[CORE_LEVEL_LOW] = _createLadderButton(
				foreSector0, x, y0, inwardSide(foreSector0), 0, nullptr, true);
			registerExtensionControl(createdControls[CORE_LEVEL_LOW]);

			createdControls[CORE_LEVEL_HIGH] = _createLadderButton(
				foreSector1, x, y1, inwardSide(foreSector1), 0, nullptr, true);
			registerExtensionControl(createdControls[CORE_LEVEL_HIGH]);
		}

		CreateLadderResult result{
			{ ~0u, SectorObjectType::Ladder, ladderSector },
			{ createdControls[0], createdControls[1] },
			traversalResource
		};
		ConstructionRecord record{ ConstructionType::Ladder };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.decksHigh; record.d = options.directionalBatchLimit;
		record.p = options.extensible; record.q = options.startExtended;
		recordConstruction(std::move(record));
		return result;
	}

	bool Building::canAddStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t decksHigh,
		string* diagnostic) const
	{
		auto reject = [&](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (diagnostic) diagnostic->clear();
		if (layerIndex == 0 || layerIndex >= getLayerCount())
			return reject("A Stairwell must sit on a Layer that has a Layer in front of it to land on");
		if (decksHigh < 2) return reject("A Stairwell must span at least two decks");
		if (x >= mCellsWide || y >= mDecksHigh || x + 2 > mCellsWide
			|| y + decksHigh > mDecksHigh)
			return reject("The Stairwell is outside the Building bounds");
		auto const& transitLayer = mLayers[layerIndex];
		auto const& landing = mLayers[layerInFront(layerIndex)];
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			auto const& first = landing->getCellDefinition(x, iy);
			if (first.sectorIndex == ~0u)
				return reject(format("A landing Location is required at {},{}", x, iy));
			auto sector = mSectors[first.sectorIndex];
			// A Stairwell lands on any location-like Sector, a Facade included
			// (ADR 0003, ticket #52).
			if (!sector || !isLocationLike(sector->getType()))
				return reject(format("A landing Location is required at {},{}", x, iy));
			for (uint32_t ix = x; ix < x + 2; ++ix)
			{
				auto const& fore = landing->getCellDefinition(ix, iy);
				if (fore.sectorIndex != first.sectorIndex)
					return reject(format("The Stairwell spans different landing Locations at deck {}", iy));
				if (!fore.isTraversableOnFoot())
					return reject(format("The landing floor at {},{} is not traversable", ix, iy));
				auto const occupant = transitLayer->getCellDefinition(ix, iy).sectorIndex;
				if (occupant != ~0u)
					return reject(format("A Sector at {},{} blocks the Stairwell", ix, iy));
			}
		}
		return true;
	}

	uint32_t Building::addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t decksHigh, int mountSide)
	{
		beginStructuralEdit("addStairwell");
		return addStairwell(layerIndex, y, x, CreateStairwellOptions{ decksHigh, mountSide }).sectorIndex;
	}

	Building::CreateStairwellResult Building::addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateStairwellOptions const& options)
	{
		beginStructuralEdit("addStairwell");
		auto decksHigh = options.decksHigh;
		auto mountSide = options.mountSide;
		ASSERT_SIDE_OK(mountSide);

		// A Stairwell Transit sits on layerIndex and lands on the Layer directly in front.
		auto const landingLayer = layerInFront(layerIndex);
		auto transitLayer = getLayer(layerIndex);
		const uint32_t cellsWide = 2;

		// Checks
		string caller = format("Building::addStairwell({}, {}, {}, {}, {})", layerIndex, y, x, decksHigh, mountSide);

		validateLayer(caller, layerIndex);
		if (isFrontMostLayer(layerIndex))
		{
			throw BuildingException(this, format("{} - a Stairwell cannot be placed on the front-most Layer, because it has no Layer in front to land on", caller));
		}

		if (decksHigh < 2)
		{
			throw BuildingException(this, format("{} - Stairwell at {},{} must be at least 2 decks high", caller, x, y));
		}

		validateBounds(caller, x, y, cellsWide, decksHigh);
		validateLayerSpace(caller, layerIndex, x, y, cellsWide, decksHigh);

		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			auto const& cellDef0 = mLayers[landingLayer]->getCellDefinition(x, iy);
			auto deckSectorIndex = cellDef0.sectorIndex;

			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cellDef = mLayers[landingLayer]->getCellDefinition(ix, iy);
				auto foreSectorIndex = cellDef.sectorIndex;

				// Make sure the landing cells have a Sector, and that the horizontal Sectors are not different:
				// Stairwells cannot span different Sectors horizontally, due to placement of the door leading to them.
				if (foreSectorIndex != deckSectorIndex)
				{
					throw BuildingException(this, format("{} - the stairwell horizontally spans different landing Sectors between {},{} and {},{}, which is not allowed", caller, x, iy, x + 1, iy));
				}

				// Landing Sector can't be empty
				if (foreSectorIndex == ~0u)
				{
					throw BuildingException(this, format("{} - landing cell at {},{} is not occupied, which blocks stairwell being placed", caller, ix, iy));
				}

				// Stairwells can only connect location-like Sectors: Rooms,
				// Corridors, and Facades (ADR 0003, ticket #52).
				auto const& foreSector = getSector(foreSectorIndex);

				if (!isLocationLike(foreSector->getType()))
				{
					throw BuildingException(this, format("{} - landing cell at {},{} is not a Location, which blocks stairwell being placed", caller, ix, iy));
				}

				// Stairwells must not be in the air
				validateCellTraversableOnFoot(caller, "Stairwell", landingLayer, ix, iy);
			}
		}

		if (options.directionalCapacity > 0 && options.directionalBatchLimit == 0)
		{
			throw BuildingException(this, format("{} - Narrow stairwell directional batch limit must be positive.", caller));
		}

		// Create stairwell
		auto sectorIndex = createStairwell(layerIndex, x, y, decksHigh, mountSide);

		// Set layers
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = transitLayer->getCellDefinition(ix, iy);

				cellDef.sectorIndex = sectorIndex;
			}
		}

		TraversalResourceId traversalResource;
		if (options.directionalCapacity > 0)
		{
			auto stairwellTransit = dynamic_pointer_cast<StairwellTransit>(_getSector(sectorIndex));
			auto stairwell = stairwellTransit->getStairwell();
			traversalResource = createStairwellTraversalResource("Narrow stairwell capacity",
				stairwell, SectorId{ (uint64_t)sectorIndex + 1 }, options.directionalCapacity,
				options.directionalBatchLimit);
			stairwell->configureTraversal(traversalResource);
		}

		ConstructionRecord record{ ConstructionType::Stairwell };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.decksHigh;
		record.i = options.mountSide; record.d = options.directionalCapacity; record.e = options.directionalBatchLimit;
		recordConstruction(std::move(record));
		return { sectorIndex, traversalResource };
	}

	bool Building::validateStaircaseEndpoint(uint32_t layerIndex, uint32_t x, uint32_t y, bool upperEndpoint,
		int /*riseSide*/, string& diagnostic) const
	{
		auto const& cell = mLayers[layerIndex]->getCellDefinition(x, y);
		if (!cell.occupied())
		{
			diagnostic = format("A landing Location is required at {},{}", x, y);
			return false;
		}
		auto location = dynamic_pointer_cast<const Location>(mSectors[cell.sectorIndex]);
		if (!location)
		{
			diagnostic = format("A landing Location is required at {},{}", x, y);
			return false;
		}
		// Lower landings and Corridor landings require ordinary walkable floor.
		// An upper Room landing may instead terminate at an open side wall whose
		// adjacent Location supplies the walkable landing floor.
		if (!upperEndpoint || location->isCorridor())
		{
			if (cell.isTraversableOnFoot()) return true;
			diagnostic = format("Traversable landing floor is required at {},{}", x, y);
			return false;
		}

		auto const roomDeck = y - location->getCellY();
		auto openLandingAt = [&](int wallSide)
		{
			uint32_t const boundaryX = wallSide == CORE_SIDE_LEFT
				? location->getCellX0() : location->getCellX1();
			if (x != boundaryX || location->getEndType(roomDeck, wallSide) != SectorEndType::None)
				return false;
			int const adjacentX = wallSide == CORE_SIDE_LEFT ? (int)x - 1 : (int)x + 1;
			if (adjacentX < 0 || adjacentX >= (int)mCellsWide) return false;
			auto const& adjacent = mLayers[layerIndex]
				->getCellDefinition((uint32_t)adjacentX, y);
			if (!adjacent.occupied() || !adjacent.isTraversableOnFoot()) return false;
			auto adjacentLocation = dynamic_pointer_cast<const Location>(mSectors[adjacent.sectorIndex]);
			if (!adjacentLocation || y < adjacentLocation->getCellY()) return false;
			auto const adjacentDeck = y - adjacentLocation->getCellY();
			return adjacentDeck < adjacentLocation->getDecksHigh()
				&& adjacentLocation->getEndType(adjacentDeck, 1 - wallSide) == SectorEndType::None;
		};
		if (openLandingAt(CORE_SIDE_LEFT) || openLandingAt(CORE_SIDE_RIGHT)) return true;
		diagnostic = format("The upper Staircase endpoint at {},{} must meet an open Room wall with adjacent traversable floor", x, y);
		return false;
	}

	bool Building::canAddStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		int riseSide, string* diagnostic) const
	{
		auto reject = [&](string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (diagnostic) diagnostic->clear();
		if (layerIndex == 0 || layerIndex >= getLayerCount())
			return reject("A Staircase must sit on a Layer that has a Layer in front of it to land on");
		if (riseSide != CORE_SIDE_LEFT && riseSide != CORE_SIDE_RIGHT)
			return reject("The Staircase rise direction is invalid");
		if (cellsWide < 2) return reject("A Staircase must be at least two cells wide");
		if (x >= mCellsWide || y >= mDecksHigh || cellsWide > mCellsWide - x || y + 1 >= mDecksHigh)
			return reject("The Staircase is outside the Building bounds");
		for (uint32_t iy = y; iy <= y + 1; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
				if (mLayers[layerIndex]->getCellDefinition(ix, iy).occupied())
					return reject(format("A Sector at {},{} blocks the Staircase", ix, iy));

		auto const landingLayer = layerInFront(layerIndex);
		uint32_t const lowerX = riseSide == CORE_SIDE_RIGHT ? x : x + cellsWide - 1;
		uint32_t const upperX = riseSide == CORE_SIDE_RIGHT ? x + cellsWide - 1 : x;
		string endpointDiagnostic;
		if (!validateStaircaseEndpoint(landingLayer, lowerX, y, false, riseSide, endpointDiagnostic)
			|| !validateStaircaseEndpoint(landingLayer, upperX, y + 1, true, riseSide, endpointDiagnostic))
			return reject(std::move(endpointDiagnostic));
		return true;
	}

	uint32_t Building::addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		int riseSide, float speed)
	{
		return addStaircase(layerIndex, y, x, CreateStaircaseOptions{ cellsWide, riseSide, speed });
	}

	uint32_t Building::addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, CreateStaircaseOptions const& options)
	{
		beginStructuralEdit("addStaircase");
		string diagnostic;
		if (!isfinite(options.speed))
			throw BuildingException(this, "A Staircase speed must be finite");
		if (!canAddStaircase(layerIndex, y, x, options.cellsWide, options.riseSide, &diagnostic))
			throw BuildingException(this, format("Building::addStaircase({}, {}, {}, {}) - {}", layerIndex, y, x, options.cellsWide, diagnostic));
		auto sectorIndex = createStaircase(layerIndex, x, y, options.cellsWide, options.riseSide, options.speed);
		auto transitLayer = getLayer(layerIndex);
		for (uint32_t iy = y; iy <= y + 1; ++iy)
			for (uint32_t ix = x; ix < x + options.cellsWide; ++ix)
				transitLayer->getCellDefinition(ix, iy).sectorIndex = sectorIndex;
		ConstructionRecord record{ ConstructionType::Staircase };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.cellsWide; record.i = options.riseSide;
		record.x = options.speed;
		recordConstruction(std::move(record));
		return sectorIndex;
	}

	std::vector<Building::LiftLandingRow> Building::getLiftLandingRows(uint32_t layerIndex,
		uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh) const
	{
		vector<LiftLandingRow> rows;
		if (layerIndex >= mLayers.size() || cellsWide == 0 || decksHigh == 0)
			return rows;
		// A Transit lands on the Layer directly in front of the Layer it occupies, so
		// the front-most Layer - which has nothing in front of it - has no landings.
		if (isFrontMostLayer(layerIndex)) return rows;
		if (x >= mCellsWide || y >= mDecksHigh || cellsWide > mCellsWide - x
			|| decksHigh > mDecksHigh - y) return rows;

		auto const& landing = mLayers[layerInFront(layerIndex)];
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			LiftLandingRow row;
			row.offset = iy - y;
			auto const& firstCell = landing->getCellDefinition(x, iy);
			if (firstCell.sectorIndex != ~0u)
				row.location = dynamic_pointer_cast<const Location>(mSectors[firstCell.sectorIndex]);
			if (!row.location)
			{
				rows.push_back(row);
				continue;
			}
			bool complete = true;
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cell = landing->getCellDefinition(ix, iy);
				complete = complete && cell.sectorIndex == firstCell.sectorIndex
					&& cell.isTraversableOnFoot();
				row.obstructed = row.obstructed || cell.hasObject() || !cell.markers.empty();
			}
			row.fullyOverlapping = complete;
			if (row.fullyOverlapping)
				row.callButtonSpace = !(x == row.location->getCellX0()
					&& x + cellsWide - 1 == row.location->getCellX1());
			rows.push_back(row);
		}
		return rows;
	}

	Building::CreateLiftResult Building::addLift(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t decksHigh)
	{
		CreateLiftOptions options;
		options.cellsWide = cellsWide;
		options.decksHigh = decksHigh;
		if (cellsWide == 0 || cellsWide > 2 || decksHigh == 0)
			throw BuildingException(this, "Editor lifts must be one or two cells wide and at least one deck high");
		validateLayer(format("Building::addLift({}, ...)", layerIndex), layerIndex);
		if (isFrontMostLayer(layerIndex))
			throw BuildingException(this, "A Lift cannot be placed on the front-most Layer, because it has no Layer in front to land on");
		for (auto const& row : getLiftLandingRows(layerIndex, y, x, cellsWide, decksHigh))
		{
			if (!row.fullyOverlapping) continue;
			if (row.obstructed)
				throw BuildingException(this, format("An object blocks the Lift landing at floor {}", y + row.offset));
			options.stopOffsets.push_back(row.offset);
		}
		return addLift(layerIndex, y, x, options);
	}

	Building::CreateLiftResult Building::addLift(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLiftOptions const& options)
	{
		beginStructuralEdit("addLift");
		// The Lift Transit occupies layerIndex; its landings are the fore Layer of the
		// pair it forms, which is the Layer directly in front.
		validateLayer(format("Building::addLift({}, ...)", layerIndex), layerIndex);
		if (isFrontMostLayer(layerIndex))
			throw BuildingException(this, "A Lift cannot be placed on the front-most Layer, because it has no Layer in front to land on");
		auto foreLayer = getLayer(layerInFront(layerIndex));
		auto backLayer = getLayer(layerIndex);

		// Checks
		string caller = format("Building::addLift({}, {}, {}, {}, <stopOffsts>)", layerIndex, y, x, options.cellsWide);

		validateLiftOptions(caller, options);
		if (options.cellsWide > 2)
			throw BuildingException(this, format("{} - enclosed Lift width must be one or two cells.", caller));

		auto decksHigh = options.decksHigh ? options.decksHigh : options.stopOffsets.back() + 1;
		if (options.stopOffsets.back() >= decksHigh)
			throw BuildingException(this, format("{} - Lift stop is outside the shaft bounds.", caller));

		validateBounds(caller, x, y, options.cellsWide, decksHigh);
		validateLayerSpace(caller, layerIndex, x, y, options.cellsWide, decksHigh);

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

				// Enclosed lifts connect fully overlapping Fore-layer Locations.
				auto const& foreSector = getSector(foreSectorIndex);
				auto location = dynamic_pointer_cast<const Location>(foreSector);

				if (!location)
				{
					throw BuildingException(this, format("{} - foreground cell at {},{} is not a Location, which blocks lift being placed", caller, ix, iy));
				}

				// Lifts must not be in the air and every intersecting landing must be clear.
				validateCellTraversableOnFoot(caller, "Lift", layerInFront(layerIndex), ix, iy);
				if (cellDef.hasObject() || !cellDef.markers.empty())
					throw BuildingException(this, format("{} - an object blocks the Lift landing at {},{}", caller, ix, iy));
			}
			if (x == getSector(deckSectorIndex)->getCellX0()
				&& x + options.cellsWide - 1 == getSector(deckSectorIndex)->getCellX1())
				throw BuildingException(this, format("{} - there is no space for a Lift call button at floor {}", caller, iy));
		}

		// Create lift
		auto liftObject = createLift(layerIndex, x, y, options.cellsWide, decksHigh, options.stopOffsets);

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

		// Landing controls are physical InteractionPoints; the lift coordinator owns
		// scheduling, door interlocks, and operation completion.
		for (size_t stopIndex = 0; stopIndex < options.stopOffsets.size(); ++stopIndex)
		{
			// A Lift entrance reads as a centre-opening pair, so its generated Doors
			// are authored OpenApart unless the Lift's record carries a per-stop
			// override.  Shuttle-owned Doors keep OpenUp.
			CreateDoorOptions stopDoorOptions;
			stopDoorOptions.width = options.cellsWide;
			stopDoorOptions.controls[0] = true;
			stopDoorOptions.activationMode = DoorActivationMode::Unavailable;
			stopDoorOptions.openStyle = Door::OpenStyle::OpenApart;
			if (stopIndex < options.stopDoorOpenStyles.size()
				&& options.stopDoorOpenStyles[stopIndex] != ~0u)
				stopDoorOptions.openStyle =
					static_cast<Door::OpenStyle>(options.stopDoorOpenStyles[stopIndex]);
			auto doorRes = _addSectorDoor(layerInFront(layerIndex), y + options.stopOffsets[stopIndex],
				x, stopDoorOptions, true);
			liftRes.doors.push_back(doorRes);
		}

		// The replacement lift coordinator owns the car manifest and schedule. Each
		// landing remains a distinct threshold resource, linked to this coordinator.
		vector<LiftStop> liftStops;
		for (uint32_t i = 0; i < options.stopOffsets.size(); ++i)
		{
			auto location = getSector(foreLayer->getCellDefinition(x, y + options.stopOffsets[i]).sectorIndex);
			liftStops.push_back({ SectorId{ (uint64_t)location->getIndex() + 1 },
				(float)(y + options.stopOffsets[i]), liftRes.doors[i].traversalResource, {} });
		}
		auto coordinator = createLiftTraversalResource("Lift journey", lift,
			SectorId{ (uint64_t)liftTransit->getIndex() + 1 }, liftStops, options.capacity,
			options.minimumDwellSeconds, options.maximumBoardingSeconds);
		lift->configureTraversal(coordinator);
		liftRes.traversalResource = coordinator;
		auto liftResource = mTraversalResources.find(coordinator);
		liftResource->mLiftCurrentStop = options.initialStop;
		liftResource->mLiftPosition = liftStops[options.initialStop].globalPosition;
		lift->setCoordinatedPosition(liftResource->mLiftPosition);
		// Standing positions are deterministic and local to the moving car.  Validation
		// above guarantees agent-safe horizontal separation for every declared slot.
		auto standingWidth = CORE_AGENT_MAX_WIDTH * options.capacity;
		auto standingStart = x + (options.cellsWide - standingWidth) * 0.5f
			+ CORE_AGENT_MAX_WIDTH * 0.5f - liftTransit->getPosition().x;
		for (uint32_t i = 0; i < options.capacity; ++i)
			liftResource->mCapacityPositions[i] = {
				standingStart + CORE_AGENT_MAX_WIDTH * i, 0.0f };
		for (uint32_t i = 0; i < liftRes.doors.size(); ++i)
		{
			auto landing = mTraversalResources.find(liftRes.doors[i].traversalResource);
			landing->mLiftCoordinator = coordinator;
			landing->mLiftStopIndex = i;
			// The car-side lane represents standing capacity, not corridor waiting
			// space. Give it exactly one spot per passenger position.
			for (auto& lane : landing->mQueueLanes)
			{
				if (lane.sector != liftResource->mLiftSector) continue;
				lane.positions.clear();
				for (auto const& position : liftResource->mCapacityPositions)
					lane.positions.push_back({ liftTransit->getPosition().x + position.x,
						liftStops[i].globalPosition + position.y });
				lane.positionOwners.assign(lane.positions.size(), {});
				if (!lane.positions.empty())
				{
					lane.origin = lane.positions.front();
					lane.direction = Vector2::UNIT_X;
					lane.extent = lane.positions.back().x - lane.positions.front().x;
				}
			}

			auto& control = liftRes.doors[i].controls[0];
			DeviceCommand call;
			call.type = DeviceCommandType::CallLift;
			call.traversalResource = coordinator;
			call.stopIndex = i;
			auto point = createPhysicalControlInteractionPoint("Lift landing call", control,
				(float)(y + options.stopOffsets[i]), 0.15f, getFixedTimestep(),
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

		ConstructionRecord record{ ConstructionType::Lift };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.cellsWide; record.d = options.capacity;
		record.e = decksHigh; record.g = options.initialStop;
		record.x = options.minimumDwellSeconds; record.y = options.maximumBoardingSeconds;
		record.values = options.stopOffsets;
		// The per-stop Door styles are authored Lift data, not just initial Door
		// state: record them so save/load and later rebuilds replay them, the
		// way the Shuttle path records its doorOpenStyles.
		record.overrides = options.stopDoorOpenStyles;
		recordConstruction(std::move(record));
		return liftRes;
	}

	Building::CreateShuttleResult Building::addShuttle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, CreateShuttleOptions const& options)
	{
		beginStructuralEdit("addShuttle");
		// The Shuttle Transit occupies layerIndex; its landings are the fore Layer of
		// the pair it forms, which is the Layer directly in front.
		validateLayer(format("Building::addShuttle({}, ...)", layerIndex), layerIndex);
		if (isFrontMostLayer(layerIndex))
			throw BuildingException(this, "A Shuttle cannot be placed on the front-most Layer, because it has no Layer in front to land on");
		auto foreLayer = getLayer(layerInFront(layerIndex));
		auto backLayer = getLayer(layerIndex);

		// Checks
		string caller = format("Building::addShuttle({}, {}, {}, {}, <options>)", layerIndex, y, x, cellsWide);

		// Bear in mind that we may have to allow extra space on the transit layer beyond the x/x+cellsWide
		// extents.  Eg, for where a=x and b=x+cellsWide, and s=stop offsets:
		//       a        b
		//        s      s 
		// BACK: XDX-------
		// FORE: -D-....-D-


		validateShuttleOptions(caller, options);
		validateBounds(caller, x, y, cellsWide, 1);
		validateLayerSpace(caller, layerIndex, x, y, cellsWide, 1);

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

			// A stop remains usable when at least one configured carriage door has
			// a Location landing. Partial mode omits unsupported individual doors.
			bool hasLanding = false;
			auto doorOffsets = SimulationCoordinator::shuttleDoorOffsets(options.carWidth, options.doorMask);
			for (uint32_t car = 0; car < options.numCars; ++car)
				for (uint32_t door = 0; door < doorOffsets.size(); ++door)
				{
					uint32_t cx = ix + car * (options.carWidth + 1) + doorOffsets[door];
					auto const& cell = foreLayer->getCellDefinition(cx, y);
					// A carriage door lands on any location-like Sector: a Room, a
					// Corridor, or a Facade (ADR 0003, ticket #52).
					bool supported = cell.sectorIndex != ~0u
						&& isLocationLike(getSector(cell.sectorIndex)->getType());
					if (!supported && !options.allowPartialLandings)
						throw BuildingException(this, format("{} - door {} of carriage {} at stop offset {} has no supported landing",
							caller, door, car, options.stopOffsets[i]));
					hasLanding = hasLanding || supported;
				}
			if (!hasLanding)
				throw BuildingException(this, format("{} - stop offset {} has no supported carriage landing", caller, options.stopOffsets[i]));
		}

		// Create shuttle
		auto shuttleObject = createShuttle(layerIndex, x, y, cellsWide, options.numCars, options.carWidth, options.stopOffsets);

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

		// Create landing thresholds and physical InteractionPoints. Keep a fixed
		// stop/carriage/door result grid so absent partial landings remain explicit.
		auto doorOffsets = SimulationCoordinator::shuttleDoorOffsets(options.carWidth, options.doorMask);
		auto doorResultIndex = [&](uint32_t stop, uint32_t car, uint32_t door)
		{
			return (stop * options.numCars + car) * doorOffsets.size() + door;
		};
		shuttleRes.doors.resize(options.stopOffsets.size() * options.numCars * doorOffsets.size());
		for (uint32_t stop = 0; stop < options.stopOffsets.size(); ++stop)
			for (uint32_t car = 0; car < options.numCars; ++car)
				for (uint32_t door = 0; door < doorOffsets.size(); ++door)
				{
					auto globalX = x + options.stopOffsets[stop]
						+ car * (options.carWidth + 1) + doorOffsets[door];
					auto const& cell = foreLayer->getCellDefinition(globalX, y);
					// Same rule as the validation pass above: only a location-like
					// landing is supported, a Facade included. A Background behind a
					// carriage door is omitted, never built into a threshold that
					// touches it.
					bool supported = cell.sectorIndex != ~0u
						&& isLocationLike(getSector(cell.sectorIndex)->getType());
					if (supported)
					{
						// A Shuttle landing reads as a hatch, so its generated Doors
						// are authored OpenUp unless the Shuttle's record carries a
						// per-Door override on this grid slot.
						CreateDoorOptions stopDoorOptions;
						stopDoorOptions.width = 1;
						stopDoorOptions.controls[0] = true;
						stopDoorOptions.activationMode = DoorActivationMode::Unavailable;
						auto const slot = doorResultIndex(stop, car, door);
						if (slot < options.doorOpenStyles.size()
							&& options.doorOpenStyles[slot] != ~0u)
							stopDoorOptions.openStyle =
								static_cast<Door::OpenStyle>(options.doorOpenStyles[slot]);
						shuttleRes.doors[slot] = _addSectorDoor(layerInFront(layerIndex),
							y, globalX, stopDoorOptions, true);
					}
				}

		vector<LiftStop> stops;
		for (uint32_t i = 0; i < options.stopOffsets.size(); ++i)
		{
			uint32_t doorIndex = i * options.numCars * (uint32_t)doorOffsets.size();
			auto stopEnd = (i + 1) * options.numCars * (uint32_t)doorOffsets.size();
			while (doorIndex < stopEnd && !shuttleRes.doors[doorIndex].traversalResource) ++doorIndex;
			assert(doorIndex < stopEnd);
			auto location = shuttleRes.doors[doorIndex].door.sector;
			stops.push_back({ SectorId{ (uint64_t)location->getIndex() + 1 },
				(float)(x + options.stopOffsets[i]), shuttleRes.doors[doorIndex].traversalResource, {} });
		}
		auto coordinator = createShuttleTraversalResource("Shuttle journey", shuttle,
			SectorId{ (uint64_t)shuttleTransit->getIndex() + 1 }, stops, options.capacity,
			options.minimumDwellSeconds, options.maximumBoardingSeconds);
		shuttle->configureTraversal(coordinator);
		shuttleRes.traversalResource = coordinator;
		auto shuttleResource = mTraversalResources.find(coordinator);
		shuttleResource->mLiftCurrentStop = options.initialStop;
		shuttleResource->mLiftPosition = stops[options.initialStop].globalPosition;
		shuttle->setCoordinatedPosition(shuttleResource->mLiftPosition);

		// A Location sector is one connected platform access zone. Doors opening
		// onto the same sector share a logical queue; different sectors do not.
		for (uint32_t stop = 0; stop < stops.size(); ++stop)
		{
			map<SectorId, uint32_t> accessZones;
			for (uint32_t carriage = 0; carriage < options.numCars; ++carriage)
				for (uint32_t door = 0; door < doorOffsets.size(); ++door)
			{
				auto doorIndex = doorResultIndex(stop, carriage, door);
				auto& doorResult = shuttleRes.doors[doorIndex];
				if (!doorResult.traversalResource) continue;
				auto landing = mTraversalResources.find(doorResult.traversalResource);
				auto doorX = x + options.stopOffsets[stop]
					+ carriage * (options.carWidth + 1) + doorOffsets[door];
				auto location = getSector(foreLayer->getCellDefinition(doorX, y).sectorIndex);
				auto locationId = SectorId{ (uint64_t)location->getIndex() + 1 };
				auto [zone, inserted] = accessZones.emplace(locationId, (uint32_t)accessZones.size());
				(void)inserted;
				landing->mLiftCoordinator = coordinator;
				landing->mLiftStopIndex = stop;
				shuttleResource->mShuttleDoors.push_back({ stop, carriage, zone->second,
					locationId, doorResult.traversalResource });
				auto& carriageState = shuttleResource->mShuttleCarriages[carriage];
				carriageState.stopDoors[stop].push_back(doorResult.traversalResource);
				// Car-side Door spots mirror this carriage's declared capacity. They
				// remain independent from the platform-side queue, even where the two
				// layers' spots overlap visually.
				for (auto& lane : landing->mQueueLanes)
				{
					if (lane.sector != shuttleResource->mLiftSector) continue;
					lane.positions.clear();
					for (uint32_t position = 0; position < carriageState.capacity; ++position)
					{
						auto capacityIndex = carriageState.firstCapacityPosition + position;
						auto const& local = shuttleResource->mCapacityPositions[capacityIndex];
						lane.positions.push_back({ stops[stop].globalPosition + local.x,
							(float)y + local.y });
					}
					lane.positionOwners.assign(lane.positions.size(), {});
					if (!lane.positions.empty())
					{
						lane.origin = lane.positions.front();
						lane.direction = Vector2::UNIT_X;
						lane.extent = lane.positions.back().x - lane.positions.front().x;
					}
				}

				auto& control = doorResult.controls[0];
				DeviceCommand call;
				call.type = DeviceCommandType::CallShuttle;
				call.traversalResource = coordinator;
				call.stopIndex = stop;
				auto point = createPhysicalControlInteractionPoint("Shuttle landing call", control,
					(float)y, 0.15f, getFixedTimestep(),
					{ { call, InteractionBindingRequirement::Required } });
				landing->mControls.push_back(point);
				if (!shuttleResource->mLiftStops[stop].callControl)
					shuttleResource->mLiftStops[stop].callControl = point;
			}
		}

		// Destination selection is serialized vehicle-wide. The interaction point
		// is moved to the acting passenger, so passengers retain carriage positions.
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			DeviceCommand select;
			select.type = DeviceCommandType::SelectShuttleDestination;
			select.traversalResource = coordinator;
			select.stopIndex = i;
			auto selector = createInteractionPoint("Shuttle destination selector",
				shuttleResource->mLiftSector,
				{ shuttleResource->mLiftPosition + options.carWidth * 0.5f, (float)y },
				0.25f, getFixedTimestep(), { { select, InteractionBindingRequirement::Required } });
			shuttleResource->mControls.push_back(selector);
			if (i == 0) shuttleRes.interiorSelector = selector;
		}
		shuttleResource->mLiftSelector = shuttleRes.interiorSelector;

		ConstructionRecord record{ ConstructionType::Shuttle };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = options.numCars;
		record.e = options.carWidth; record.f = options.initialStop; record.g = options.capacity;
		record.h = options.doorMask; record.p = options.allowPartialLandings;
		record.x = options.minimumDwellSeconds; record.y = options.maximumBoardingSeconds;
		record.values = options.stopOffsets;
		record.overrides = options.doorOpenStyles;
		recordConstruction(std::move(record));
		return shuttleRes;
	}

	bool Building::canRemoveLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side,
		string* diagnostic) const
	{
		auto reject = [&](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT)
			return reject("A Location wall side must be left or right");
		if (sectorIndex >= mSectors.size()) return reject("The Location does not exist");
		auto sector = mSectors[sectorIndex];
		if (sector->getType() == SectorType::Facade)
			return reject("A Facade has no walls: its perimeter is open by construction");
		if (sector->getType() != SectorType::Location)
			return reject("Walls can only be edited on Rooms and Corridors");
		if (deckIndex >= sector->getDecksHigh()) return reject("The Location deck does not exist");

		auto const globalY = sector->getCellY() + deckIndex;
		int const neighbourX = side == CORE_SIDE_LEFT
			? (int)sector->getCellX() - 1
			: (int)sector->getCellX() + (int)sector->getCellsWide();
		if (neighbourX < 0 || neighbourX >= (int)getCellsWide())
			return reject("The wall is on the outside of the Building");
		auto const& neighbourCell = mLayers[sector->getLayerIndex()]
			->getCellDefinition((uint32_t)neighbourX, globalY);
		if (neighbourCell.sectorIndex == ~0u || neighbourCell.sectorIndex == sectorIndex)
			return reject("No adjacent Room or Corridor shares this wall");
		auto neighbour = mSectors[neighbourCell.sectorIndex];
		// A Facade may share a boundary with a Room whose wall is being opened,
		// and vice versa: the Facade's half is open by construction, and
		// refusing the Room's half would let a Facade merge outward while its
		// neighbour could not merge inward.  The shared location-like rule
		// (ticket #45) admits both here; a Facade itself never has a wall to
		// remove, so the sector-side type check above still refuses it.
		if (!isLocationLike(neighbour->getType()))
			return reject("The adjacent sector is not a Room, Corridor or Facade");
		if (globalY < neighbour->getCellY()
			|| globalY >= neighbour->getCellY() + neighbour->getDecksHigh())
			return reject("The adjacent Location does not occupy this deck");
		auto const neighbourDeck = globalY - neighbour->getCellY();
		if (neighbour->getType() == SectorType::Facade)
		{
			// The Facade's half of the boundary is open by construction; only
			// the Room's own wall stands in the way.
			if (neighbour->getEndType(neighbourDeck, 1 - side) != SectorEndType::None)
				return reject("The Facade side of the boundary is not open");
		}
		else if (neighbour->getEndType(neighbourDeck, 1 - side) != SectorEndType::Wall)
			return reject("The shared boundary is not a pair of walls");
		if (sector->getEndType(deckIndex, side) != SectorEndType::Wall)
			return reject("The shared boundary is not a pair of walls");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool Building::canAddLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side,
		string* diagnostic) const
	{
		auto reject = [&](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT)
			return reject("A Location wall side must be left or right");
		if (sectorIndex >= mSectors.size()) return reject("The Location does not exist");
		auto sector = mSectors[sectorIndex];
		if (sector->getType() == SectorType::Facade)
			return reject("A Facade has no walls: its perimeter is open by construction");
		if (sector->getType() != SectorType::Location)
			return reject("Walls can only be edited on Rooms and Corridors");
		if (deckIndex >= sector->getDecksHigh()) return reject("The Location deck does not exist");

		auto const globalY = sector->getCellY() + deckIndex;
		int const neighbourX = side == CORE_SIDE_LEFT
			? (int)sector->getCellX() - 1
			: (int)sector->getCellX() + (int)sector->getCellsWide();
		if (neighbourX < 0 || neighbourX >= (int)getCellsWide())
			return reject("The wall is on the outside of the Building");
		auto const& neighbourCell = mLayers[sector->getLayerIndex()]
			->getCellDefinition((uint32_t)neighbourX, globalY);
		if (neighbourCell.sectorIndex == ~0u || neighbourCell.sectorIndex == sectorIndex)
			return reject("No adjacent Room or Corridor shares this opening");
		auto neighbour = mSectors[neighbourCell.sectorIndex];
		if (neighbour->getType() != SectorType::Location)
			return reject("The adjacent sector is not a Room or Corridor");
		if (globalY < neighbour->getCellY()
			|| globalY >= neighbour->getCellY() + neighbour->getDecksHigh())
			return reject("The adjacent Location does not occupy this deck");
		auto const neighbourDeck = globalY - neighbour->getCellY();
		if (sector->getEndType(deckIndex, side) != SectorEndType::None
			|| neighbour->getEndType(neighbourDeck, 1 - side) != SectorEndType::None)
			return reject("The shared boundary is not an open wall");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	void Building::removeLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side)
	{
		string diagnostic;
		if (!canRemoveLocationWall(sectorIndex, deckIndex, side, &diagnostic))
			throw BuildingException(this, "Building::removeLocationWall - " + diagnostic);
		beginStructuralEdit("removeLocationWall");

		auto sector = _getSector(sectorIndex);
		auto const globalY = sector->getCellY() + deckIndex;
		auto const neighbourX = side == CORE_SIDE_LEFT
			? sector->getCellX() - 1
			: sector->getCellX() + sector->getCellsWide();
		auto const neighbourIndex = mLayers[sector->getLayerIndex()]
			->getCellDefinition(neighbourX, globalY).sectorIndex;
		auto neighbour = _getSector(neighbourIndex);
		auto const neighbourDeck = globalY - neighbour->getCellY();
		sector->removeEndWall(deckIndex, side);
		neighbour->removeEndWall(neighbourDeck, 1 - side);

		ConstructionRecord record{ ConstructionType::RemoveWall };
		record.a = sectorIndex; record.b = deckIndex; record.i = side;
		recordConstruction(std::move(record));
	}

	void Building::addLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side)
	{
		string diagnostic;
		if (!canAddLocationWall(sectorIndex, deckIndex, side, &diagnostic))
			throw BuildingException(this, "Building::addLocationWall - " + diagnostic);
		beginStructuralEdit("addLocationWall");

		auto sector = _getSector(sectorIndex);
		auto const globalY = sector->getCellY() + deckIndex;
		auto const boundaryX = side == CORE_SIDE_LEFT
			? sector->getCellX() : sector->getCellX() + sector->getCellsWide();
		auto const neighbourX = side == CORE_SIDE_LEFT
			? sector->getCellX() - 1
			: sector->getCellX() + sector->getCellsWide();
		auto const neighbourIndex = mLayers[sector->getLayerIndex()]
			->getCellDefinition(neighbourX, globalY).sectorIndex;
		auto neighbour = _getSector(neighbourIndex);
		auto const neighbourDeck = globalY - neighbour->getCellY();
		sector->addEndWall(deckIndex, side);
		neighbour->addEndWall(neighbourDeck, 1 - side);

		// An open wall is persisted as a RemoveWall command. Restoring the wall
		// removes either side's command for this same physical boundary.
		mConstructionRecords.erase(remove_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				if (record.type != ConstructionType::RemoveWall
					|| record.a >= mSectors.size()) return false;
				auto commandSector = mSectors[record.a];
				if (record.b >= commandSector->getDecksHigh()
					|| commandSector->getLayerIndex() != sector->getLayerIndex()) return false;
				auto const commandX = record.i == CORE_SIDE_LEFT
					? commandSector->getCellX()
					: commandSector->getCellX() + commandSector->getCellsWide();
				auto const commandY = commandSector->getCellY() + record.b;
				return commandX == boundaryX && commandY == globalY;
			}), mConstructionRecords.end());
	}

	Building::CreateObjectResult Building::_createSectorButton(string const& name, shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t flags, uint32_t* index)
	{
		auto side = CORE_SIDE_MIDDLE;
		uint32_t buttonX = sector->getCellX() + x;

		auto const& obj = createPhysicalControl(name, sector->getLayerIndex(), buttonX, y, side, flags);

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
		uint32_t alternateX = ~0u;
		int alternateSide = -1;
		if (side == CORE_SIDE_RIGHT && x > sector->getCellX0())
		{
			alternateX = x;
			alternateSide = CORE_SIDE_LEFT;
		}
		else if (side == CORE_SIDE_LEFT && x + cellsWide - 1 < sector->getCellX1())
		{
			alternateX = x + cellsWide - 1;
			alternateSide = CORE_SIDE_RIGHT;
		}

		auto obj = createPhysicalControl("Door button", sector->getLayerIndex(), buttonX, y,
			side, flags, nullptr, alternateX, alternateSide);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateObjectResult Building::_createBulkheadDoorButton(shared_ptr<const Sector> sector, uint32_t y, int side, uint32_t* index)
	{
		uint32_t buttonX = side == CORE_SIDE_LEFT ? sector->getCellX1() : sector->getCellX0();

		auto obj = createPhysicalControl("BulkheadDoor button", sector->getLayerIndex(), buttonX, y, CORE_SIDE_MIDDLE, 0);

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

		auto obj = createPhysicalControl("ForceBridge button", sector->getLayerIndex(), buttonX, y, side, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateObjectResult Building::_createLadderButton(shared_ptr<const Sector> sector,
		uint32_t x, uint32_t y, int side, uint32_t flags, uint32_t* index,
		bool insetWithinCell)
	{
		string caller = format("_createLadderButton(<sector>, {}, {}, {}, {}, <index>)", x, y, side, flags);
		uint32_t buttonX = x;

		// Check position of button cell
		validateCellIsInSector(caller, buttonX, y, sector);

		auto obj = createPhysicalControl("Ladder button", sector->getLayerIndex(), buttonX, y, side, flags);
		if (insetWithinCell)
		{
			for (auto& placement : mPhysicalControlPlacements)
			{
				if (placement.sectorIndex != obj.sector->getIndex()
					|| placement.objectIndex != obj.index) continue;
				placement.edgeInset = 0.2f;
				break;
			}
			reflowPhysicalControls(sector->getLayerIndex(), sector->getIndex(), y);
		}

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	Building::CreateObjectResult Building::_createPlatformLiftButton(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index)
	{
		string caller = format("_createPlatformLiftButton(<sector>, {}, {}, {}, {}, {}, <index>)", x, y, cellsWide, side, flags);
		uint32_t buttonX = x + (side == CORE_SIDE_LEFT ? 0 : cellsWide - 1);

		// Check position of button cell
		validateCellIsInSector(caller, buttonX, y, sector);

		auto obj = createPhysicalControl("Platform lift button", sector->getLayerIndex(), buttonX, y, side, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	bool Building::getLiftLandingGeometry(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t& landingX, uint32_t& landingWidth) const
	{
		if (layerIndex >= getLayerCount()) return false;
		if (x >= mCellsWide || y >= mDecksHigh) return false;
		auto const& cell = mLayers[layerIndex]->getCellDefinition(x, y);
		if (cell.sectorIndex == ~0u) return false;
		auto lift = dynamic_pointer_cast<const LiftTransit>(mSectors[cell.sectorIndex]);
		if (!lift) return false;
		landingX = lift->getCellX();
		landingWidth = lift->getCellsWide();
		return y >= lift->getCellY() && y <= lift->getCellY1();
	}

	bool Building::isLiftOwnedDoor(shared_ptr<const SectorObject> const& object,
		uint32_t* liftSectorIndex, uint32_t* stopIndex) const
	{
		auto doorObject = dynamic_pointer_cast<const DoorSectorObject>(object);
		if (!doorObject) return false;
		auto resource = mTraversalResources.find(doorObject->getDoor()->getTraversalResourceId());
		if (!resource || !resource->mLiftCoordinator) return false;
		auto coordinator = mTraversalResources.find(resource->mLiftCoordinator);
		if (!coordinator || !coordinator->mLift || !coordinator->mLiftSector) return false;
		if (liftSectorIndex) *liftSectorIndex = (uint32_t)coordinator->mLiftSector.value - 1;
		if (stopIndex) *stopIndex = resource->mLiftStopIndex;
		return true;
	}

	bool Building::isLiftOwnedControl(shared_ptr<const SectorObject> const& object,
		uint32_t* liftSectorIndex, uint32_t* stopIndex) const
	{
		if (!object || object->getObjectType() != SectorObjectType::InteractionPoint) return false;
		auto button = dynamic_pointer_cast<const Button>(object->_getObject());
		if (!button || !button->getInteractionPointId()) return false;
		auto point = mInteractionPoints.find(button->getInteractionPointId());
		if (!point) return false;
		for (auto const& binding : point->mBindings)
		{
			if (binding.command.type != DeviceCommandType::CallLift) continue;
			auto resource = mTraversalResources.find(binding.command.traversalResource);
			if (!resource || !resource->mLift || !resource->mLiftSector) continue;
			if (liftSectorIndex) *liftSectorIndex = (uint32_t)resource->mLiftSector.value - 1;
			if (stopIndex) *stopIndex = binding.command.stopIndex;
			return true;
		}
		return false;
	}

	bool Building::isShuttleOwnedDoor(shared_ptr<const SectorObject> const& object,
		uint32_t* shuttleSectorIndex, uint32_t* stopIndex, uint32_t* carriageIndex,
		uint32_t* doorIndex) const
	{
		auto doorObject = dynamic_pointer_cast<const DoorSectorObject>(object);
		if (!doorObject) return false;
		auto landingId = doorObject->getDoor()->getTraversalResourceId();
		auto landing = mTraversalResources.find(landingId);
		if (!landing || !landing->mLiftCoordinator) return false;
		auto coordinator = mTraversalResources.find(landing->mLiftCoordinator);
		if (!coordinator || !coordinator->mShuttle || !coordinator->mLiftSector) return false;
		auto mapping = find_if(coordinator->mShuttleDoors.begin(), coordinator->mShuttleDoors.end(),
			[landingId](auto const& value) { return value.landingResource == landingId; });
		if (mapping == coordinator->mShuttleDoors.end()) return false;
		// Every output is independently optional (#104): the Shuttle sector index is
		// kept in a local so resolving doorIndex never dereferences a null output.
		auto const sectorIndex = (uint32_t)coordinator->mLiftSector.value - 1;
		if (shuttleSectorIndex) *shuttleSectorIndex = sectorIndex;
		if (stopIndex) *stopIndex = mapping->stopIndex;
		if (carriageIndex) *carriageIndex = mapping->carriageIndex;
		if (doorIndex)
		{
			// The doorIndex is the position of this Door's cell within the
			// carriage's selected doorMask cells, matching the per-Door override
			// grid used by setShuttleDoorOpenStyle.
			*doorIndex = ~0u;
			uint32_t producerIndex = 0;
			for (auto const& record : mConstructionRecords)
			{
				if (!constructionTypeCreatesSector(record.type)) continue;
				if (producerIndex++ != sectorIndex) continue;
				if (record.type != ConstructionType::Shuttle) break;
				if (mapping->stopIndex >= record.values.size()
					|| mapping->carriageIndex >= record.d) break;
				auto const doorMask = record.h ? record.h : (1u << 1);
				auto const doorOffsets = SimulationCoordinator::shuttleDoorOffsets(record.e, doorMask);
				auto const base = record.b + record.values[mapping->stopIndex]
					+ mapping->carriageIndex * (record.e + 1);
				for (size_t d = 0; d < doorOffsets.size(); ++d)
					if (base + doorOffsets[d] == doorObject->getCellX())
					{
						*doorIndex = static_cast<uint32_t>(d);
						break;
					}
				break;
			}
		}
		return true;
	}

	bool Building::isShuttleOwnedControl(shared_ptr<const SectorObject> const& object,
		uint32_t* shuttleSectorIndex, uint32_t* stopIndex) const
	{
		if (!object || object->getObjectType() != SectorObjectType::InteractionPoint) return false;
		auto button = dynamic_pointer_cast<const Button>(object->_getObject());
		if (!button || !button->getInteractionPointId()) return false;
		auto point = mInteractionPoints.find(button->getInteractionPointId());
		if (!point) return false;
		for (auto const& binding : point->mBindings)
		{
			if (binding.command.type != DeviceCommandType::CallShuttle) continue;
			auto resource = mTraversalResources.find(binding.command.traversalResource);
			if (!resource || !resource->mShuttle || !resource->mLiftSector) continue;
			if (shuttleSectorIndex) *shuttleSectorIndex = (uint32_t)resource->mLiftSector.value - 1;
			if (stopIndex) *stopIndex = binding.command.stopIndex;
			return true;
		}
		return false;
	}

	vector<uint32_t> Building::getValidShuttleStopOffsets(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t numCars, uint32_t carWidth,
		bool allowPartialLandings, uint32_t doorMask) const
	{
		vector<uint32_t> result;
		if (layerIndex == 0 || layerIndex >= getLayerCount()) return result;
		if (y >= mDecksHigh || x >= mCellsWide || cellsWide > mCellsWide - x
			|| numCars == 0 || carWidth < 3 || carWidth > 5
			|| doorMask == 0 || (doorMask >> carWidth) != 0
			|| numCars > (cellsWide + 1) / (carWidth + 1)) return result;
		auto const& landing = mLayers[layerInFront(layerIndex)];
		auto shuttleWidth = numCars * carWidth + numCars - 1;
		auto doorOffsets = SimulationCoordinator::shuttleDoorOffsets(carWidth, doorMask);
		if (shuttleWidth > cellsWide) return result;
		for (uint32_t offset = 0; offset + shuttleWidth <= cellsWide; ++offset)
		{
			bool any = false, all = true;
			for (uint32_t car = 0; car < numCars; ++car)
				for (auto doorOffset : doorOffsets)
				{
					auto doorX = x + offset + car * (carWidth + 1) + doorOffset;
					auto const& first = landing->getCellDefinition(doorX, y);
					// A Shuttle stop lands on any location-like Sector, a Facade
					// included (ADR 0003, ticket #52).
					bool supported = first.sectorIndex != ~0u
						&& isLocationLike(mSectors[first.sectorIndex]->getType());
					supported = supported && first.isTraversableOnFoot()
						&& !first.hasObject() && first.markers.empty();
					if (supported)
					{
						auto sector = mSectors[first.sectorIndex];
						supported = doorX != sector->getCellX0() || doorX != sector->getCellX1();
					}
					any = any || supported;
					all = all && supported;
				}
			if (any && (allowPartialLandings || all)) result.push_back(offset);
		}
		return result;
	}

	bool Building::getShuttleOptions(Shuttle const* shuttle, CreateShuttleOptions& options) const
	{
		if (!shuttle) return false;
		uint32_t sectorIndex = 0;
		for (auto const& record : mConstructionRecords)
		{
			bool producer = record.type == ConstructionType::Corridor || record.type == ConstructionType::Room
				|| record.type == ConstructionType::Ladder || record.type == ConstructionType::Stairwell || record.type == ConstructionType::Staircase
				|| record.type == ConstructionType::Lift || record.type == ConstructionType::Shuttle;
			if (!producer) continue;
			if (record.type == ConstructionType::Shuttle && sectorIndex < mSectors.size())
			{
				auto transit = dynamic_pointer_cast<const ShuttleTransit>(mSectors[sectorIndex]);
				if (transit && transit->getShuttle().get() == shuttle)
				{
					options = { record.d, record.e, record.values, record.f, record.g,
						record.x, record.y, record.p, record.h ? record.h : (1u << 1),
						record.overrides };
					return true;
				}
			}
			++sectorIndex;
		}
		return false;
	}

	vector<Building::ShuttleStopCandidate> Building::getShuttleStopCandidatesForDoor(
		uint32_t shuttleLayer, uint32_t y, uint32_t doorX) const
	{
		vector<ShuttleStopCandidate> result;
		uint32_t sectorIndex = 0;
		for (auto const& record : mConstructionRecords)
		{
			bool producer = record.type == ConstructionType::Corridor || record.type == ConstructionType::Room
				|| record.type == ConstructionType::Ladder || record.type == ConstructionType::Stairwell || record.type == ConstructionType::Staircase
				|| record.type == ConstructionType::Lift || record.type == ConstructionType::Shuttle;
			if (!producer) continue;
			// Only a Shuttle on the requested Layer can serve a Door on that pair.
			if (record.type == ConstructionType::Shuttle && record.layer == shuttleLayer && record.a == y)
			{
				auto doorMask = record.h ? record.h : (1u << 1);
				auto doorOffsets = SimulationCoordinator::shuttleDoorOffsets(record.e, doorMask);
				for (auto offset : getValidShuttleStopOffsets(record.layer, record.a, record.b, record.c,
					record.d, record.e, record.p, doorMask))
				{
					if (find(record.values.begin(), record.values.end(), offset) != record.values.end()) continue;
					auto vehicleWidth = record.d * record.e + record.d - 1;
					if (any_of(record.values.begin(), record.values.end(), [&](auto existing)
						{ return max(existing, offset) - min(existing, offset) < vehicleWidth; })) continue;
					bool matchesDoor = false;
					for (uint32_t car = 0; car < record.d && !matchesDoor; ++car)
						for (auto doorOffset : doorOffsets)
							matchesDoor = matchesDoor || doorX == record.b + offset
								+ car * (record.e + 1) + doorOffset;
					if (matchesDoor) result.push_back({ sectorIndex, offset });
				}
			}
			++sectorIndex;
		}
		return result;
	}

	bool Building::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, string* diagnostic) const
	{
		uint32_t liftX, liftWidth;
		if (getLiftLandingGeometry(layerBehind(layerIndex), y, x, liftX, liftWidth))
		{
			CreateDoorOptions options;
			options.width = liftWidth;
			return canAddCorridorDoor(layerIndex, y, liftX, options, diagnostic);
		}
		return canAddCorridorDoor(layerIndex, y, x, CreateDoorOptions{}, diagnostic);
	}

	bool Building::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateDoorOptions const& options, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		// A Door is authored on the front Layer of the pair it crosses.
		if (layerIndex + 1 >= getLayerCount())
			return reject("A Door needs a Layer directly behind the Layer it is authored on");
		auto const backLayer = layerBehind(layerIndex);
		// Door authoring reserves the final column as the building boundary.
		if (options.width == 0 || x >= mCellsWide || options.width > mCellsWide - x
			|| x + options.width >= mCellsWide || y >= mDecksHigh)
			return reject("Door position is outside the building");
		try
		{
			string const caller = "Building::canAddCorridorDoor";
			validateSectorDoorOptions(caller, options);
			uint32_t liftX, liftWidth;
			bool const liftLanding = getLiftLandingGeometry(backLayer, y, x, liftX, liftWidth);
			if (liftLanding && (x != liftX || options.width != liftWidth))
				return reject(format("Lift landing doors must start at {} and be {} cells wide", liftX, liftWidth));
			if (options.activationMode != DoorActivationMode::RemoteControlled
				&& (options.controls[0] || options.controls[1]))
				return reject("Physical controls require a remote-controlled Door");
			validateBounds(caller, x, y, options.width, 1);
			validateSpaceOnlyInOneSector(caller, layerIndex, x, y, options.width, 1);
			validateSpaceOnlyInOneSector(caller, backLayer, x, y, options.width, 1);
			shared_ptr<const Sector> sectors[2];
			for (uint32_t ix = x; ix < x + options.width; ++ix)
			{
				auto const& foreCell = mLayers[layerIndex]->getCellDefinition(ix, y);
				auto const& backCell = mLayers[backLayer]->getCellDefinition(ix, y);
				if (!foreCell.occupied()) return reject("A Door must be placed in a Room, Corridor or Facade on the Layer where it is authored");
				if (!backCell.occupied()) return reject("A Room, Corridor, Facade or Lift on the Layer behind is required here");
				sectors[0] = mSectors[foreCell.sectorIndex];
				sectors[1] = mSectors[backCell.sectorIndex];
				auto fore = dynamic_pointer_cast<const Location>(sectors[0]);
				auto back = dynamic_pointer_cast<const Location>(sectors[1]);
				if (!fore) return reject("A Door must be placed in a Room, Corridor or Facade on the Layer where it is authored");
				if (liftLanding)
				{
					if (sectors[1]->getType() != SectorType::Lift)
						return reject("The complete lift width must overlap one corridor");
				}
				else if (!back) return reject("A Room, Corridor or Facade on the Layer behind is required here");
				if (foreCell.hasObject() || backCell.hasObject()
					|| !foreCell.markers.empty() || !backCell.markers.empty())
					return reject("Another object blocks Door placement");
				if (!foreCell.isTraversableOnFoot() || !backCell.isTraversableOnFoot())
					return reject("Door placement requires a traversable floor on both layers");
				validateObjectAllowedInSector(caller, SectorObjectType::Door, foreCell.sectorIndex);
				validateObjectAllowedInSector(caller, SectorObjectType::Door, backCell.sectorIndex);
			}
			if (liftLanding)
			{
				auto const lift = dynamic_pointer_cast<const LiftTransit>(sectors[1]);
				for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
				{
					auto const& existing = lift->getStop(stop);
					if ((uint32_t)((int)existing.sector->getCellY() + existing.sectorOffsetY) == y)
						return reject("This lift already has a stop on this floor");
				}
				if (x == sectors[0]->getCellX0() && x + options.width - 1 == sectors[0]->getCellX1())
					return reject("There is no space to place the Lift call button");
			}
			else for (int side = 0; side < 2; ++side)
				if (options.controls[side] && x == sectors[side]->getCellX0()
					&& x + options.width - 1 == sectors[side]->getCellX1())
					return reject("There is no space to place a Door Button on this side");
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool Building::getSectorDoorOptions(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
		CreateDoorOptions& options) const
	{
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == layerIndex
					&& record.a == y && record.b == x && record.c == width;
			});
		if (found == mConstructionRecords.rend()) return false;
		options.width = found->c;
		options.controls[0] = found->p;
		options.controls[1] = found->q;
		options.activationMode = static_cast<DoorActivationMode>(found->i);
		options.holdOpenSeconds = found->x;
		options.crossingLanes = found->d;
		options.openStyle = static_cast<Door::OpenStyle>(found->j);
		return true;
	}

	bool Building::setSectorDoorOpenStyle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
		Door::OpenStyle style, std::string* diagnostic)
	{
		// Same patch shape as a Background recolour: the authored record is the
		// persistence boundary, so the record's style and the live Door move
		// together.  The most recent matching record wins, matching
		// getSectorDoorOptions' read-back.
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == layerIndex
					&& record.a == y && record.b == x && record.c == width;
			});
		if (found == mConstructionRecords.rend())
		{
			if (diagnostic)
				*diagnostic = "The selected Door no longer has an authored definition";
			return false;
		}
		found->j = static_cast<int32_t>(style);

		// The live Door rides with its record so the viewport and the Selection
		// panel show the new style without a rebuild.  The record's coordinates
		// are the Door's own left cell on its front Layer.
		if (layerIndex < mLayers.size() && mLayers[layerIndex])
		{
			auto const layer = mLayers[layerIndex].get();
			if (x < layer->getCellsWide() && y < layer->getDecksHigh())
			{
				auto const& cell = layer->getCellDefinition(x, y);
				if (cell.sectorObjectType == SectorObjectType::Door
					&& cell.sectorIndex < mSectors.size() && mSectors[cell.sectorIndex])
				{
					auto const sector = mSectors[cell.sectorIndex];
					if (cell.sectorObjectIndex < sector->getNumObjects())
					{
						auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
							sector->_getObject(cell.sectorObjectIndex));
						if (doorObject && doorObject->getDoor())
							doorObject->getDoor()->setOpenStyle(style);
					}
				}
			}
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	Building::CreateDoorResult Building::addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x)
	{
		return addSectorDoor(layerIndex, y, x, CreateDoorOptions{});
	}

	bool Building::setLiftStopDoorOpenStyle(uint32_t liftSectorIndex, uint32_t stopIndex,
		Door::OpenStyle style, std::string* diagnostic)
	{
		// A Lift's landing Doors have no Door records of their own; the Lift's
		// producing record owns them.  The per-stop override therefore rides in
		// that record, which is the persistence boundary: save/load and the
		// editor's snapshot-based undo/redo carry the choice, and any later
		// rebuild replays it.  The Lift's topology is not touched.
		uint32_t producerIndex = 0;
		auto found = mConstructionRecords.end();
		for (auto it = mConstructionRecords.begin(); it != mConstructionRecords.end(); ++it)
		{
			if (!constructionTypeCreatesSector(it->type)) continue;
			if (producerIndex++ == liftSectorIndex) { found = it; break; }
		}
		if (found == mConstructionRecords.end() || found->type != ConstructionType::Lift)
		{
			if (diagnostic) *diagnostic = "The selected Lift no longer has an authored definition";
			return false;
		}
		if (stopIndex >= found->values.size())
		{
			if (diagnostic)
				*diagnostic = "The selected stop is outside the Lift's authored topology";
			return false;
		}
		// Normalize with a preserving resize: a creation/load vector shorter
		// than the stop list keeps every authored prefix entry, and only the
		// newly added slots take the default sentinel.  assign() would discard
		// accepted overrides on earlier stops.
		if (found->overrides.size() < found->values.size())
			found->overrides.resize(found->values.size(), ~0u);
		found->overrides[stopIndex] = static_cast<uint32_t>(style);

		// The live landing Door rides with its record so the viewport and the
		// Selection panel show the new style without a rebuild.  The Door is
		// authored on the Layer in front of the Lift, at the stop's deck.
		if (liftSectorIndex < mSectors.size() && mSectors[liftSectorIndex])
		{
			auto const lift = dynamic_pointer_cast<const LiftTransit>(mSectors[liftSectorIndex]);
			if (lift && lift->getLayerIndex() > 0)
			{
				auto const frontLayer = layerInFront(lift->getLayerIndex());
				auto const doorY = lift->getCellY() + found->values[stopIndex];
				if (frontLayer < mLayers.size() && mLayers[frontLayer]
					&& lift->getCellX() < mLayers[frontLayer]->getCellsWide()
					&& doorY < mLayers[frontLayer]->getDecksHigh())
				{
					auto const& cell = mLayers[frontLayer]->getCellDefinition(lift->getCellX(), doorY);
					if (cell.sectorObjectType == SectorObjectType::Door
						&& cell.sectorIndex < mSectors.size() && mSectors[cell.sectorIndex])
					{
						auto const sector = mSectors[cell.sectorIndex];
						if (cell.sectorObjectIndex < sector->getNumObjects())
						{
							auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
								sector->_getObject(cell.sectorObjectIndex));
							if (doorObject && doorObject->getDoor())
								doorObject->getDoor()->setOpenStyle(style);
						}
					}
				}
			}
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool Building::setShuttleDoorOpenStyle(uint32_t shuttleSectorIndex, uint32_t stopIndex,
		uint32_t carriageIndex, uint32_t doorIndex, Door::OpenStyle style, std::string* diagnostic)
	{
		// A Shuttle's landing Doors have no Door records of their own; the Shuttle's
		// producing record owns them.  The per-Door override therefore rides in
		// that record, which is the persistence boundary: save/load and the
		// editor's snapshot-based undo/redo carry the choice, and any later
		// rebuild replays it.  The Shuttle's topology is not touched, and the
		// override addresses one cell of the fixed stop/carriage/door grid, so
		// no sibling Door at the same or another stop is reached.
		uint32_t producerIndex = 0;
		auto found = mConstructionRecords.end();
		for (auto it = mConstructionRecords.begin(); it != mConstructionRecords.end(); ++it)
		{
			if (!constructionTypeCreatesSector(it->type)) continue;
			if (producerIndex++ == shuttleSectorIndex) { found = it; break; }
		}
		if (found == mConstructionRecords.end() || found->type != ConstructionType::Shuttle)
		{
			if (diagnostic) *diagnostic = "The selected Shuttle no longer has an authored definition";
			return false;
		}
		auto const doorMask = found->h ? found->h : (1u << 1);
		auto const doorOffsets = SimulationCoordinator::shuttleDoorOffsets(found->e, doorMask);
		if (stopIndex >= found->values.size() || carriageIndex >= found->d
			|| doorIndex >= doorOffsets.size())
		{
			if (diagnostic)
				*diagnostic = "The selected Door is outside the Shuttle's authored topology";
			return false;
		}
		auto const totalSlots = found->values.size() * found->d * doorOffsets.size();
		found->overrides.resize(totalSlots, ~0u);
		auto const slot = (stopIndex * found->d + carriageIndex) * doorOffsets.size() + doorIndex;
		found->overrides[slot] = static_cast<uint32_t>(style);

		// The live landing Door rides with its record so the viewport and the
		// Selection panel show the new style without a rebuild.  The Door is
		// authored on the Layer in front of the Shuttle, at the stop's column.
		if (shuttleSectorIndex < mSectors.size() && mSectors[shuttleSectorIndex])
		{
			auto const transit = dynamic_pointer_cast<const ShuttleTransit>(mSectors[shuttleSectorIndex]);
			if (transit && transit->getLayerIndex() > 0)
			{
				auto const frontLayer = layerInFront(transit->getLayerIndex());
				auto const doorX = found->b + found->values[stopIndex]
					+ carriageIndex * (found->e + 1) + doorOffsets[doorIndex];
				if (frontLayer < mLayers.size() && mLayers[frontLayer]
					&& doorX < mLayers[frontLayer]->getCellsWide()
					&& found->a < mLayers[frontLayer]->getDecksHigh())
				{
					auto const& cell = mLayers[frontLayer]->getCellDefinition(doorX, found->a);
					if (cell.sectorObjectType == SectorObjectType::Door
						&& cell.sectorIndex < mSectors.size() && mSectors[cell.sectorIndex])
					{
						auto const sector = mSectors[cell.sectorIndex];
						if (cell.sectorObjectIndex < sector->getNumObjects())
						{
							auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
								sector->_getObject(cell.sectorObjectIndex));
							if (doorObject && doorObject->getDoor())
								doorObject->getDoor()->setOpenStyle(style);
						}
					}
				}
			}
		}
		modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	Building::CreateDoorResult Building::addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, CreateDoorOptions const& options)
	{
		beginStructuralEdit("addSectorDoor");
		uint32_t liftX, liftWidth;
		if (getLiftLandingGeometry(layerBehind(layerIndex), y, x, liftX, liftWidth))
		{
			string diagnostic;
			CreateDoorOptions normalized;
			normalized.width = liftWidth;
			if (!canAddCorridorDoor(layerIndex, y, liftX, normalized, &diagnostic))
				throw BuildingException(this, diagnostic);
			auto const liftIndex = mLayers[layerBehind(layerIndex)]->getCellDefinition(liftX, y).sectorIndex;
			auto lift = dynamic_pointer_cast<LiftTransit>(_getSector(liftIndex));
			auto idlePlan = planResizeLift(liftIndex, lift->getCellX(), lift->getCellY(),
				lift->getCellsWide(), lift->getDecksHigh());
			if (!idlePlan.valid) throw BuildingException(this, idlePlan.diagnostic);
			idlePlan.stopOffsets.clear();
			for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
			{
				auto const& value = lift->getStop(stop);
				idlePlan.stopOffsets.push_back((uint32_t)((int)value.sector->getCellY()
					+ value.sectorOffsetY - (int)lift->getCellY()));
			}
			idlePlan.stopOffsets.push_back(y - lift->getCellY());
			sort(idlePlan.stopOffsets.begin(), idlePlan.stopOffsets.end());
			vector<ConstructionRecord> records;
			if (!prepareLiftEdit(idlePlan, records, diagnostic)) throw BuildingException(this, diagnostic);
			rebuildFromConstructionRecords(std::move(records));
			auto const& cell = mLayers[layerIndex]->getCellDefinition(liftX, y);
			auto sector = _getSector(cell.sectorIndex);
			CreateObjectResult doorResult{ cell.sectorObjectIndex, SectorObjectType::Door, sector };
			auto doorObject = dynamic_pointer_cast<DoorSectorObject>(sector->_getObject(cell.sectorObjectIndex));
			return { doorResult, {}, doorObject->getDoor()->getTraversalResourceId() };
		}
		auto result = _addSectorDoor(layerIndex, y, x, options);
		ConstructionRecord record{ ConstructionType::Door };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.width; record.d = options.crossingLanes;
		record.p = options.controls[0]; record.q = options.controls[1];
		record.i = static_cast<int32_t>(options.activationMode); record.x = options.holdOpenSeconds;
		record.j = static_cast<int32_t>(options.openStyle);
		recordConstruction(std::move(record));
		return result;
	}

	Building::CreateObjectResult Building::addSectorDoorButton(uint32_t sectorIndex,
		uint32_t objectIndex)
	{
		beginStructuralEdit("addSectorDoorButton");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| objectIndex >= mSectors[sectorIndex]->getNumObjects())
		{
			throw BuildingException(this, "The selected Door no longer exists");
		}

		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mSectors[sectorIndex]->getObject(objectIndex));
		if (doorObject && (isLiftOwnedDoor(doorObject) || isShuttleOwnedDoor(doorObject)))
			throw BuildingException(this, "Transport-owned Doors are read-only; their call button is managed by the transport");
		if (!doorObject)
		{
			throw BuildingException(this, "The selected object is not a Door");
		}
		auto door = doorObject->getDoor();
		auto sector = mSectors[sectorIndex];
		// A Door owns one Sector in each Layer of the pair it crosses.  Which side of
		// that pair the selected Sector sits on decides which control flag is free.
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector)
		{
			throw BuildingException(this, "The selected Door does not belong to this Sector");
		}
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;

		auto source = find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorObject->getCellY()
					&& record.b == doorObject->getCellX()
					&& record.c == door->getCellsWide();
			});
		if (source == mConstructionRecords.end())
		{
			throw BuildingException(this, "This Door does not support an added Door Button");
		}
		bool const alreadyHasButton = isFrontSide ? source->p : source->q;
		if (alreadyHasButton)
		{
			throw BuildingException(this, "This side of the Door already has a Door Button");
		}
		if (doorObject->getCellX() == sector->getCellX0()
			&& doorObject->getCellX() + door->getCellsWide() - 1 == sector->getCellX1())
		{
			throw BuildingException(this, "There is no space to place a Door Button on this side");
		}

		auto control = _createDoorButton(sector, doorObject->getCellX(),
			doorObject->getCellY(), door->getCellsWide(), CORE_BUTTON_F_AUTO_REENABLE);
		DeviceCommand command;
		command.type = DeviceCommandType::OpenDoor;
		command.desiredState = true;
		command.traversalResource = door->getTraversalResourceId();
		auto point = createPhysicalControlInteractionPoint("Door button", control,
			(float)doorObject->getCellY(), CORE_AGENT_MAX_HEIGHT * 0.4f,
			getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
		if (!addTraversalControl(door->getTraversalResourceId(), point))
		{
			throw BuildingException(this, "Could not bind the Door Button to its Door");
		}

		// A Door with a physical open control uses remote-controlled preparation.
		auto resource = mTraversalResources.find(door->getTraversalResourceId());
		resource->mDoorActivationMode = DoorActivationMode::RemoteControlled;
		door->configureTraversal(DoorActivationMode::RemoteControlled,
			door->getTraversalResourceId(), door->mHoldOpenTime);
		source->i = static_cast<int32_t>(DoorActivationMode::RemoteControlled);
		if (isFrontSide) source->p = true;
		else source->q = true;
		return control;
	}

	Building::CreateDoorResult Building::_addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateDoorOptions const& options, bool controlsAreExternallyBound)
	{
		string caller = format("Building::addSectorDoor({}, {}, {}, {})", layerIndex, y, x, options.width);

		auto cellsWide = options.width;

		// A Door is authored on the front Layer of the pair it crosses.
		validateLayer(caller, layerIndex);
		if (layerIndex + 1 >= getLayerCount())
		{
			throw BuildingException(this,
				format("{} - a Door needs a Layer directly behind the Layer it is authored on", caller));
		}
		auto const backLayer = layerBehind(layerIndex);

		validateSectorDoorOptions(caller, options);
		if (!controlsAreExternallyBound && options.activationMode != DoorActivationMode::RemoteControlled
			&& (options.controls[0] || options.controls[1]))
		{
			throw BuildingException(this,
				format("{} - physical controls require remote-controlled activation", caller));
		}
		validateBounds(caller, x, y, cellsWide, 1);
		validateSpaceOnlyInOneSector(caller, layerIndex, x, y, cellsWide, 1);
		validateSpaceOnlyInOneSector(caller, backLayer, x, y, cellsWide, 1);

		auto frontLayer = getLayer(layerIndex);
		auto behindLayer = getLayer(backLayer);

		shared_ptr<Sector> sectors[2];
		for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef0 = frontLayer->getCellDefinition(ix, y);
			auto& cellDef1 = behindLayer->getCellDefinition(ix, y);

			if (cellDef0.sectorIndex == ~0u)
			{
				throw BuildingException(this, format("{} - front Layer cell at {},{} is not occupied.", caller, ix, y));
			}

			sectors[0] = _getSector(cellDef0.sectorIndex);

			if (cellDef1.sectorIndex == ~0u)
			{
				throw BuildingException(this, format("{} - back Layer cell at {},{} is not occupied.", caller, ix, y));
			}

			sectors[1] = _getSector(cellDef1.sectorIndex);

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
		auto doorObject = createDoor(layerIndex, x, y, cellsWide);
		auto doorSectorObject = dynamic_pointer_cast<DoorSectorObject>(doorObject.sector->_getObject(doorObject.index));
		auto door = doorSectorObject->getDoor();
		door->setOpenStyle(options.openStyle);
		auto traversalResource = createDoorTraversalResource(
			format("Door at {},{}", x, y), door, options.activationMode, options.holdOpenSeconds);
		door->configureTraversal(options.activationMode, traversalResource, options.holdOpenSeconds);
		if (options.crossingLanes != 0)
		{
			configureDoorCrossingLanes(traversalResource, options.crossingLanes);
		}

		// Door approaches are explicit resource geometry. Seed each side at the
		// threshold, which Door placement already proved has traversable floor.
		// Queue positions are expanded below only across contiguous usable floor;
		// an unrelated gap elsewhere in a multi-deck Room must not invalidate the Door.
		auto const threshold = Vector2{ x + cellsWide * 0.5f, (float)y };
		for (auto const& sector : sectors)
			configureDoorQueueLane(traversalResource,
				SectorId{ (uint64_t)sector->getIndex() + 1 }, threshold,
				Vector2::UNIT_X, 0.0f);

		// Each side owns an independent centre-first queue. Expand left and right
		// until that direction reaches either the Sector boundary or a floor gap.
		auto queueResource = mTraversalResources.find(traversalResource);
		auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
		for (auto& lane : queueResource->mQueueLanes)
		{
			auto sector = mSectors[(size_t)lane.sector.value - 1];
			auto positionFits = [&](float positionX)
			{
				if (positionX - halfWidth < sector->getCellX0() - 0.001f
					|| positionX + halfWidth > sector->getCellX1() + 1.0f + 0.001f
					|| threshold.y < sector->getCellY0() - 0.001f
					|| threshold.y + CORE_AGENT_MAX_HEIGHT > sector->getCellY1() + 1.0f + 0.001f)
					return false;
				auto const cellX = min(sector->getCellX1(), (uint32_t)floor(positionX));
				auto const cellY = min(sector->getCellY1(), (uint32_t)floor(threshold.y));
				return mLayers[sector->getLayerIndex()]
					->getCellDefinition(cellX, cellY).isTraversableOnFoot();
			};

			vector<Vector2> positions{ threshold };
			bool scanLeft = true, scanRight = true;
			for (uint32_t step = 1; scanLeft || scanRight; ++step)
			{
				auto const distance = step * (float)CORE_DOOR_QUEUE_STOP_WIDTH;
				auto const left = threshold.x - distance;
				auto const right = threshold.x + distance;
				if (scanLeft)
				{
					scanLeft = positionFits(left);
					if (scanLeft) positions.push_back({ left, threshold.y });
				}
				if (scanRight)
				{
					scanRight = positionFits(right);
					if (scanRight) positions.push_back({ right, threshold.y });
				}
			}
			lane.origin = threshold;
			lane.direction = Vector2::UNIT_X;
			lane.extent = positions.empty() ? 0.0f : max(abs(positions.back().x - threshold.x),
				abs(positions.front().x - threshold.x));
			lane.positions = std::move(positions);
			lane.positionOwners.assign(lane.positions.size(), {});
		}

		// The shared Door can have different local object indices in its two Sectors.
		uint32_t backDoorIndex = ~0u;
		for (uint32_t i = 0; i < sectors[1]->getNumObjects(); ++i)
		{
			if (sectors[1]->getObject(i) == doorSectorObject)
			{
				backDoorIndex = i;
				break;
			}
		}
		if (backDoorIndex == ~0u)
			throw BuildingException(this, format("{} - could not locate Door in its back-layer Sector", caller));

		// Set layers
		for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef0 = frontLayer->getCellDefinition(ix, y);
			auto& cellDef1 = behindLayer->getCellDefinition(ix, y);

			cellDef0.sectorObjectIndex = doorObject.index;
			cellDef0.sectorObjectType = doorObject.type;

			cellDef1.sectorObjectIndex = backDoorIndex;
			cellDef1.sectorObjectType = doorObject.type;
		}

		// Bind physical controls to typed device commands
		CreateObjectResult createdControls[2];

		for (int i = 0; i < 2; ++i)
		{
			if (options.controls[i])
			{
				if (x == sectors[i]->getCellX0() && (x + options.width - 1) == sectors[i]->getCellX1())
				{
					throw BuildingException(this, format("{} - No space to place Buttons for Door", caller));
				}

				auto buttonObject = _createDoorButton(sectors[i], x, y, cellsWide, CORE_BUTTON_F_AUTO_REENABLE, &createdControls[i].index);
				createdControls[i].type = SectorObjectType::InteractionPoint;
				createdControls[i].sector = sectors[i];

				if (!controlsAreExternallyBound)
				{
					DeviceCommand command;
					command.type = DeviceCommandType::OpenDoor;
					command.desiredState = true;
					command.traversalResource = traversalResource;
					auto point = createPhysicalControlInteractionPoint("Door button",
						createdControls[i], (float)y, CORE_AGENT_MAX_HEIGHT * 0.4f,
						getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
					addTraversalControl(traversalResource, point);
				}
			}
		}


		return { doorObject, { createdControls[0], createdControls[1] }, traversalResource };
	}

	bool Building::canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t decksHigh, string* diagnostic) const
	{
		string caller = format("Building::addSectorWindow({}, {}, {}, {})", layerIndex, y, x, cellsWide);
		try
		{
			validateLayer(caller, layerIndex);
			validateBounds(caller, x, y, cellsWide, decksHigh);

			// A Window joins its own Layer to the Layer directly behind it.  The back-most
			// Layer has nothing behind it, so a Window there would cross no threshold and
			// could never take part in the Graph.  Maps written before the rule can still
			// replay one from that Layer - see createWindow() - but none may be authored.
			if (!mDeserializingConstruction && isBackMostLayer(layerIndex, getLayerCount()))
				throw BuildingException(this, format(
					"{} - a Window needs a Layer behind it, and Layer {} is the back-most Layer",
					caller, layerIndex));

			vector<uint32_t> requiredLayers{ layerIndex };
			if (layerIndex + 1 < getLayerCount()) requiredLayers.push_back(layerBehind(layerIndex));

			// The front Layer of the pair keeps the strict one-Sector rule: a Window
			// whose own wall straddles two Sectors has no coherent frame.  The Layer
			// behind may span several Backgrounds, but may not mix one with a Room.
			for (auto requiredLayer : requiredLayers)
				validateSpaceOnlyInOneSector(caller, requiredLayer, x, y, cellsWide, decksHigh,
					requiredLayer != layerIndex);

			for (auto requiredLayer : requiredLayers)
			{
				auto layer = getLayer(requiredLayer);
				for (uint32_t iy = y; iy < y + decksHigh; ++iy)
					for (uint32_t ix = x; ix < x + cellsWide; ++ix)
					{
						auto const& cellDef = layer->getCellDefinition(ix, iy);
						if (cellDef.sectorIndex == ~0u)
							throw BuildingException(this, format("{} - Layer {} cell at {},{} is not occupied.",
								caller, requiredLayer, ix, iy));
						if (requiredLayer == layerIndex)
							validateObjectAllowedInSector(caller, SectorObjectType::Window, cellDef.sectorIndex);
						else
							validateObjectAllowedInSectorAsLookTarget(caller, SectorObjectType::Window, cellDef.sectorIndex);
						if (cellDef.hasObject() || !cellDef.markers.empty())
							throw BuildingException(this, format("{} - another object occupies cell at {},{}",
								caller, ix, iy));
					}
			}
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	uint32_t Building::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh)
	{
		return addSectorWindow(layerIndex, y, x, cellsWide, decksHigh, {}).window.index;
	}

	Building::CreateWindowResult Building::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t decksHigh, CreateWindowOptions const& options)
	{
		string diagnostic;
		if (!canAddSectorWindow(layerIndex, y, x, cellsWide, decksHigh, &diagnostic))
			throw BuildingException(this, diagnostic);
		if (options.traversable)
		{
			// A Window may look into a Background, but a traversable Window would admit
			// Agents into the Sector behind it, and a Background is seen through, never
			// entered: it owns no walkable floor and takes no part in traversal.
			if (layerIndex + 1 < getLayerCount())
			{
				auto const backLayer = layerBehind(layerIndex);
				for (uint32_t iy = y; iy < y + decksHigh; ++iy)
					for (uint32_t ix = x; ix < x + cellsWide; ++ix)
					{
						auto const& backCell = mLayers[backLayer]->getCellDefinition(ix, iy);
						if (backCell.sectorIndex != ~0u
							&& mSectors[backCell.sectorIndex]->getType() == SectorType::Background)
							throw BuildingException(this, format(
								"A traversable Window cannot cross into the Background at {},{}: a Background can be looked into, but never entered",
								ix, iy));
					}
			}
		}
		beginStructuralEdit("addSectorWindow");
		auto layer = getLayer(layerIndex);

		// Create window
		auto createdWindow = createWindow(layerIndex, x, y, cellsWide, decksHigh);
		auto windowIndex = createdWindow.index;
		auto windowObjType = createdWindow.type;
		auto windowSector = createdWindow.sector;
		auto windowObject = dynamic_pointer_cast<WindowSectorObject>(windowSector->_getObject(windowIndex));
		auto window = windowObject->getWindow();
		window->setState(options.initialState, options.style);
		TraversalResourceId traversalResource;
		if (options.traversable && window->getFrontSector() && window->getBackSector())
		{
			traversalResource = createWindowTraversalResource(format("Window at {},{}", x, y), window);
			window->configureTraversal(true, traversalResource);

			// A traversable threshold must be discovered while scanning both layers.
			// The shared SectorObject may have a different vector index in its second
			// sector, so recover that index rather than copying the foreground value.
			auto backSector = const_pointer_cast<Sector>(window->getBackSector());
			uint32_t backObjectIndex = ~0u;
			for (uint32_t i = 0; i < backSector->getNumObjects(); ++i)
			{
				if (backSector->_getObject(i) == windowObject) { backObjectIndex = i; break; }
			}
			if (backObjectIndex == ~0u) throw BuildingException(this, "Traversable window is missing its back-sector object");
			for (uint32_t iy = y; iy < y + decksHigh; ++iy)
				for (uint32_t ix = x; ix < x + cellsWide; ++ix)
				{
					auto& backCell = mLayers[layerBehind(layerIndex)]->getCellDefinition(ix, iy);
					backCell.sectorObjectIndex = backObjectIndex;
					backCell.sectorObjectType = SectorObjectType::Window;
				}
		}

		// Set layers
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);

				cellDef.sectorObjectIndex = windowIndex;
				cellDef.sectorObjectType = SectorObjectType::Window;
			}

		ConstructionRecord record{ ConstructionType::Window };
		record.a = layerIndex; record.b = y; record.c = x; record.d = cellsWide; record.e = decksHigh;
		record.p = options.traversable;
		record.i = static_cast<int32_t>(options.initialState);
		record.j = static_cast<int32_t>(options.style);
		recordConstruction(std::move(record));
		return { { windowIndex, windowObjType, windowSector }, window, traversalResource };
	}

	bool Building::getSectorWindowOptions(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t decksHigh, CreateWindowOptions& options) const
	{
		auto found = find_if(mConstructionRecords.rbegin(), mConstructionRecords.rend(),
			[&](ConstructionRecord const& record)
			{
				return record.type == ConstructionType::Window && record.a == layerIndex
					&& record.b == y && record.c == x && record.d == cellsWide
					&& record.e == decksHigh;
			});
		if (found == mConstructionRecords.rend()) return false;
		options.traversable = found->p;
		options.initialState = static_cast<Window::State>(found->i);
		options.style = static_cast<Window::Style>(found->j);
		return true;
	}

	bool Building::canAddSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		int side, CreateBulkheadDoorOptions const& options, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (layerIndex >= mLayers.size() || y >= mDecksHigh
			|| (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT))
			return reject("Bulkhead Door position is outside the building");
		uint32_t thresholdX;
		if (side == CORE_SIDE_LEFT)
		{
			if (x == 0 || x >= mCellsWide) return reject("Bulkhead Doors require a cell on each side");
			thresholdX = x;
		}
		else
		{
			if (x >= mCellsWide - 1) return reject("Bulkhead Doors require a cell on each side");
			thresholdX = x + 1;
		}
		if (options.holdOpenSeconds < 0.0f)
			return reject("Bulkhead Door hold-open time cannot be negative");
		if (options.crossingLanes != 1)
			return reject("Bulkhead Doors support exactly one crossing lane");
		if (options.activationMode != DoorActivationMode::RemoteControlled
			&& (options.controls[0] || options.controls[1]))
			return reject("Physical controls require a remote-controlled Bulkhead Door");
		try
		{
			auto const& left = mLayers[layerIndex]->getCellDefinition(thresholdX - 1, y);
			auto const& right = mLayers[layerIndex]->getCellDefinition(thresholdX, y);
			if (!left.occupied() || !right.occupied())
				return reject("Bulkhead Doors require a Location on each side");
			if (left.sectorIndex == right.sectorIndex)
				return reject("Bulkhead Doors must connect two distinct Locations");
			if (!dynamic_pointer_cast<const Location>(mSectors[left.sectorIndex])
				|| !dynamic_pointer_cast<const Location>(mSectors[right.sectorIndex]))
				return reject("Bulkhead Doors can only connect Locations");
			// A Bulkhead Door is set into a pair of wall ends. A Facade has no
			// wall ends, so there is nowhere for one to go (ADR 0003).
			if (mSectors[left.sectorIndex]->getType() == SectorType::Facade
				|| mSectors[right.sectorIndex]->getType() == SectorType::Facade)
				return reject("A Bulkhead Door cannot connect a Facade: a Facade has no wall ends");
			if (!left.isTraversableOnFoot() || !right.isTraversableOnFoot())
				return reject("Bulkhead Door placement requires a traversable floor on both sides");
			if (left.bulkheadIndices[CORE_SIDE_RIGHT] != ~0u
				|| right.bulkheadIndices[CORE_SIDE_LEFT] != ~0u)
				return reject("A Bulkhead Door already occupies this boundary");
			if (left.sectorObjectType == SectorObjectType::Door
				|| left.sectorObjectType == SectorObjectType::Window
				|| right.sectorObjectType == SectorObjectType::Door
				|| right.sectorObjectType == SectorObjectType::Window)
				return reject("Another object blocks Bulkhead Door placement");
			validateObjectAllowedInSector("Building::canAddSectorBulkheadDoor",
				SectorObjectType::BulkheadDoor, left.sectorIndex);
			validateObjectAllowedInSector("Building::canAddSectorBulkheadDoor",
				SectorObjectType::BulkheadDoor, right.sectorIndex);
		}
		catch (Exception const& error) { return reject(error.getMessage()); }
		catch (exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	Building::CreateBulkheadDoorResult Building::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y,
		uint32_t x, int side)
	{
		return addSectorBulkheadDoor(layerIndex, y, x, side, CreateBulkheadDoorOptions{});
	}

	Building::CreateBulkheadDoorResult Building::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		int side, CreateBulkheadDoorOptions const& options)
	{
		beginStructuralEdit("addSectorBulkheadDoor");
		ASSERT_SIDE_OK(side);

		string caller = format("Building::addSectorBulkheadDoor({}, {}, {}, {})", layerIndex, y, x, side);
		string diagnostic;
		if (!canAddSectorBulkheadDoor(layerIndex, y, x, side, options, &diagnostic))
			throw BuildingException(this, format("{} - {}", caller, diagnostic));

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

		auto traversalResource = createDoorTraversalResource(format("Bulkhead door at {},{}", x, y),
			door, options.activationMode, options.holdOpenSeconds);
		door->configureTraversal(options.activationMode, traversalResource, options.holdOpenSeconds);
		configureDoorCrossingLanes(traversalResource, options.crossingLanes);

		// Same-layer geometry gets explicit approaches on opposite sides of the
		// threshold; no Y/layer heuristic participates in authorization.
		auto threshold = Vector2{ (float)x, (float)y };
		auto leftOrigin = threshold - Vector2::UNIT_X * CORE_DOOR_QUEUE_STOP_WIDTH;
		auto rightOrigin = threshold + Vector2::UNIT_X * CORE_DOOR_QUEUE_STOP_WIDTH;
		configureDoorQueueLane(traversalResource, SectorId{ (uint64_t)sector0->getIndex() + 1 },
			leftOrigin, Vector2::NEGATIVE_UNIT_X,
			max(0.0f, leftOrigin.x - (sector0->getCellX0() + CORE_AGENT_MAX_WIDTH * 0.5f)));
		configureDoorQueueLane(traversalResource, SectorId{ (uint64_t)sector1->getIndex() + 1 },
			rightOrigin, Vector2::UNIT_X,
			max(0.0f, (sector1->getCellX1() + 1.0f - CORE_AGENT_MAX_WIDTH * 0.5f) - rightOrigin.x));

		CreateObjectResult createdControls[2];

		for (int i = 0; i < CORE_NUM_SIDES; ++i)
		{
			if (!options.controls[i]) continue;
			auto sector = i == CORE_SIDE_LEFT ? sector0 : sector1;
			createdControls[i] = _createBulkheadDoorButton(sector, y, i);
			if (options.activationMode == DoorActivationMode::RemoteControlled)
			{
				DeviceCommand command;
				command.type = DeviceCommandType::OpenDoor;
				command.desiredState = true;
				command.traversalResource = traversalResource;
				auto point = createPhysicalControlInteractionPoint("Bulkhead door button",
					createdControls[i], (float)y, 0.15f, getFixedTimestep(),
					{ { command, InteractionBindingRequirement::Required } });
				addTraversalControl(traversalResource, point);
			}
		}
		ConstructionRecord record{ ConstructionType::BulkheadDoor };
		record.a = layerIndex; record.b = y; record.c = x; record.i = side;
		record.p = options.controls[0]; record.q = options.controls[1];
		record.j = static_cast<int32_t>(options.activationMode);
		record.x = options.holdOpenSeconds; record.d = options.crossingLanes;
		recordConstruction(std::move(record));
		return { doorObject, { createdControls[0], createdControls[1] }, traversalResource };
	}

	Building::CreateObjectResult Building::addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset)
	{
		beginStructuralEdit("addSectorLightSwitch");
		auto sector = _getSector(sectorIndex);
		auto ctrl = _createSectorButton("Lightswitch", sector, xOffset, 0, CORE_BUTTON_F_AUTO_REENABLE);

		auto object = ctrl.sector->_getObject(ctrl.index);
		DeviceCommand command;
		command.type = DeviceCommandType::SetSectorLights;
		command.target = SectorId{ (uint64_t)sectorIndex + 1 };
		command.desiredState = !sector->areLightsOn();
		createPhysicalControlInteractionPoint("Light switch", ctrl,
			(float)object->getCellY(), 0.15f, getFixedTimestep(),
			{ { command, InteractionBindingRequirement::Required } });
		ConstructionRecord record{ ConstructionType::LightSwitch };
		record.a = sectorIndex; record.b = xOffset;
		recordConstruction(std::move(record));
		return ctrl;
	}

	bool Building::canAddSectorWalkway(uint32_t sectorIndex, uint32_t deckIndex,
		uint32_t xOffset, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (sectorIndex >= mSectors.size()) return reject("Walkway Room does not exist");
		auto location = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!location) return reject("Walkways require a Location");
		if (deckIndex == 0) return reject("Walkways must be placed above the Location's ground floor");
		if (deckIndex >= location->getDecksHigh() || xOffset >= location->getCellsWide())
			return reject("Walkway position is outside the Location");

		auto const& cellDef = mLayers[location->getLayerIndex()]->getCellDefinition(
			location->getCellX() + xOffset, location->getCellY() + deckIndex);
		if (cellDef.floorType == CellFloorType::Ground)
			return reject("Walkways cannot replace a ground floor");
		if (cellDef.floorType == CellFloorType::Walkway)
			return reject("A Walkway already occupies this cell");
		if (cellDef.floorType == CellFloorType::ForceBridge)
			return reject("A Force Bridge already occupies this cell");
		if (cellDef.floorType != CellFloorType::None)
			return reject("The destination floor cell is occupied");

		if (diagnostic) diagnostic->clear();
		return true;
	}

	Building::CreateObjectResult Building::addSectorWalkway(uint32_t sectorIndex,
		uint32_t deckIndex, uint32_t xOffset)
	{
		string caller = format("Building::addSectorWalkway({}, {}, {})", sectorIndex, deckIndex, xOffset);
		string diagnostic;
		if (!canAddSectorWalkway(sectorIndex, deckIndex, xOffset, &diagnostic))
			throw BuildingException(this, format("{} - {}", caller, diagnostic));

		// Once editing an already-built document, replay the authored structure so
		// every Ladder in this column can shorten to the newly nearest Walkway.
		if (mBuildFinished && !mDeserializingConstruction)
		{
			beginStructuralEdit("addSectorWalkway");
			auto room = _getSector(sectorIndex);
			for (uint32_t i = 0; i < room->getNumObjects(); ++i)
			{
				auto ladderObject = dynamic_pointer_cast<LadderSectorObject>(room->getObject(i));
				if (ladderObject && ladderObject->getCellX() == room->getCellX() + xOffset)
				{
					auto ladder = ladderObject->getLadder();
					auto base = ladderObject->getCellY() - room->getCellY();
					auto top = base + ladder->getDecksHigh() - 1;
					if (deckIndex > base && deckIndex < top && roomLadderIsActive(ladder))
						throw BuildingException(this, "A Room Ladder cannot be resized while it is in use");
				}
				auto liftObject = dynamic_pointer_cast<LiftSectorObject>(room->getObject(i));
				if (liftObject && liftObject->getCellX() == room->getCellX() + xOffset
					&& platformLiftIsActive(liftObject->getLift()))
					throw BuildingException(this, "The PlatformLift shaft cannot be changed while it is in use");
			}
			auto records = mConstructionRecords;
			ConstructionRecord record{ ConstructionType::Walkway };
			record.a = sectorIndex; record.b = deckIndex; record.c = xOffset;
			// A PlatformLift validates authored landing Walkways while replaying, so
			// keep a newly added Walkway before PlatformLifts in the same Room.
			auto beforeLift = find_if(records.begin(), records.end(), [&](auto const& candidate)
				{ return candidate.type == ConstructionType::PlatformLift && candidate.a == sectorIndex; });
			records.insert(beforeLift, record);
			if (!normalizeRoomLadderRecords(records, diagnostic))
				throw BuildingException(this, diagnostic);
			rebuildFromConstructionRecords(std::move(records));
			auto rebuilt = _getSector(sectorIndex);
			for (uint32_t i = 0; i < rebuilt->getNumObjects(); ++i)
			{
				auto object = rebuilt->getObject(i);
				if (object && object->getObjectType() == SectorObjectType::Walkway
					&& object->getCellX() == rebuilt->getCellX() + xOffset
					&& object->getCellY() == rebuilt->getCellY() + deckIndex)
					return { i, SectorObjectType::Walkway, rebuilt };
			}
			throw BuildingException(this, "Could not locate the added Walkway");
		}
		beginStructuralEdit("addSectorWalkway");

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(sector->getCellX() + xOffset,
			sector->getCellY() + deckIndex);
		auto createdWalkway = createWalkway(layerIndex,
			sector->getCellX() + xOffset, sector->getCellY() + deckIndex);

		cellDef.floorIndex = createdWalkway.index;
		cellDef.floorType = CellFloorType::Walkway;
		ConstructionRecord record{ ConstructionType::Walkway };
		record.a = sectorIndex; record.b = deckIndex; record.c = xOffset;
		recordConstruction(std::move(record));
		return createdWalkway;
	}

	bool Building::canAddSectorMarker(uint32_t sectorIndex, uint32_t deckIndex, float xOffset,
		string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (sectorIndex >= mSectors.size()) return reject("Marker sector does not exist");
		auto const& sector = mSectors[sectorIndex];
		if (!sector || !sector->sectorSupportsObjectType(SectorObjectType::Marker))
			return reject("This sector does not support Markers");
		if (deckIndex >= sector->getDecksHigh()) return reject("Marker deck is outside the sector");
		if (!isfinite(xOffset) || xOffset < 0.0f || xOffset >= sector->getSize().x)
			return reject("Marker position is outside the sector");

		auto const cellX = sector->getCellX() + (uint32_t)floor(xOffset);
		auto const cellY = sector->getCellY() + deckIndex;
		auto const& cellDef = mLayers[sector->getLayerIndex()]->getCellDefinition(cellX, cellY);
		if (cellDef.floorType != CellFloorType::Ground
			&& cellDef.floorType != CellFloorType::Walkway)
			return reject("Markers require ground or a Walkway");

		auto const globalX = sector->getCellX() + xOffset;
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto markerObject = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
			if (!markerObject) continue;
			auto marker = markerObject->getMarker();
			if (marker->getCellY() == cellY
				&& fabs(marker->getCellX() + marker->getOffset() - globalX) <= 0.05f)
				return reject("A Marker already exists at this position");
		}

		if (diagnostic) diagnostic->clear();
		return true;
	}

	Building::CreateObjectResult Building::addSectorMarker(uint32_t sectorIndex,
		uint32_t deckIndex, float xOffset, uint32_t* vertexIdentifier)
	{
		string caller = format("Building::addSectorMarker({}, {}, {})", sectorIndex, deckIndex, xOffset);
		string diagnostic;
		if (!canAddSectorMarker(sectorIndex, deckIndex, xOffset, &diagnostic))
			throw BuildingException(this, format("{} - {}", caller, diagnostic));
		beginStructuralEdit("addSectorMarker");

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(sector->getCellX() + (uint32_t)xOffset,
			sector->getCellY() + deckIndex);
		auto createdMarker = createMarker(layerIndex, sector->getCellX(),
			sector->getCellY() + deckIndex, xOffset, vertexIdentifier);
		cellDef.markers.push_back(createdMarker.index);
		ConstructionRecord record{ ConstructionType::Marker };
		record.a = sectorIndex; record.b = deckIndex; record.x = xOffset;
		recordConstruction(std::move(record));
		return createdMarker;
	}

	bool Building::removeSectorMarker(uint32_t sectorIndex, uint32_t objectIndex)
	{
		if (sectorIndex >= mSectors.size()) return false;
		auto sector = _getSector(sectorIndex);
		if (objectIndex >= sector->getNumObjects()) return false;
		auto markerObject = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(objectIndex));
		if (!markerObject) return false;

		beginStructuralEdit("removeSectorMarker");
		auto const marker = markerObject->getMarker();
		auto& cellDef = mLayers[sector->getLayerIndex()]->getCellDefinition(
			marker->getCellX(), marker->getCellY());
		auto const found = find(cellDef.markers.begin(), cellDef.markers.end(), objectIndex);
		if (found == cellDef.markers.end())
			throw BuildingException(this, "removeSectorMarker - Marker is not registered in its cell");
		cellDef.markers.erase(found);
		if (!sector->removeSectorObject(objectIndex)) return false;

		ConstructionRecord record{ ConstructionType::RemoveMarker };
		record.a = sectorIndex;
		record.b = objectIndex;
		recordConstruction(std::move(record));
		return true;
	}

	Building::CreateForceBridgeResult Building::addSectorForceBridge(uint32_t sectorIndex,
		uint32_t deckIndex, uint32_t xOffset)
	{
		return addSectorForceBridge(sectorIndex, deckIndex, xOffset, CreateForceBridgeOptions{});
	}

	bool Building::calculateSectorForceBridgeWidthToRight(uint32_t sectorIndex,
		uint32_t deckIndex, uint32_t xOffset, uint32_t& width, string* diagnostic) const
	{
		width = 0;
		auto reject = [&](string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (sectorIndex >= mSectors.size()) return reject("Force Bridge Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("Force Bridges can only be placed in Rooms");
		if (deckIndex >= room->getDecksHigh() || xOffset == 0 || xOffset >= room->getCellsWide())
			return reject("The Force Bridge and both supports must remain inside its Room");
		auto layer = mLayers[room->getLayerIndex()];
		uint32_t x = room->getCellX() + xOffset, y = room->getCellY() + deckIndex;
		auto const& left = layer->getCellDefinition(x - 1, y);
		if (left.floorType != CellFloorType::Ground && left.floorType != CellFloorType::Walkway)
			return reject("The Force Bridge requires Ground or a Walkway on its left");
		uint32_t roomRight = room->getCellX() + room->getCellsWide();
		for (uint32_t ix = x; ix < roomRight; ++ix)
		{
			auto const floor = layer->getCellDefinition(ix, y).floorType;
			if (floor == CellFloorType::Walkway)
			{
				width = ix - x;
				if (width == 0) return reject("Drop the Force Bridge in the gap before the Walkway");
				if (width > CORE_FORCEBRIDGE_MAX_SIZE)
					return reject(format("The next Walkway is farther than the maximum Force Bridge width of {}", CORE_FORCEBRIDGE_MAX_SIZE));
				if (diagnostic) diagnostic->clear();
				return true;
			}
			if (floor != CellFloorType::None)
				return reject("Another floor object blocks the gap before the next Walkway");
		}
		return reject("No Walkway exists to the right of this gap");
	}

	bool Building::canAddSectorForceBridge(uint32_t sectorIndex, uint32_t deckIndex,
		uint32_t xOffset, CreateForceBridgeOptions const& options, string* diagnostic) const
	{
		auto reject = [&](string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (options.fromSide != CORE_SIDE_LEFT && options.fromSide != CORE_SIDE_RIGHT)
			return reject("Force Bridge extension side is invalid");
		if (options.width == 0 || options.width > CORE_FORCEBRIDGE_MAX_SIZE)
			return reject(format("Force Bridge width must be [1,{}]", CORE_FORCEBRIDGE_MAX_SIZE));
		if (options.extensible && (options.controlCount < 1 || options.controlCount > 2))
			return reject("An extensible Force Bridge requires one or two controls");
		if (!options.extensible && (options.controlCount != 0 || !options.startExtended))
			return reject("A non-extensible Force Bridge must be permanently extended and have no controls");
		if (sectorIndex >= mSectors.size()) return reject("Force Bridge Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("Force Bridges can only be placed in Rooms");
		if (deckIndex >= room->getDecksHigh() || xOffset == 0
			|| (uint64_t)xOffset + options.width >= room->getCellsWide())
			return reject("The Force Bridge and both supports must remain inside its Room");
		auto layer = mLayers[room->getLayerIndex()];
		uint32_t x = room->getCellX() + xOffset, y = room->getCellY() + deckIndex;
		for (uint32_t ix = x; ix < x + options.width; ++ix)
			if (layer->getCellDefinition(ix, y).floorType != CellFloorType::None)
				return reject("The Force Bridge span must be clear air");
		auto supported = [](CellDefinition const& cell)
			{ return cell.floorType == CellFloorType::Ground || cell.floorType == CellFloorType::Walkway; };
		if (!supported(layer->getCellDefinition(x - 1, y)))
			return reject("The Force Bridge requires Ground or a Walkway on its left");
		if (!supported(layer->getCellDefinition(x + options.width, y)))
			return reject("The Force Bridge requires Ground or a Walkway on its right");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	Building::CreateForceBridgeResult Building::addSectorForceBridge(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateForceBridgeOptions const& options)
	{
		string caller = format("Building::addSectorForceBridge({}, {}, {}, {}, {}, {})", sectorIndex, deckIndex, xOffset, options.width, options.fromSide, options.startExtended);
		string diagnostic;
		if (!canAddSectorForceBridge(sectorIndex, deckIndex, xOffset, options, &diagnostic))
			throw BuildingException(this, format("{} - {}", caller, diagnostic));
		beginStructuralEdit("addSectorForceBridge");
		ASSERT_SIDE_OK(options.fromSide);
		validateSectorForceBridgeOptions(caller, options);

		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + deckIndex;
		auto layer = getLayer(layerIndex);

		// Create and mark every cell in the authored span as one floor object.
		auto fbObject = createForceBridge(layerIndex, x, y, options);
		for (uint32_t ix = x; ix < x + options.width; ++ix)
		{
			auto& cellDef = layer->getCellDefinition(ix, y);
			cellDef.floorIndex = fbObject.index;
			cellDef.floorType = CellFloorType::ForceBridge;
		}

		auto forceBridge = dynamic_pointer_cast<ForceBridgeSectorObject>(
			fbObject.sector->_getObject(fbObject.index))->getForceBridge();
		auto traversalResource = createForceBridgeTraversalResource("Force bridge", forceBridge);
		forceBridge->configureTraversal(traversalResource);
		auto const halfAgentWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
		configureForceBridgeQueueLanes(traversalResource,
			SectorId{ (uint64_t)sector->getIndex() + 1 },
			{ Vector2{ (float)x - halfAgentWidth, (float)y },
				Vector2{ (float)(x + options.width) + halfAgentWidth, (float)y } });

		// See if a physical control is needed
		CreateObjectResult createdControls[2];

		if (options.extensible)
		{
			if (x == sector->getCellX0() && (x + options.width - 1) == sector->getCellX1())
			{
				throw BuildingException(this, format("{} - No space to place Force Bridge controls", caller));
			}

			if (options.controlCount > 0)
			{
				createdControls[0] = _createForceBridgeButton(fbObject.sector, x, y, options.width, options.fromSide, 0);
				DeviceCommand command{ DeviceCommandType::SetExtendedState, {}, true, traversalResource };
				auto point = createPhysicalControlInteractionPoint("Force bridge extension control",
					createdControls[0], (float)y, CORE_AGENT_MAX_WIDTH * 0.5f + 0.001f,
					getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
				addTraversalControl(traversalResource, point);
			}
			if (options.controlCount > 1)
			{
				createdControls[1] = _createForceBridgeButton(fbObject.sector, x, y, options.width, 1 - options.fromSide, 0);
				DeviceCommand command{ DeviceCommandType::SetExtendedState, {}, true, traversalResource };
				auto point = createPhysicalControlInteractionPoint("Force bridge extension control",
					createdControls[1], (float)y, CORE_AGENT_MAX_WIDTH * 0.5f + 0.001f,
					getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
				addTraversalControl(traversalResource, point);
			}
		}
		// Keep the aggregate at three authored object slots (bridge plus two
		// controls) so changing control count cannot shift unrelated object indices.
		for (uint32_t i = options.controlCount; i < 2; ++i)
			sector->addSectorObject(nullptr);

		CreateForceBridgeResult result{
			fbObject,
			{ createdControls[0], createdControls[1] },
			traversalResource
		};
		ConstructionRecord record{ ConstructionType::ForceBridge };
		record.a = sectorIndex; record.b = deckIndex; record.c = xOffset; record.d = options.width;
		record.i = options.fromSide; record.p = options.extensible; record.q = options.startExtended;
		record.e = options.controlCount;
		recordConstruction(std::move(record));
		return result;
	}

	bool Building::canAddRoomLadder(uint32_t sectorIndex, uint32_t deckIndex,
		uint32_t xOffset, uint32_t* decksHigh, string* diagnostic) const
	{
		auto reject = [&](string reason)
		{
			if (decksHigh) *decksHigh = 0;
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (sectorIndex >= mSectors.size()) return reject("Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("Room Ladders can only be placed in Rooms");
		if (xOffset >= room->getCellsWide() || deckIndex >= room->getDecksHigh())
			return reject("Ladder base is outside the Room");

		auto const x = room->getCellX() + xOffset;
		auto const y = room->getCellY() + deckIndex;
		auto const& base = mLayers[room->getLayerIndex()]->getCellDefinition(x, y);
		if (base.floorType != CellFloorType::Ground && base.floorType != CellFloorType::Walkway)
			return reject("Drop the Room Ladder on Ground or a Walkway");

		uint32_t topDeck = ~0u;
		for (uint32_t offset = deckIndex + 1; offset < room->getDecksHigh(); ++offset)
		{
			auto const& cell = mLayers[room->getLayerIndex()]->getCellDefinition(
				x, room->getCellY() + offset);
			if (cell.floorType == CellFloorType::Walkway) { topDeck = offset; break; }
		}
		if (topDeck == ~0u) return reject("No Walkway exists above this position");

		uint32_t const topY = room->getCellY() + topDeck;
		for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		{
			auto object = room->getObject(i);
			if (!object || object->getObjectType() == SectorObjectType::Walkway) continue;
			uint32_t const objectX0 = object->getCellX();
			uint32_t const objectX1 = objectX0 + (uint32_t)ceil(object->getSize().x) - 1;
			if (x < objectX0 || x > objectX1) continue;
			uint32_t const objectY0 = object->getCellY();
			uint32_t const objectY1 = objectY0 + (uint32_t)ceil(object->getSize().y) - 1;
			if (object->getObjectType() == SectorObjectType::Ladder)
			{
				// Two independently authored ladders may meet, but never overlap.
				if (max(y, objectY0) < min(topY, objectY1))
					return reject("Another Ladder overlaps this Ladder's interior");
			}
			else if (max(y, objectY0) <= min(topY, objectY1))
				return reject("Another object blocks the Ladder");
		}
		if (decksHigh) *decksHigh = topDeck - deckIndex + 1;
		if (diagnostic) diagnostic->clear();
		return true;
	}

	Building::CreateLadderResult Building::addRoomLadder(uint32_t sectorIndex,
		uint32_t deckIndex, uint32_t xOffset)
	{
		return addRoomLadder(sectorIndex, deckIndex, xOffset, { 0, false, true });
	}

	Building::CreateLadderResult Building::addRoomLadder(uint32_t sectorIndex,
		uint32_t deckIndex, uint32_t xOffset, CreateLadderOptions options)
	{
		uint32_t height{}; string diagnostic;
		if (!canAddRoomLadder(sectorIndex, deckIndex, xOffset, &height, &diagnostic))
			throw BuildingException(this, format("Building::addRoomLadder - {}", diagnostic));
		options.decksHigh = height;
		return addSectorLadder(sectorIndex, deckIndex, xOffset, options);
	}

	Building::CreateLadderResult Building::addSectorLadder(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateLadderOptions const& options)
	{
		beginStructuralEdit("addSectorLadder");
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

		auto const& baseCell = mLayers[layerIndex]->getCellDefinition(x, y);
		// Stacked Room Ladders may share exactly one Walkway endpoint.
		if (baseCell.hasObject() && baseCell.sectorObjectType != SectorObjectType::Ladder)
			validateCellHasNoObject(caller, layerIndex, x, y);
		// Authored replays may encounter the Ladder before a later Walkway record;
		// the normalized construction set guarantees both endpoint floors exist by
		// finishBuild(). Interactive creation still validates immediately.
		if (!mDeserializingConstruction)
		{
			validateCellTraversableOnFoot(caller, "Ladder", layerIndex, x, y0);
			validateCellTraversableOnFoot(caller, "Ladder", layerIndex, x, y1);
		}

		// Create
		auto layer = getLayer(layerIndex);

		auto ladderObject = createLadderSectorObject(layerIndex, x, y, options);
		auto ladder = dynamic_pointer_cast<LadderSectorObject>(
			ladderObject.sector->_getObject(ladderObject.index))->getLadder();
		auto traversalResource = createLadderTraversalResource("Ladder capacity", ladder,
			SectorId{ (uint64_t)sectorIndex + 1 }, options.directionalBatchLimit);
		auto const location = SectorId{ (uint64_t)sectorIndex + 1 };
		configureLadderQueueLanes(traversalResource, { location, location },
			{ Vector2{ (float)x + 0.5f, (float)y0 },
				Vector2{ (float)x + 0.5f, (float)y1 } });
		ladder->configureTraversal(traversalResource);
		auto registerExtensionControl = [&](CreateObjectResult& control)
		{
			auto object = control.sector->_getObject(control.index);
			DeviceCommand command;
			command.type = DeviceCommandType::SetExtendedState;
			command.desiredState = true;
			command.traversalResource = traversalResource;
			auto point = createPhysicalControlInteractionPoint("Ladder extension control",
				control, (float)object->getCellY(), 0.15f, getFixedTimestep(),
				{ { command, InteractionBindingRequirement::Required } });
			addTraversalControl(traversalResource, point);
		};

		for (uint32_t iy = y0; iy <= y1; ++iy)
		{
			auto& cellDef = layer->getCellDefinition(x, iy);

			cellDef.sectorObjectType = SectorObjectType::Ladder;
			cellDef.sectorObjectIndex = ladderObject.index;
		}

		// See if a physical control is needed
		CreateObjectResult createdControls[2];

		if (options.extensible)
		{
			if (x == sector->getCellX0() && x == sector->getCellX1())
			{
				throw BuildingException(this, format("{} - No space to place Buttons for Ladder", caller));
			}

			// Try and place on the right of the Ladder, unless it's at the end of the Location
			int side = x == sector->getCellX1() ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;

			// Lower
			createdControls[CORE_LEVEL_LOW] = _createLadderButton(
				ladderObject.sector, x, y0, side, 0, nullptr, true);
			registerExtensionControl(createdControls[CORE_LEVEL_LOW]);


			// Upper
			createdControls[CORE_LEVEL_HIGH] = _createLadderButton(
				ladderObject.sector, x, y1, side, 0, nullptr, true);
			registerExtensionControl(createdControls[CORE_LEVEL_HIGH]);

		}
		else
		{
			// Reserve the two control slots so toggling extensibility does not shift
			// independently authored object indices in this Room.
			sector->addSectorObject(nullptr);
			sector->addSectorObject(nullptr);
		}

		CreateLadderResult result{
			ladderObject,
			{ createdControls[CORE_LEVEL_LOW], createdControls[CORE_LEVEL_HIGH] },
			traversalResource
		};
		ConstructionRecord record{ ConstructionType::SectorLadder };
		record.a = sectorIndex; record.b = deckIndex; record.c = xOffset;
		record.d = options.decksHigh; record.e = options.directionalBatchLimit;
		record.p = options.extensible; record.q = options.startExtended;
		recordConstruction(std::move(record));
		return result;
	}

	vector<Building::PlatformLiftStopCandidate> Building::getPlatformLiftStopCandidates(
		uint32_t sectorIndex, uint32_t xOffset) const
	{
		vector<PlatformLiftStopCandidate> result;
		if (sectorIndex >= mSectors.size()) return result;
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor() || xOffset >= room->getCellsWide()) return result;
		auto layer = mLayers[room->getLayerIndex()];
		auto x = room->getCellX() + xOffset;
		auto sideAvailable = [&](uint32_t y, int side)
		{
			if (side == CORE_SIDE_LEFT)
				return x > room->getCellX0() + 1 && layer->getCellDefinition(x - 1, y).isTraversableOnFoot();
			return x + 1 < room->getCellX1() && layer->getCellDefinition(x + 1, y).isTraversableOnFoot();
		};
		for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		{
			auto walkway = dynamic_pointer_cast<const WalkwaySectorObject>(room->getObject(i));
			if (!walkway || walkway->getCellX() != x || walkway->getCellY() <= room->getCellY()) continue;
			auto deck = walkway->getCellY() - room->getCellY();
			if (deck >= room->getDecksHigh()) continue;
			result.push_back({ deck, sideAvailable(walkway->getCellY(), CORE_SIDE_LEFT),
				sideAvailable(walkway->getCellY(), CORE_SIDE_RIGHT) });
		}
		sort(result.begin(), result.end(), [](auto const& a, auto const& b)
			{ return a.deckOffset < b.deckOffset; });
		return result;
	}

	bool Building::canAddPlatformLift(uint32_t sectorIndex, uint32_t xOffset,
		CreateLiftOptions const& requested, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (sectorIndex >= mSectors.size()) return reject("PlatformLift Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("PlatformLifts can only be placed in Rooms");
		if (xOffset >= room->getCellsWide()) return reject("PlatformLift position is outside the Room");
		if (requested.cellsWide != 1) return reject("Editor PlatformLifts are one cell wide");
		if (requested.platformStopDurationSeconds < 0.0f)
			return reject("PlatformLift stop duration cannot be negative");
		auto stops = requested.stopOffsets;
		sort(stops.begin(), stops.end());
		stops.erase(unique(stops.begin(), stops.end()), stops.end());
		if (stops.size() < 2 || stops.front() != 0)
			return reject("A PlatformLift requires ground and at least one Walkway stop");
		auto candidates = getPlatformLiftStopCandidates(sectorIndex, xOffset);
		bool left = true, right = true;
		auto x = room->getCellX() + xOffset;
		auto layer = mLayers[room->getLayerIndex()];
		left = x > room->getCellX0() + 1
			&& layer->getCellDefinition(x - 1, room->getCellY()).isTraversableOnFoot();
		right = x + 1 < room->getCellX1()
			&& layer->getCellDefinition(x + 1, room->getCellY()).isTraversableOnFoot();
		for (size_t i = 1; i < stops.size(); ++i)
		{
			auto found = find_if(candidates.begin(), candidates.end(), [&](auto const& candidate)
				{ return candidate.deckOffset == stops[i]; });
			if (found == candidates.end())
				return reject(format("No Walkway exists at deck {} in the PlatformLift column", stops[i]));
			left = left && found->leftButton;
			right = right && found->rightButton;
		}
		if (!left && !right) return reject("The selected stops have no common side for PlatformLift buttons");
		uint32_t top = stops.back();
		for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		{
			auto object = room->getObject(i);
			if (!object || object->getObjectType() == SectorObjectType::Walkway) continue;
			uint32_t objectRight = object->getCellX() + (uint32_t)ceil(object->getSize().x);
			uint32_t objectTop = object->getCellY() + (uint32_t)ceil(object->getSize().y);
			if (x >= object->getCellX() && x < objectRight
				&& room->getCellY() < objectTop && room->getCellY() + top >= object->getCellY())
				return reject("Another object blocks the PlatformLift shaft");
		}
		for (uint32_t deck = 0; deck <= top; ++deck)
			if (!layer->getCellDefinition(x, room->getCellY() + deck).markers.empty())
				return reject("A Marker blocks the PlatformLift shaft");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	Building::CreatePlatformLiftResult Building::addSectorPlatformLift(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateLiftOptions const& options)
	{
		string placementDiagnostic;
		if (deckIndex != 0)
			throw BuildingException(this, "PlatformLifts must be placed on a Room's ground floor");
		if (!canAddPlatformLift(sectorIndex, xOffset, options, &placementDiagnostic))
			throw BuildingException(this, placementDiagnostic);
		beginStructuralEdit("addSectorPlatformLift");
		auto sector = _getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = getLayer(layerIndex);

		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + deckIndex;

		// Checks
		string caller = format("Building::addSectorPlatformLift({}, {}, {})", sectorIndex, deckIndex, xOffset);

		validateLiftOptions(caller, options);
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
				// An upper platform stop normally replaces a walkway tile. The lift
				// remains the cell's traversable object after construction.
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

		CreatePlatformLiftResult liftRes;
		liftRes.lift = liftObject;
		for (auto stopOffset : options.stopOffsets)
		{
			CreateObjectResult control = _createPlatformLiftButton(sector, x,
				y + stopOffset, options.cellsWide, side, CORE_BUTTON_F_AUTO_REENABLE,
				&control.index);
			liftRes.buttons.push_back(control);
		}

		// Platform lifts use the same manifest, dwell, cutoff, destination and LOOK
		// policies as enclosed lifts. Their stops deliberately have no door resource:
		// the coordinator owns a virtual boarding boundary instead.
		auto lift = dynamic_pointer_cast<LiftSectorObject>(
			liftObject.sector->_getObject(liftObject.index))->getLift();
		vector<LiftStop> stops;
		for (auto stopOffset : options.stopOffsets)
			stops.push_back({ SectorId{ (uint64_t)sector->getIndex() + 1 },
				(float)(y + stopOffset), {}, {} });
		auto coordinator = createOpenPlatformLiftTraversalResource("Open platform lift journey", lift,
			SectorId{ (uint64_t)sector->getIndex() + 1 }, stops, options.capacity,
			options.platformStopDurationSeconds);
		lift->configureTraversal(coordinator);
		liftRes.traversalResource = coordinator;
		auto resource = mTraversalResources.find(coordinator);

		// Open platforms have no landing Door resource, so their coordinator owns
		// one physical waiting lane per stop. The positions use the same queue-ticket
		// allocator, spacing, progress tracking, and cancellation cleanup as Doors,
		// Lifts, and Ladders.
		resource->mQueueLanes.clear();
		resource->mQueueLanes.resize(stops.size());
		auto const halfAgentWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
		auto const queueDirection = side == CORE_SIDE_RIGHT ? 1.0f : -1.0f;
		for (uint32_t stop = 0; stop < stops.size(); ++stop)
		{
			auto& lane = resource->mQueueLanes[stop];
			lane.sector = SectorId{ (uint64_t)sector->getIndex() + 1 };
			lane.origin = { side == CORE_SIDE_RIGHT ? (float)(x + options.cellsWide) : (float)x,
				stops[stop].globalPosition };
			lane.direction = Vector2::UNIT_X * queueDirection;
			for (uint32_t step = 0; ; ++step)
			{
				auto distance = halfAgentWidth + step * (float)CORE_DOOR_QUEUE_STOP_WIDTH;
				auto position = lane.origin + lane.direction * distance;
				if (position.x - halfAgentWidth < sector->getCellX0() - 0.001f
					|| position.x + halfAgentWidth > sector->getCellX1() + 1.0f + 0.001f)
					break;
				auto cellX = min(sector->getCellX1(), (uint32_t)floor(position.x));
				auto cellY = min(sector->getCellY1(), (uint32_t)floor(position.y));
				if (!layer->getCellDefinition(cellX, cellY).isTraversableOnFoot()) break;
				lane.positions.push_back(position);
			}
			lane.positionOwners.resize(lane.positions.size());
			lane.extent = lane.positions.empty() ? 0.0f
				: lane.origin.distanceTo(lane.positions.back());
		}

		auto standingWidth = CORE_AGENT_MAX_WIDTH * options.capacity;
		auto standingStart = x + (options.cellsWide - standingWidth) * 0.5f
			+ CORE_AGENT_MAX_WIDTH * 0.5f - sector->getPosition().x;
		for (uint32_t i = 0; i < options.capacity; ++i)
			resource->mCapacityPositions[i] = { standingStart + CORE_AGENT_MAX_WIDTH * i, 0.0f };

		for (uint32_t i = 0; i < liftRes.buttons.size(); ++i)
		{
			auto& buttonResult = liftRes.buttons[i];
			DeviceCommand call;
			call.type = DeviceCommandType::CallLift;
			call.traversalResource = coordinator;
			call.stopIndex = i;
			auto callPoint = createPhysicalControlInteractionPoint("Platform lift landing call",
				buttonResult, stops[i].globalPosition, 0.15f, getFixedTimestep(),
				{ { call, InteractionBindingRequirement::Required } });
			resource->mLiftStops[i].callControl = callPoint;

			DeviceCommand select;
			select.type = DeviceCommandType::SelectLiftDestination;
			select.traversalResource = coordinator;
			select.stopIndex = i;
			auto selector = createInteractionPoint("Platform lift destination selector",
				stops[i].locationSector, { x + options.cellsWide * 0.5f, resource->mLiftPosition },
				0.25f, getFixedTimestep(), { { select, InteractionBindingRequirement::Required } });
			resource->mControls.push_back(selector);
			if (i == 0) liftRes.interiorSelector = selector;
		}
		resource->mLiftSelector = liftRes.interiorSelector;

		ConstructionRecord record{ ConstructionType::PlatformLift };
		record.a = sectorIndex; record.b = deckIndex; record.c = xOffset;
		record.d = options.cellsWide; record.e = options.capacity;
		record.z = options.platformStopDurationSeconds;
		record.values = options.stopOffsets;
		recordConstruction(std::move(record));
		return liftRes;
	}

	void Building::beginStructuralEdit(string const& operation)
	{
		if (mBuildFinished && !mSimulationPaused)
		{
			throw BuildingException(this, format(
				"{} is a structural edit and requires pauseSimulation() before it can run", operation));
		}
		modify();
		mTopologyDirty = true;
		mTopologyValid = false;
		mTopologyDiagnostic = "Traversal topology has unvalidated structural edits";
	}

	// Topology event publication and the teardown of live traversal for a topology
	// rebuild live in SimulationCoordinator (ADR 0004 stage 5); Building's
	// pause/resume protocol calls them.

	// Pause and resume are the edit/simulation boundary and stay here, on the
	// structural-edit side of that boundary (ADR 0004 stage 5): they belong to
	// the same contract as beginStructuralEdit, finishBuild and
	// rebuildTraversalTopology, which refuses a structural edit unless the
	// simulation is paused and refuses a resume over dirty topology. The
	// simulation-side work they perform - tearing down every live traversal,
	// remembering each Agent's route intent, restoring those routes onto the new
	// graph and publishing the boundary events - lives in SimulationCoordinator.
	void Building::pauseSimulation()
	{
		if (mSimulationPaused) return;
		if (mCurrentPhase != SimulationPhase::None)
			throw BuildingException(this, "Simulation cannot be paused from inside a simulation phase");

		mSimulationPaused = true;
		mAccumulatedTime = 0.0;
		mSimulationCoordinator.cancelAllTraversalForTopologyRebuild();
		mSimulationCoordinator.publishTopologyEvent(SimulationEventType::SimulationPaused);
	}

	bool Building::getPausedPathIntent(Agent const& agent, TopologyPathIntent& intent) const
	{
		for (auto const& [id, candidate] : mPausedPathIntents)
		{
			if (mAgents.find(id) == &agent)
			{
				intent = candidate;
				return true;
			}
		}
		return false;
	}

	void Building::validateTraversalTopology(Graph const& graph) const
	{
		auto validSector = [&](SectorId id) { return id && id.value <= mSectors.size(); };
		auto require = [&](bool condition, string const& diagnostic)
		{
			if (!condition) throw BuildingException(this, diagnostic);
		};
		require(mTraversalRequests.entries().empty() && mTraversalPermits.entries().empty(),
			"Traversal requests and permits must be drained before topology replacement");

		for (auto const& edge : graph.getEdges())
		{
			auto id = edge->getTraversalResourceId();
			bool requiresAuthority = edge->getType() == EdgeType::Door
				|| edge->getType() == EdgeType::BulkheadDoor || edge->getType() == EdgeType::Window
				|| edge->getType() == EdgeType::ForceBridge || edge->getType() == EdgeType::Ladder
				|| edge->getType() == EdgeType::LadderMount || edge->getType() == EdgeType::Lift
				|| edge->getType() == EdgeType::LiftMount || edge->getType() == EdgeType::Shuttle
				|| edge->getType() == EdgeType::ShuttleMount;
			require(!requiresAuthority || id,
				format("Edge {} ({}) has no traversal authority", edge->getId(), edge->getDescription()));
			if (!id) continue; // Explicit immediate-permit policy.
			auto resource = mTraversalResources.find(id);
			require(resource != nullptr, format("Edge {} references removed traversal resource {}",
				edge->getId(), id.value));
			bool compatible = edge->getType() == EdgeType::Door || edge->getType() == EdgeType::BulkheadDoor
				? resource->mDoor != nullptr
				: edge->getType() == EdgeType::Window ? resource->mWindow != nullptr
				: edge->getType() == EdgeType::ForceBridge ? resource->mForceBridge != nullptr
				: edge->getType() == EdgeType::Ladder || edge->getType() == EdgeType::LadderMount
					? resource->mLadder != nullptr
				: edge->getType() == EdgeType::Stairwell || edge->getType() == EdgeType::StairwellMount
					? resource->mStairwell != nullptr
				: edge->getType() == EdgeType::Lift || edge->getType() == EdgeType::LiftMount
					? resource->mLift != nullptr
				: edge->getType() == EdgeType::Shuttle || edge->getType() == EdgeType::ShuttleMount
					? resource->mShuttle != nullptr : true;
			require(compatible, format("Edge {} references an incompatible traversal resource {}",
				edge->getId(), id.value));
		}

		for (auto const& [id, resourcePtr] : mTraversalResources.entries())
		{
			auto const& resource = *resourcePtr;
			if (resource.mDoor || resource.mForceBridge)
			{
				if (resource.mDoor) require(resource.mDoor->getTraversalResourceId() == id,
					format("Door resource {} is not the door's sole configured authority", id.value));
				auto const maximumCrossingLanes = resource.mDoor
					? resource.mDoor->getCellsWide() : 1u;
				require(!resource.mCrossingOwners.empty()
					&& resource.mCrossingOwners.size() <= maximumCrossingLanes,
					format("Queued crossing resource {} has invalid crossing-lane geometry", id.value));
				set<SectorId> approachSectors;
				for (auto const& lane : resource.mQueueLanes)
				{
					if (!lane.sector) continue;
					require(validSector(lane.sector)
						&& (!resource.mDoor || approachSectors.insert(lane.sector).second),
						format("Queued crossing resource {} has invalid approach sectors", id.value));
					require(lane.positions.size() == lane.positionOwners.size() && !lane.positions.empty(),
						format("Queued crossing resource {} has invalid queue-position storage", id.value));
					auto sector = mSectors[(size_t)lane.sector.value - 1];
					for (auto const& position : lane.positions)
						require(isfinite(position.x) && isfinite(position.y)
							&& position.x - CORE_AGENT_MAX_WIDTH * 0.5f >= sector->getCellX0() - 0.001f
							&& position.x + CORE_AGENT_MAX_WIDTH * 0.5f <= sector->getCellX1() + 1.001f
							&& position.y >= sector->getCellY0() - 0.001f
							&& position.y + CORE_AGENT_MAX_HEIGHT <= sector->getCellY1() + 1.001f,
							format("Queued crossing resource {} has a queue position outside its approach sector", id.value));
				}
			}
			if (resource.mOpenPlatformLift)
			{
				require(resource.mQueueLanes.size() == resource.mLiftStops.size(),
					format("Platform-lift resource {} does not have one queue lane per stop", id.value));
				for (uint32_t stop = 0; stop < resource.mQueueLanes.size(); ++stop)
				{
					auto const& lane = resource.mQueueLanes[stop];
					require(lane.sector == resource.mLiftSector && validSector(lane.sector)
						&& lane.positions.size() == lane.positionOwners.size()
						&& !lane.positions.empty(),
						format("Platform-lift resource {} has invalid queue geometry at stop {}",
							id.value, stop));
					auto sector = mSectors[(size_t)lane.sector.value - 1];
					for (auto const& position : lane.positions)
						require(isfinite(position.x) && isfinite(position.y)
							&& abs(position.y - resource.mLiftStops[stop].globalPosition) <= 0.001f
							&& position.x - CORE_AGENT_MAX_WIDTH * 0.5f
								>= sector->getCellX0() - 0.001f
							&& position.x + CORE_AGENT_MAX_WIDTH * 0.5f
								<= sector->getCellX1() + 1.001f,
							format("Platform-lift resource {} has an invalid queue position at stop {}",
								id.value, stop));
				}
			}
			if (resource.mWindow)
				require(resource.mWindow->getTraversalResourceId() == id,
					format("Window resource {} is not the window's configured authority", id.value));
			if (resource.mLadder)
				require(resource.mLadder->getTraversalResourceId() == id,
					format("Ladder resource {} is not the ladder's configured authority", id.value));
			if (resource.mStairwell)
				require(resource.mStairwell->getTraversalResourceId() == id,
					format("Stairwell resource {} is not the stairwell's configured authority", id.value));
			if (resource.mForceBridge)
				require(resource.mForceBridge->getTraversalResourceId() == id,
					format("Force-bridge resource {} is not the bridge's configured authority", id.value));
			if (resource.mLift)
				require(resource.mLift->getTraversalResourceId() == id,
					format("Lift resource {} is not the lift's configured authority", id.value));
			if (resource.mShuttle)
				require(resource.mShuttle->getTraversalResourceId() == id,
					format("Shuttle resource {} is not the shuttle's configured authority", id.value));

			if (resource.mCapacity)
			{
				require(resource.mCapacityPositions.size() == resource.mCapacity
					&& resource.mOccupants.size() == resource.mCapacity
					&& resource.mAdmissionReservations.size() == resource.mCapacity,
					format("Traversal resource {} has inconsistent capacity positions", id.value));
				for (uint32_t i = 0; i < resource.mCapacityPositions.size(); ++i)
				{
					auto const& position = resource.mCapacityPositions[i];
					require(isfinite(position.x) && isfinite(position.y),
						format("Traversal resource {} has a non-finite capacity position", id.value));
					require(!resource.mOccupants[i] || mAgents.find(resource.mOccupants[i]),
						format("Traversal resource {} contains a removed manifest occupant", id.value));
					require(!resource.mAdmissionReservations[i]
						|| mTraversalRequests.find(resource.mAdmissionReservations[i]),
						format("Traversal resource {} contains a stale admission reservation", id.value));
					for (uint32_t j = 0; j < i; ++j)
						require(position.distanceTo(resource.mCapacityPositions[j]) > 0.001f,
							format("Traversal resource {} has overlapping capacity positions", id.value));
				}
			}
			for (auto requestId : resource.mAdmissionQueue)
				require(mTraversalRequests.find(requestId) != nullptr,
					format("Traversal resource {} contains a stale admission queue entry", id.value));
			for (auto owner : resource.mVirtualBoundaryOwners)
				require(!owner || mTraversalRequests.find(owner),
					format("Traversal resource {} contains a stale boundary owner", id.value));
			for (auto const& [leaseId, lease] : resource.mOpenLeases)
			{
				(void)leaseId;
				require(lease.kind == DoorOpenLeaseKind::ExternalHoldOpen || (lease.request
					&& mTraversalRequests.find(lease.request)),
					format("Traversal resource {} contains a stale open lease", id.value));
			}

			for (auto controlId : resource.mControls)
			{
				auto control = mInteractionPoints.find(controlId);
				require(control != nullptr, format("Traversal resource {} references removed control {}",
					id.value, controlId.value));
				auto expected = resource.mLiftCoordinator ? resource.mLiftCoordinator : id;
				require(any_of(control->mBindings.begin(), control->mBindings.end(), [&](auto const& binding)
					{ return binding.command.traversalResource == expected; }),
					format("Control {} does not target traversal resource {}", controlId.value, expected.value));
				if (resource.mDoor)
					require(any_of(resource.mQueueLanes.begin(), resource.mQueueLanes.end(),
						[&](auto const& lane) { return lane.sector == control->mSector; }),
						format("Control {} is unreachable from resource {} approaches", controlId.value, id.value));
				if (resource.mLift || resource.mShuttle)
					require(control->mSector == resource.mLiftSector,
						format("Transport selector {} is outside resource {}", controlId.value, id.value));
			}

			if (resource.mLift || resource.mShuttle)
			{
				require(resource.mLiftStops.size() >= 2 && validSector(resource.mLiftSector),
					format("Transport resource {} has invalid stops or transit sector", id.value));
				for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
				{
					auto const& value = resource.mLiftStops[stop];
					require(validSector(value.locationSector) && isfinite(value.globalPosition)
						&& (stop == 0 || value.globalPosition > resource.mLiftStops[stop - 1].globalPosition),
						format("Transport resource {} has invalid stop {} geometry", id.value, stop));
					if (!resource.mOpenPlatformLift)
					{
						auto landing = mTraversalResources.find(value.landingResource);
						require(landing && landing->mDoor && landing->mLiftCoordinator == id
							&& landing->mLiftStopIndex == stop,
							format("Transport resource {} has invalid landing-door mapping at stop {}", id.value, stop));
					}
					auto call = mInteractionPoints.find(value.callControl);
					require(call && call->mSector == value.locationSector
						&& any_of(call->mBindings.begin(), call->mBindings.end(), [&](auto const& binding)
							{ return binding.command.traversalResource == id
								&& binding.command.stopIndex == stop; }),
						format("Transport resource {} has an invalid landing control at stop {}", id.value, stop));
				}
				if (resource.mShuttle)
					for (auto const& door : resource.mShuttleDoors)
					{
						auto landing = mTraversalResources.find(door.landingResource);
						require(door.stopIndex < resource.mLiftStops.size()
							&& door.carriageIndex < resource.mShuttleCarriages.size()
							&& validSector(door.locationSector) && landing && landing->mDoor
							&& landing->mLiftCoordinator == id && landing->mLiftStopIndex == door.stopIndex,
							format("Shuttle resource {} has an invalid carriage-door mapping", id.value));
					}
			}
		}

		for (auto const& [pointId, point] : mInteractionPoints.entries())
		{
			require(validSector(point->mSector),
				format("Interaction point {} has an invalid sector", pointId.value));
			for (auto const& binding : point->mBindings)
				if (binding.command.type != DeviceCommandType::SetSectorLights)
					require(mTraversalResources.find(binding.command.traversalResource) != nullptr,
						format("Interaction point {} targets removed traversal resource {}",
							pointId.value, binding.command.traversalResource.value));
		}
	}


	void Building::buildGraph()
	{
		mGraph->build();
		mGraph->validate();
		validateTraversalTopology(*mGraph);
	}

	bool Building::rebuildTraversalTopology()
	{
		if (!mBuildFinished)
		{
			mTopologyDiagnostic = "finishBuild() must establish the initial topology";
			return false;
		}
		if (!mSimulationPaused)
		{
			mTopologyDiagnostic = "Traversal topology can only be rebuilt while the simulation is paused";
			return false;
		}

		auto candidate = make_shared<Graph>(this);
		try
		{
			candidate->build();
			candidate->validate();
			validateTraversalTopology(*candidate);
			mGraph = std::move(candidate);
			mTopologyDirty = false;
			mTopologyValid = true;
			mTopologyDiagnostic.clear();
			++mTopologyGeneration;
			mSimulationCoordinator.restorePausedPathIntents();
			auto const& graphLog = mGraph->getBuildLog();
			mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());
			mSimulationCoordinator.publishTopologyEvent(SimulationEventType::TopologyRebuilt);
			return true;
		}
		catch (Exception const& error)
		{
			mTopologyDirty = true;
			mTopologyValid = false;
			mTopologyDiagnostic = error.getMessage();
			auto const& graphLog = candidate->getBuildLog();
			mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());
			mBuildLog.push_back({ "Topology rebuild", ~0u, LogLevel::Error, mTopologyDiagnostic });
			mSimulationCoordinator.publishTopologyEvent(SimulationEventType::TopologyRebuildFailed, mTopologyDiagnostic);
			return false;
		}
		catch (exception const& error)
		{
			mTopologyDirty = true;
			mTopologyValid = false;
			mTopologyDiagnostic = error.what();
			mBuildLog.push_back({ "Topology rebuild", ~0u, LogLevel::Error, mTopologyDiagnostic });
			mSimulationCoordinator.publishTopologyEvent(SimulationEventType::TopologyRebuildFailed, mTopologyDiagnostic);
			return false;
		}
	}

	bool Building::resumeSimulation()
	{
		if (!mSimulationPaused) return true;
		if (mTopologyDirty || !mTopologyValid)
		{
			if (mTopologyDiagnostic.empty())
				mTopologyDiagnostic = "Traversal topology contains unvalidated structural edits";
			return false;
		}
		mSimulationCoordinator.restorePausedPathIntents();
		mSimulationPaused = false;
		mAccumulatedTime = 0.0;
		mSimulationCoordinator.publishTopologyEvent(SimulationEventType::SimulationResumed);
		return true;
	}

	void Building::finishBuild()
	{
		if (mBuildFinished)
		{
			if (!mSimulationPaused)
				throw BuildingException(this, "A finished building must be paused before rebuilding topology");
			if (!rebuildTraversalTopology()) throw BuildingException(this, mTopologyDiagnostic);
			return;
		}
		try
		{
			buildGraph();
			mBuildFinished = true;
			mTopologyDirty = false;
			mTopologyValid = true;
			mTopologyDiagnostic.clear();
			++mTopologyGeneration;
		}
		catch(Exception const& e)
		{
			auto const& graphLog = mGraph->getBuildLog();
			mBuildLog.insert(mBuildLog.end(), graphLog.begin(), graphLog.end());
			mTopologyValid = false;
			mTopologyDiagnostic = e.getMessage();
			throw;
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

	shared_ptr<const Object> Building::getObjectAtPosition(uint32_t layerIndex, float x, float y,
		shared_ptr<const SectorObject>* sectorObject) const
	{
		// An open platform is deliberately rendered a little below its nominal
		// floor. Hit-test its current geometry before resolving the pointer through
		// a grid cell, because that rendered strip may lie in the cell below its Room.
		for (auto const& sector : getSectors(layerIndex))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto liftObject = dynamic_pointer_cast<const LiftSectorObject>(sector->getObject(i));
				if (!liftObject) continue;
				Vector2 first, second;
				liftObject->getLift()->getCurrentShape(first, second);
				auto minX = min(first.x, second.x), maxX = max(first.x, second.x);
				auto minY = min(first.y, second.y), maxY = max(first.y, second.y);
				if (x < minX || x > maxX || y < minY || y > maxY) continue;
				if (sectorObject) *sectorObject = liftObject;
				return liftObject->getLift();
			}
		try
		{
			auto const& cell = mLayers[layerIndex]->getCellDefinition((int)x, (int)y);
			if (!cell.occupied()) return nullptr;
			auto sector = getSector(cell.sectorIndex);
			// A Facade hosts objects exactly as a Location does (ADR 0003), so
			// hit-testing resolves through it too; without this, controls hosted
			// by a Facade - light switches, door buttons - can never be hovered
			// or clicked (ticket #51).
			if (isLocationLike(sector->getType()))
				return sector->getObjectAtPosition(x, y, sectorObject);
			if (sector->getType() == SectorType::Ladder)
				return dynamic_pointer_cast<const LadderTransit>(sector)->getLadder();
		}
		catch (BuildingException const&) {}
		return nullptr;
	}

	// Agent creation, placement, and waking live in SimulationCoordinator
	// (ADR 0004). Building owns the Agent registry (ADR 0001) and forwards, so
	// no caller outside Building names the coordinator.

	AgentId Building::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId, uint32_t deckOffset, float xOffset)
	{
		return mSimulationCoordinator.addOwnedAgentToSector(std::move(agent), sectorId, deckOffset, xOffset);
	}

	AgentId Building::addOwnedAgentToSector(unique_ptr<Agent> agent, uint32_t sectorId)
	{
		return mSimulationCoordinator.addOwnedAgentToSector(std::move(agent), sectorId);
	}

	AgentId Building::createAgent(string const& name, uint32_t sectorId, uint32_t deckOffset, float xOffset)
	{
		return mSimulationCoordinator.createAgent(name, sectorId, deckOffset, xOffset);
	}

	AgentId Building::createAgent(string const& name, uint32_t sectorId)
	{
		return mSimulationCoordinator.createAgent(name, sectorId);
	}

	void Building::wakeAllAgents()
	{
		mSimulationCoordinator.wakeAllAgents();
	}

	// Snapshot building - the per-entity projections and the whole-world
	// snapshot - lives in SimulationCoordinator (ADR 0004 stage 5). Building
	// keeps the whole-world forward with the rest of the tick pipeline below,
	// and this one private forward: creating and removing a traversal resource is
	// entity ownership which stays with Building (ADR 0001), and the lifecycle
	// events those paths publish carry the resource snapshot the coordinator
	// builds.
	TraversalResourceSnapshot Building::makeTraversalResourceSnapshot(TraversalResourceId id,
		TraversalResource const& resource) const
	{
		return mSimulationCoordinator.makeTraversalResourceSnapshot(id, resource);
	}

	// The queue and admission core - traversal-request creation, queue tickets,
	// queue positions and their refresh, the door queue grant and release, the
	// ladder admission family with its entry-spacing rule, traversal progress and
	// timeouts, permit expiry, and the grant / allocate / deny / commit / cancel /
	// release transaction lifecycle - lives in SimulationCoordinator (ADR 0004).
	// Building keeps the entity registries (ADR 0001) and forwards the entry
	// points which still have a caller outside Building: Agent's request
	// creation, queue-join query and transaction calls, and the deactivation and
	// topology-rebuild paths which deny, cancel and release. The queue, door
	// queue and ladder admission helpers are reached only from inside the
	// coordinator now, so no forward is left for them.

	TraversalRequestId Building::createTraversalRequest(Agent const& agent,
		shared_ptr<const Edge> const& edge, shared_ptr<const Vertex> const& source,
		shared_ptr<const Vertex> const& destination)
	{
		return mSimulationCoordinator.createTraversalRequest(agent, edge, source, destination);
	}

	bool Building::stopForAvailableQueuePosition(Agent& agent,
		shared_ptr<const Edge> const& edge, Vector2 const& endpoint,
		float movementDistance)
	{
		return mSimulationCoordinator.stopForAvailableQueuePosition(agent, edge, endpoint, movementDistance);
	}

	bool Building::isAtDoorCrossingArrival(Agent const& agent,
		shared_ptr<const Edge> const& edge, Vector2 const& threshold)
	{
		return mSimulationCoordinator.isAtDoorCrossingArrival(agent, edge, threshold);
	}

	void Building::refreshQueuePositions(TraversalResource& resource)
	{
		mSimulationCoordinator.refreshQueuePositions(resource);
	}

	void Building::updateTraversalProgressAndTimeouts()
	{
		mSimulationCoordinator.updateTraversalProgressAndTimeouts();
	}

	void Building::allocateTraversalRequest(TraversalRequestId requestId,
		shared_ptr<const Edge> const& edge, shared_ptr<const Vertex> const& destination)
	{
		mSimulationCoordinator.allocateTraversalRequest(requestId, edge, destination);
	}

	// Lift scheduling - stop lookup, destination finding, disembark demand, stop
	// requests, next-stop choice, boarding-direction compatibility, and lift
	// admission release - lives in SimulationCoordinator (ADR 0004). Building
	// keeps these entry points and forwards, so no caller outside Building
	// names the coordinator.

	uint32_t Building::findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const
	{
		return mSimulationCoordinator.findLiftStop(resource, endpoint);
	}

	uint32_t Building::findAgentLiftDestination(Agent const& agent,
		TraversalResource const& resource) const
	{
		return mSimulationCoordinator.findAgentLiftDestination(agent, resource);
	}

	bool Building::liftHasDisembarkDemand(TraversalResource const& resource, uint32_t stop) const
	{
		return mSimulationCoordinator.liftHasDisembarkDemand(resource, stop);
	}

	void Building::addLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner)
	{
		mSimulationCoordinator.addLiftStopRequest(resource, stop, owner);
	}

	void Building::removeLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner)
	{
		mSimulationCoordinator.removeLiftStopRequest(resource, stop, owner);
	}

	uint32_t Building::chooseNextLiftStop(TraversalResource& resource) const
	{
		return mSimulationCoordinator.chooseNextLiftStop(resource);
	}

	bool Building::isLiftBoardingDirectionCompatible(TraversalResource& resource,
		uint32_t originStop, uint32_t destinationStop)
	{
		return mSimulationCoordinator.isLiftBoardingDirectionCompatible(resource, originStop, destinationStop);
	}

	void Building::releaseLiftAdmission(TraversalRequestId requestId, TraversalResource& resource)
	{
		mSimulationCoordinator.releaseLiftAdmission(requestId, resource);
	}


	// Shuttle door assignment - the passenger-carriage lookup, the boarding and disembark
	// door selection, and the door-traversal retargeting they share - lives in
	// SimulationCoordinator (ADR 0004), together with the static door-offset helper
	// which gives the carriage/door indexing its meaning. Both selections are reached
	// only from inside the coordinator now, so no Building forward is left for them.

	// Passenger safe exits - the safe-exit request, the safe-exit path assignment,
	// and the onboard destination replacement - live in SimulationCoordinator
	// (ADR 0004). Building keeps these entry points and forwards, so no caller
	// outside Building names the coordinator.

	void Building::requestLiftPassengerSafeExit(AgentId passenger, TraversalFailureReason reason)
	{
		mSimulationCoordinator.requestLiftPassengerSafeExit(passenger, reason);
	}

	void Building::assignLiftSafeExitPaths(TraversalResource& resource)
	{
		mSimulationCoordinator.assignLiftSafeExitPaths(resource);
	}

	bool Building::replaceOnboardLiftDestination(Agent& agent, shared_ptr<Path> const& path,
		uint32_t& sourceNode)
	{
		return mSimulationCoordinator.replaceOnboardLiftDestination(agent, path, sourceNode);
	}

	// Open platform lift traversal allocation - calling the platform, boarding at the
	// reserved queue position, onboard destination selection, and disembark through the
	// virtual boundary - lives in SimulationCoordinator (ADR 0004), alongside the
	// admission release which undoes it. It is reached only from the coordinator's own
	// lift allocation dispatcher now, so no Building forward is left for it.

	void Building::denyTraversalRequest(TraversalRequestId requestId, TraversalFailureReason reason)
	{
		mSimulationCoordinator.denyTraversalRequest(requestId, reason);
	}

	bool Building::commitTraversal(Agent& agent, TraversalRequestId requestId, TraversalPermitId permitId,
		shared_ptr<const Vertex> const& destination)
	{
		return mSimulationCoordinator.commitTraversal(agent, requestId, permitId, destination);
	}

	void Building::cancelTraversal(TraversalRequestId requestId, TraversalPermitId permitId,
		bool requestSafeTransportExit)
	{
		mSimulationCoordinator.cancelTraversal(requestId, permitId, requestSafeTransportExit);
	}

	void Building::releaseTraversal(TraversalRequestId requestId, TraversalPermitId permitId)
	{
		mSimulationCoordinator.releaseTraversal(requestId, permitId);
	}


	// Agent lookup, id resolution, removal, and traversal-ownership release live
	// in SimulationCoordinator (ADR 0004). Building owns the registries (ADR
	// 0001) and forwards, so no caller outside Building names the coordinator.

	AgentId Building::getAgentId(Agent const* agent) const
	{
		return mSimulationCoordinator.getAgentId(agent);
	}

	EntityLookup<Agent> Building::lookupAgent(AgentId id)
	{
		return mSimulationCoordinator.lookupAgent(id);
	}

	EntityLookup<Agent const> Building::lookupAgent(AgentId id) const
	{
		return mSimulationCoordinator.lookupAgent(id);
	}

	bool Building::holdsTraversalOwnership(AgentId id) const
	{
		return mSimulationCoordinator.holdsTraversalOwnership(id);
	}

	void Building::releaseAgentFromResource(TraversalResource& resource, AgentId id)
	{
		mSimulationCoordinator.releaseAgentFromResource(resource, id);
	}

	void Building::releaseTraversalOwnership(AgentId id)
	{
		mSimulationCoordinator.releaseTraversalOwnership(id);
	}

	EntityRemovalResult Building::removeAgent(AgentId id)
	{
		return mSimulationCoordinator.removeAgent(id);
	}

	// Interaction and device-operation orchestration - the InteractionPoint and
	// InteractionRequest lifecycles, the DeviceOperation lifecycle, pressing
	// physical controls, and the per-tick interaction phases - lives in
	// SimulationCoordinator (ADR 0004). Building keeps the registries
	// (ADR 0001) and forwards every entry point, so no caller outside Building
	// names the coordinator.

	InteractionPointId Building::createInteractionPoint(string const& name)
	{
		return mSimulationCoordinator.createInteractionPoint(name);
	}

	InteractionPointId Building::createInteractionPoint(string const& name, SectorId sector,
		Vector2 position, float reach, float durationSeconds, vector<InteractionBinding> bindings)
	{
		return mSimulationCoordinator.createInteractionPoint(name, sector, position, reach, durationSeconds, std::move(bindings));
	}

	EntityLookup<InteractionPoint> Building::lookupInteractionPoint(InteractionPointId id)
	{
		return mSimulationCoordinator.lookupInteractionPoint(id);
	}

	EntityLookup<InteractionPoint const> Building::lookupInteractionPoint(InteractionPointId id) const
	{
		return mSimulationCoordinator.lookupInteractionPoint(id);
	}

	EntityRemovalResult Building::removeInteractionPoint(InteractionPointId id)
	{
		return mSimulationCoordinator.removeInteractionPoint(id);
	}

	DeviceOperationId Building::findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester)
	{
		return mSimulationCoordinator.findOrCreateDeviceOperation(command, requester);
	}

	InteractionRequestId Building::requestInteraction(InteractionPointId pointId, AgentId actorId)
	{
		return mSimulationCoordinator.requestInteraction(pointId, actorId);
	}

	InteractionRequestId Building::requestInteractionForTraversal(InteractionPointId point, AgentId actor)
	{
		return mSimulationCoordinator.requestInteractionForTraversal(point, actor);
	}

	InteractionRequestId Building::requestInteractionWhilePassing(
		InteractionPointId pointId, AgentId actorId)
	{
		return mSimulationCoordinator.requestInteractionWhilePassing(pointId, actorId);
	}

	EntityLookup<InteractionRequest const> Building::lookupInteractionRequest(InteractionRequestId id) const
	{
		return mSimulationCoordinator.lookupInteractionRequest(id);
	}

	bool Building::cancelInteraction(InteractionRequestId id)
	{
		return mSimulationCoordinator.cancelInteraction(id);
	}

	DeviceOperationId Building::createDeviceOperation(string const& name, AgentId requester)
	{
		return mSimulationCoordinator.createDeviceOperation(name, requester);
	}

	EntityLookup<DeviceOperation> Building::lookupDeviceOperation(DeviceOperationId id)
	{
		return mSimulationCoordinator.lookupDeviceOperation(id);
	}

	EntityLookup<DeviceOperation const> Building::lookupDeviceOperation(DeviceOperationId id) const
	{
		return mSimulationCoordinator.lookupDeviceOperation(id);
	}

	bool Building::cancelDeviceOperation(DeviceOperationId id, AgentId requester)
	{
		return mSimulationCoordinator.cancelDeviceOperation(id, requester);
	}

	EntityRemovalResult Building::removeDeviceOperation(DeviceOperationId id)
	{
		return mSimulationCoordinator.removeDeviceOperation(id);
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
		beginStructuralEdit("createDoorTraversalResource");
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

	TraversalResourceId Building::createWindowTraversalResource(string const& name,
		shared_ptr<Window> window)
	{
		beginStructuralEdit("createWindowTraversalResource");
		if (!window) throw invalid_argument("A window traversal resource requires a Window");
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(
			new TraversalResource(name, std::move(window))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId Building::createLadderTraversalResource(string const& name,
		shared_ptr<Ladder> ladder, SectorId ladderSector, uint32_t directionalBatchLimit)
	{
		beginStructuralEdit("createLadderTraversalResource");
		if (!ladder || !ladderSector || ladderSector.value > mSectors.size()
			|| directionalBatchLimit == 0)
		{
			throw invalid_argument("A ladder traversal resource requires a Ladder, sector, and positive batch limit");
		}
		// Capacity is a physical property of the usable vertical span: each slot keeps
		// neighbouring Agents at least CORE_LADDER_AGENT_SPACING apart in render space.
		// Even a short valid Ladder admits one Agent.
		auto crossedFloors = (float)(ladder->getDecksHigh() - 1);
		auto agentSpacing = CORE_LADDER_AGENT_SPACING / CORE_CELL_YX_RENDER_RATIO;
		auto capacity = max(1u, (uint32_t)floor(crossedFloors / agentSpacing));
		vector<Vector2> positions;
		positions.reserve(capacity);
		auto origin = ladder->getPosition();
		auto x = origin.x + ladder->getSize().x * 0.5f;
		for (uint32_t i = 0; i < capacity; ++i)
		{
			positions.push_back({ x, origin.y + (agentSpacing >= crossedFloors
				? crossedFloors * 0.5f : agentSpacing * ((float)i + 0.5f)) });
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
		shared_ptr<Lift> lift, SectorId liftSector, vector<LiftStop> stops, uint32_t capacity,
		float minimumDwellSeconds, float maximumBoardingSeconds)
	{
		beginStructuralEdit("createLiftTraversalResource");
		if (!lift || !liftSector || liftSector.value > mSectors.size() || stops.size() < 2
			|| capacity == 0 || minimumDwellSeconds < 0.0f || maximumBoardingSeconds < minimumDwellSeconds)
		{
			throw invalid_argument("A lift traversal resource requires a lift sector, at least two stops, positive capacity, and valid dwell timing");
		}
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			if (!stops[i].locationSector || stops[i].locationSector.value > mSectors.size())
				throw invalid_argument(format("Lift stop {} has an invalid location sector mapping", i));
			auto landing = mTraversalResources.find(stops[i].landingResource);
			if (!landing || !landing->mDoor)
				throw invalid_argument(format("Lift stop {} has no valid landing-door traversal mapping", i));
			if (!isfinite(stops[i].globalPosition))
				throw invalid_argument(format("Lift stop {} has a non-finite position", i));
			if (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition)
				throw invalid_argument(format("Lift stop {} is not strictly above the previous stop", i));
		}
		auto usableWidth = lift->getSize().x;
		if (capacity > (uint32_t)floor(usableWidth / CORE_AGENT_MAX_WIDTH))
			throw invalid_argument("Declared lift capacity cannot be represented by separated interior positions");
		vector<Vector2> positions(capacity);
		auto start = (usableWidth - capacity * CORE_AGENT_MAX_WIDTH) * 0.5f
			+ CORE_AGENT_MAX_WIDTH * 0.5f;
		for (uint32_t i = 0; i < capacity; ++i)
			positions[i] = { start + i * CORE_AGENT_MAX_WIDTH, 0.0f };
		auto minimumDwellTicks = (uint64_t)ceil(minimumDwellSeconds / getFixedTimestep());
		auto maximumBoardingTicks = (uint64_t)ceil(maximumBoardingSeconds / getFixedTimestep());
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, std::move(lift), liftSector, std::move(stops), capacity,
			minimumDwellTicks, maximumBoardingTicks, std::move(positions))));
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId Building::createOpenPlatformLiftTraversalResource(string const& name,
		shared_ptr<Lift> lift, SectorId locationSector, vector<LiftStop> stops, uint32_t capacity,
		float stopDurationSeconds)
	{
		beginStructuralEdit("createOpenPlatformLiftTraversalResource");
		if (!lift || !locationSector || locationSector.value > mSectors.size() || stops.size() < 2
			|| capacity == 0 || stopDurationSeconds < 0.0f)
			throw invalid_argument("An open platform lift requires a location, at least two stops, positive capacity, and a non-negative stop duration");
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			if (stops[i].locationSector != locationSector || stops[i].landingResource
				|| !isfinite(stops[i].globalPosition)
				|| (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition))
				throw invalid_argument(format("Platform lift stop {} has invalid virtual-boundary geometry", i));
		}
		if (capacity > (uint32_t)floor(lift->getSize().x / CORE_AGENT_MAX_WIDTH))
			throw invalid_argument("Declared platform lift capacity cannot be represented by separated standing positions");
		vector<Vector2> positions(capacity);
		auto start = (lift->getSize().x - capacity * CORE_AGENT_MAX_WIDTH) * 0.5f
			+ CORE_AGENT_MAX_WIDTH * 0.5f;
		for (uint32_t i = 0; i < capacity; ++i)
			positions[i] = { start + i * CORE_AGENT_MAX_WIDTH, 0.0f };
		auto stopDurationTicks = (uint64_t)ceil(stopDurationSeconds / getFixedTimestep());
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(name,
			std::move(lift), locationSector, std::move(stops), capacity,
			stopDurationTicks, stopDurationTicks, std::move(positions))));
		auto resource = mTraversalResources.find(id);
		resource->mOpenPlatformLift = true;
		resource->mVirtualBoundaryOwners.resize(1);
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *resource);
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId Building::createShuttleTraversalResource(string const& name,
		shared_ptr<Shuttle> shuttle, SectorId shuttleSector, vector<LiftStop> stops,
		uint32_t capacity, float minimumDwellSeconds, float maximumBoardingSeconds)
	{
		beginStructuralEdit("createShuttleTraversalResource");
		if (!shuttle || shuttle->getNumCars() == 0 || !shuttleSector
			|| shuttleSector.value > mSectors.size() || stops.size() < 2 || capacity == 0
			|| minimumDwellSeconds < 0.0f || maximumBoardingSeconds < minimumDwellSeconds)
			throw invalid_argument("A coupled shuttle requires valid stops, carriages, positive per-carriage capacity, and dwell timing");
		for (uint32_t i = 0; i < stops.size(); ++i)
		{
			auto landing = mTraversalResources.find(stops[i].landingResource);
			if (!stops[i].locationSector || stops[i].locationSector.value > mSectors.size()
				|| !landing || !landing->mDoor)
				throw invalid_argument(format("Shuttle stop {} has an invalid location or landing-door mapping", i));
			if (!isfinite(stops[i].globalPosition)
				|| (i > 0 && stops[i].globalPosition <= stops[i - 1].globalPosition))
				throw invalid_argument(format("Shuttle stop {} has invalid linear geometry", i));
		}
		auto usableWidth = (float)shuttle->getCarWidth();
		if (capacity > (uint32_t)floor(usableWidth / CORE_AGENT_MAX_WIDTH))
			throw invalid_argument("Declared shuttle capacity cannot be represented by separated carriage positions");
		vector<Vector2> positions;
		positions.reserve(capacity * shuttle->getNumCars());
		auto start = (usableWidth - capacity * CORE_AGENT_MAX_WIDTH) * 0.5f
			+ CORE_AGENT_MAX_WIDTH * 0.5f;
		for (uint32_t carriage = 0; carriage < shuttle->getNumCars(); ++carriage)
			for (uint32_t i = 0; i < capacity; ++i)
				positions.push_back({ carriage * (shuttle->getCarWidth() + 1.0f)
					+ start + i * CORE_AGENT_MAX_WIDTH, 0.0f });
		auto minimumDwellTicks = (uint64_t)ceil(minimumDwellSeconds / getFixedTimestep());
		auto maximumBoardingTicks = (uint64_t)ceil(maximumBoardingSeconds / getFixedTimestep());
		auto shuttlePtr = shuttle;
		auto stopCount = (uint32_t)stops.size();
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, std::move(shuttle), shuttleSector, std::move(stops), capacity,
			minimumDwellTicks, maximumBoardingTicks, std::move(positions))));
		auto resource = mTraversalResources.find(id);
		resource->mShuttleCarriages.reserve(shuttlePtr->getNumCars());
		for (uint32_t carriage = 0; carriage < shuttlePtr->getNumCars(); ++carriage)
			resource->mShuttleCarriages.push_back({ carriage, carriage * capacity, capacity,
				std::vector<std::vector<TraversalResourceId>>(stopCount) });
		SimulationEvent event;
		event.sequence = mNextEventSequence++;
		event.tick = mSimulationTick;
		event.type = SimulationEventType::TraversalResourceAdded;
		event.traversalResource = makeTraversalResourceSnapshot(id, *mTraversalResources.find(id));
		mEvents.push_back(std::move(event));
		return id;
	}

	TraversalResourceId Building::createStairwellTraversalResource(string const& name,
		shared_ptr<Stairwell> stairwell, SectorId stairwellSector, uint32_t capacity,
		uint32_t directionalBatchLimit)
	{
		beginStructuralEdit("createStairwellTraversalResource");
		if (!stairwell || !stairwellSector || stairwellSector.value > mSectors.size()
			|| capacity == 0 || directionalBatchLimit == 0)
		{
			throw invalid_argument("A narrow stairwell resource requires a Stairwell, sector, capacity, and batch limit");
		}
		vector<Vector2> positions;
		positions.reserve(capacity);
		auto origin = stairwell->getPosition();
		for (uint32_t i = 0; i < capacity; ++i)
		{
			positions.push_back({ origin.x + ((float)i + 0.5f) * stairwell->getSize().x / capacity,
				origin.y + stairwell->getSize().y * 0.5f });
		}
		auto id = mTraversalResources.add(unique_ptr<TraversalResource>(new TraversalResource(
			name, stairwell, stairwellSector, capacity, directionalBatchLimit, std::move(positions))));
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
		beginStructuralEdit("createForceBridgeTraversalResource");
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

	void Building::configureForceBridgeQueueLanes(TraversalResourceId resourceId,
		SectorId sectorId, array<Vector2, 2> const& endpoints)
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mForceBridge || !sectorId || sectorId.value > mSectors.size())
			throw invalid_argument("Force Bridge queue lanes require a Force Bridge and source sector");
		auto sector = mSectors[(size_t)sectorId.value - 1];
		auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
		auto const spacing = (float)CORE_DOOR_QUEUE_STOP_WIDTH;
		for (uint32_t approach = 0; approach < resource->mQueueLanes.size(); ++approach)
		{
			auto const direction = approach == 0 ? Vector2::NEGATIVE_UNIT_X : Vector2::UNIT_X;
			auto const endpoint = endpoints[approach];
			auto const boundary = approach == 0
				? sector->getCellX0() + halfWidth : sector->getCellX1() + 1.0f - halfWidth;
			auto const extent = max(0.0f, approach == 0
				? endpoint.x - boundary : boundary - endpoint.x);
			auto& lane = resource->mQueueLanes[approach];
			lane.sector = sectorId;
			lane.origin = endpoint;
			lane.direction = direction;
			lane.extent = 0.0f;
			for (float distance = 0.0f; distance <= extent + 0.001f; distance += spacing)
			{
				auto position = endpoint + direction * distance;
				auto const cellX = min(sector->getCellX1(), (uint32_t)floor(position.x));
				auto const cellY = min(sector->getCellY1(), (uint32_t)floor(position.y));
				if (!mLayers[sector->getLayerIndex()]
					->getCellDefinition(cellX, cellY).isTraversableOnFoot()) break;
				lane.positions.push_back(position);
				lane.extent = distance;
			}
			lane.positionOwners.assign(lane.positions.size(), {});
			if (lane.positions.empty())
				throw invalid_argument("A Force Bridge approach has no usable queue positions");
		}
		resource->mCrossingOwners.assign(1, {});
	}

	void Building::configureLadderQueueLanes(TraversalResourceId resourceId,
		array<SectorId, 2> const& sectors, array<Vector2, 2> const& endpoints)
	{
		auto resource = mTraversalResources.find(resourceId);
		if (!resource || !resource->mLadder)
			throw invalid_argument("Ladder queue lanes require a Ladder traversal resource");

		auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
		auto const spacing = (float)CORE_DOOR_QUEUE_STOP_WIDTH;
		for (uint32_t approach = 0; approach < resource->mQueueLanes.size(); ++approach)
		{
			if (!sectors[approach] || sectors[approach].value > mSectors.size())
				throw invalid_argument("Ladder queue lane has an invalid approach sector");
			auto sector = mSectors[(size_t)sectors[approach].value - 1];
			auto const endpoint = endpoints[approach];
			auto positionFits = [&](float positionX)
			{
				if (positionX - halfWidth < sector->getCellX0() - 0.001f
					|| positionX + halfWidth > sector->getCellX1() + 1.0f + 0.001f
					|| endpoint.y < sector->getCellY0() - 0.001f
					|| endpoint.y + CORE_AGENT_MAX_HEIGHT > sector->getCellY1() + 1.0f + 0.001f)
					return false;
				auto const cellX = min(sector->getCellX1(), (uint32_t)floor(positionX));
				auto const cellY = min(sector->getCellY1(), (uint32_t)floor(endpoint.y));
				return mLayers[sector->getLayerIndex()]
					->getCellDefinition(cellX, cellY).isTraversableOnFoot();
			};

			// Unlike a Door, the Ladder endpoint itself must remain clear for
			// mounting and dismounting. Queue positions therefore begin at step 1;
			// admission later requires the selected Agent to have reached one of them.
			vector<Vector2> positions;
			bool scanLeft = true, scanRight = true;
			for (uint32_t step = 1; scanLeft || scanRight; ++step)
			{
				auto const distance = step * spacing;
				auto const left = endpoint.x - distance;
				auto const right = endpoint.x + distance;
				if (scanLeft)
				{
					scanLeft = positionFits(left);
					if (scanLeft) positions.push_back({ left, endpoint.y });
				}
				if (scanRight)
				{
					scanRight = positionFits(right);
					if (scanRight) positions.push_back({ right, endpoint.y });
				}
			}

			auto& lane = resource->mQueueLanes[approach];
			lane.sector = sectors[approach];
			lane.origin = endpoint;
			lane.direction = Vector2::UNIT_X;
			lane.extent = positions.empty() ? 0.0f : spacing;
			for (auto const& position : positions)
				lane.extent = max(lane.extent, abs(position.x - endpoint.x));
			lane.positions = std::move(positions);
			lane.positionOwners.assign(lane.positions.size(), {});
		}
	}

	bool Building::configureDoorQueueLane(TraversalResourceId resourceId, SectorId sectorId,
		Vector2 origin, Vector2 direction, float extent)
	{
		beginStructuralEdit("configureDoorQueueLane");
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

		QueueLane* lane = nullptr;
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
		beginStructuralEdit("configureDoorCrossingLanes");
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

	// Door open lease acquisition and release live in SimulationCoordinator
	// (ADR 0004). Building forwards both the resource-reference form the
	// traversal machinery uses and the handle form external holders use.

	DoorOpenLeaseId Building::acquireDoorOpenLease(TraversalResource& resource,
		DoorOpenLeaseKind kind, TraversalRequestId request)
	{
		return mSimulationCoordinator.acquireDoorOpenLease(resource, kind, request);
	}

	bool Building::releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease)
	{
		return mSimulationCoordinator.releaseDoorOpenLease(resource, lease);
	}

	DoorOpenLeaseId Building::acquireDoorOpenLease(TraversalResourceId resource, DoorOpenLeaseKind kind)
	{
		return mSimulationCoordinator.acquireDoorOpenLease(resource, kind);
	}

	bool Building::releaseDoorOpenLease(TraversalResourceId resource, DoorOpenLeaseId lease)
	{
		return mSimulationCoordinator.releaseDoorOpenLease(resource, lease);
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
		if (resource->mEnabled == enabled
			&& ((!resource->mLift && !resource->mShuttle) || !resource->mLiftDraining)) return true;
		resource->mEnabled = enabled;
		if (!resource->mLift && !resource->mShuttle)
			return true;

		if (enabled)
		{
			resource->mLiftDraining = false;
			return true;
		}

		resource->mLiftDraining = true;
		vector<AgentId> occupants;
		for (auto occupant : resource->mOccupants) if (occupant) occupants.push_back(occupant);
		for (auto occupant : occupants)
			requestLiftPassengerSafeExit(occupant, TraversalFailureReason::ResourceDisabled);

		vector<TraversalRequestId> rejected;
		for (auto const& [requestId, request] : mTraversalRequests.entries())
		{
			auto authority = mTraversalResources.find(request->mResource);
			if (request->mState == TraversalRequestState::Pending
				&& (request->mResource == resourceId
					|| (authority && authority->mLiftCoordinator == resourceId))
				&& find(occupants.begin(), occupants.end(), request->mOwner) == occupants.end())
				rejected.push_back(requestId);
		}
		for (auto requestId : rejected)
			denyTraversalRequest(requestId, TraversalFailureReason::ResourceDisabled);
		return true;
	}

	bool Building::addTraversalControl(TraversalResourceId resourceId, InteractionPointId controlId)
	{
		beginStructuralEdit("addTraversalControl");
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

	TraversalResourceId Building::getTraversalResourceId(Object const* object) const
	{
		if (!object) return {};
		for (auto const& [id, resource] : mTraversalResources.entries())
		{
			bool matches = resource->mDoor.get() == object || resource->mWindow.get() == object
				|| resource->mLadder.get() == object || resource->mForceBridge.get() == object
				|| resource->mLift.get() == object || resource->mShuttle.get() == object
				|| resource->mStairwell.get() == object;
			if (!matches) continue;
			return resource->mLiftCoordinator ? resource->mLiftCoordinator : id;
		}
		return {};
	}

	EntityRemovalResult Building::removeTraversalResource(TraversalResourceId id)
	{
		auto found = lookupTraversalResource(id);
		if (!found)
		{
			return { false, found.diagnostic };
		}
		bool structural = found.entity->mDoor || found.entity->mWindow || found.entity->mLadder
			|| found.entity->mForceBridge || found.entity->mLift || found.entity->mShuttle
			|| found.entity->mStairwell;
		if (!structural && mGraph)
			structural = any_of(mGraph->getEdges().begin(), mGraph->getEdges().end(),
				[id](auto const& edge) { return edge->getTraversalResourceId() == id; });
		if (structural) beginStructuralEdit("removeTraversalResource");
		auto hasOwner = [](auto const& values)
			{ return any_of(values.begin(), values.end(), [](auto value) { return (bool)value; }); };
		bool owned = hasOwner(found.entity->mOccupants)
			|| hasOwner(found.entity->mAdmissionReservations)
			|| hasOwner(found.entity->mCrossingOwners)
			|| hasOwner(found.entity->mVirtualBoundaryOwners)
			|| !found.entity->mAdmissionQueue.empty() || !found.entity->mOpenLeases.empty()
			|| !found.entity->mExtensionRequestLeases.empty()
			|| !found.entity->mExtensionOccupantLeases.empty()
			|| any_of(found.entity->mQueueLanes.begin(), found.entity->mQueueLanes.end(),
				[](auto const& lane) { return !lane.queue.empty(); })
			|| any_of(found.entity->mLiftStopRequestOwners.begin(), found.entity->mLiftStopRequestOwners.end(),
				[](auto const& owners) { return !owners.empty(); });
		if (owned)
			return { false, format("TraversalResource handle {} still has active ownership and cannot be replaced safely", id.value) };
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
		auto resource = mTraversalResources.find(resourceId);
		if (!resource) return 0.0f;

		if (resource->mLiftCoordinator)
		{
			auto lift = mTraversalResources.find(resource->mLiftCoordinator);
			if (!lift) return 0.0f;
			auto stop = resource->mLiftStopIndex;
			float delay = mTraversalWaitingPolicy.queueDelayPerAgentSeconds
				* (float)count_if(lift->mAdmissionQueue.begin(), lift->mAdmissionQueue.end(),
					[&](TraversalRequestId id)
					{
						auto request = mTraversalRequests.find(id);
						return request && request->mSourceSector == sourceSector;
					}) / max(1u, lift->mCapacity);
			if (stop < lift->mLiftStops.size())
			{
				delay += abs(lift->mLiftPosition - lift->mLiftStops[stop].globalPosition)
					/ (lift->mShuttle ? CORE_SHUTTLE_SPEED : CORE_LIFT_SPEED);
				// Existing scheduled stops add stable preparation/service cost without
				// creating a reservation as a side effect of path search.
				for (uint32_t i = 0; i < lift->mLiftStops.size(); ++i)
					if (i != stop && !lift->mLiftStopRequestOwners[i].empty())
						delay += (float)lift->mLiftMinimumDwellTicks * getFixedTimestep();
			}
			return delay;
		}
		if (resource->mLift || resource->mShuttle)
		{
			float delay = (float)resource->mLiftMinimumDwellTicks * getFixedTimestep();
			for (auto const& owners : resource->mLiftStopRequestOwners)
				if (!owners.empty()) delay += mTraversalWaitingPolicy.queueDelayPerAgentSeconds;
			return delay;
		}

		size_t ahead = 0;
		for (auto const& lane : resource->mQueueLanes) ahead += lane.queue.size();
		auto lanes = max<size_t>(1, resource->mCrossingOwners.size());
		return (float)((ahead + lanes - 1) / lanes)
			* mTraversalWaitingPolicy.queueDelayPerAgentSeconds;
	}

	// The tick pipeline - lift and door resource advancement, the simulation
	// phases, tick event publication, the simulation clock and event consumption -
	// lives in SimulationCoordinator (ADR 0004 stage 5), as does every snapshot
	// builder removed above. Building keeps the forwards which still have callers
	// outside itself.

	// Per-tick interaction phases - device-operation advancement, allocation,
	// movement and result resolution - run in SimulationCoordinator (ADR 0004).
	void Building::advanceDeviceOperations()
	{
		mSimulationCoordinator.advanceDeviceOperations();
	}

	void Building::pressPhysicalControl(InteractionPointId pointId)
	{
		mSimulationCoordinator.pressPhysicalControl(pointId);
	}

	void Building::tryPressUpcomingDoorButton(Agent& agent,
		Vector2 const& movementStart, Vector2 const& movementEnd)
	{
		mSimulationCoordinator.tryPressUpcomingDoorButton(agent, movementStart, movementEnd);
	}

	void Building::allocateInteractions()
	{
		mSimulationCoordinator.allocateInteractions();
	}

	void Building::moveInteractions(float frameTime)
	{
		mSimulationCoordinator.moveInteractions(frameTime);
	}

	void Building::updateInteractionResults()
	{
		mSimulationCoordinator.updateInteractionResults();
	}

	void Building::update(float elapsedSeconds)
	{
		mSimulationCoordinator.update(elapsedSeconds);
	}

	void Building::advanceTick()
	{
		mSimulationCoordinator.advanceTick();
	}

	void Building::advanceTicks(uint64_t count)
	{
		mSimulationCoordinator.advanceTicks(count);
	}

	uint64_t Building::getSimulationTick() const
	{
		return mSimulationCoordinator.getSimulationTick();
	}

	SimulationPhase Building::getCurrentSimulationPhase() const
	{
		return mSimulationCoordinator.getCurrentSimulationPhase();
	}

	SimulationSnapshot Building::getSimulationSnapshot() const
	{
		return mSimulationCoordinator.getSimulationSnapshot();
	}

	vector<SimulationEvent> Building::consumeSimulationEvents()
	{
		return mSimulationCoordinator.consumeSimulationEvents();
	}

} // core