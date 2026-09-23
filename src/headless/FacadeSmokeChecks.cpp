// Facade checks, for tickets #43, #44, #45, #48, #51 and #52.
//
// A Facade is an occupiable Location whose perimeter walls are all open by
// construction: it hosts objects and agents exactly as a Room does, owns
// walkable floor, and takes part in the Graph, but every wall end on every
// deck is open intrinsically and it is rendered as a solid opaque colour
// (ADR 0003). These checks cover creation and placement validation, the
// open-end invariant, agent placement, object-placement parity with a Room,
// Bulkhead Door refusal, wall-command refusal, persistence of the
// ConstructionType::Facade record - its name, footprint and packed colour,
// the sector index mapping across a canonical replay, and the rejection a
// Facade map gets from a pre-Facade reader - and - from #45 -
// horizontal-adjacency merging: a Facade between two floor-aligned Rooms
// is one continuous floor, a mismatched-floor neighbour does not merge,
// wall removal accepts a Facade neighbour on either side of the boundary,
// and a Background stays out of the pathing world entirely; from #48, a
// Door accepts a Facade as its front Sector and as its back Sector; and
// from #51, hit-testing resolves through a Facade so hosted controls
// can be hovered and clicked; and from #52, Transit landings: every Transit
// type - Ladder, Stairwell, Lift, Shuttle and Staircase - lands on a Facade
// exactly as it lands on a Room, its landing Vertex is a VertexType::Location,
// and the whole menagerie round-trips through a canonical save/load.

#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/Background.h"
#include "core/World.h"
#include "core/Button.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Facade.h"
#include "core/Graph.h"
#include "core/Location.h"
#include "core/Path.h"
#include "core/Sector.h"
#include "core/SectorEdge.h"
#include "core/SectorObjectType.h"
#include "core/SectorType.h"
#include "core/VertexType.h"
#include "core/SerializationException.h"
#include "core/Transit.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool throws(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (std::exception const&)
		{
			return true;
		}
		return false;
	}

	std::string nameOf(core::SectorObjectType type)
	{
		switch (type)
		{
		case core::SectorObjectType::None: return "None";
		case core::SectorObjectType::BulkheadDoor: return "BulkheadDoor";
		case core::SectorObjectType::Door: return "Door";
		case core::SectorObjectType::ForceBridge: return "ForceBridge";
		case core::SectorObjectType::InteractionPoint: return "InteractionPoint";
		case core::SectorObjectType::Ladder: return "Ladder";
		case core::SectorObjectType::Lift: return "Lift";
		case core::SectorObjectType::Marker: return "Marker";
		case core::SectorObjectType::Shuttle: return "Shuttle";
		case core::SectorObjectType::Walkway: return "Walkway";
		case core::SectorObjectType::Window: return "Window";
		}
		return "Unknown";
	}

	// Every SectorObjectType the model knows about. The parity loop below walks
	// this list so a type added later is compared against a Room rather than
	// silently skipped.
	std::array<core::SectorObjectType, 11> const AllSectorObjectTypes{
		core::SectorObjectType::None,
		core::SectorObjectType::BulkheadDoor,
		core::SectorObjectType::Door,
		core::SectorObjectType::ForceBridge,
		core::SectorObjectType::InteractionPoint,
		core::SectorObjectType::Ladder,
		core::SectorObjectType::Lift,
		core::SectorObjectType::Marker,
		core::SectorObjectType::Shuttle,
		core::SectorObjectType::Walkway,
		core::SectorObjectType::Window
	};

	core::Facade makeFacade(core::BackgroundColour const& colour = core::Facade::defaultColour())
	{
		return core::Facade("Frontage", 1, 3, 2, 1, 4, 2, CORE_ROOM_MAX_HEIGHT, colour);
	}

	std::shared_ptr<const core::Facade> facadeIn(core::World const& world, uint32_t sectorIndex)
	{
		auto sector = world.getSector(sectorIndex);
		require(sector != nullptr, "World reported a null Sector");
		require(sector->getType() == core::SectorType::Facade,
			("Sector " + std::to_string(sectorIndex) + " is not a Facade").c_str());
		auto facade = std::dynamic_pointer_cast<const core::Facade>(sector);
		require(facade != nullptr, "A Facade Sector is not a core::Facade");
		return facade;
	}

	std::string serializeWorld(core::World const& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	void loadInto(core::World& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(target.deserialize(*reader, workData), "World YAML did not load");
	}

	// A stable description of every Sector, Facade colour included, used to
	// compare a World against its own replay.
	std::string sectorSignature(core::World const& world)
	{
		std::string signature;
		for (uint32_t index = 0; index < world.getNumSectors(); ++index)
		{
			auto const sector = world.getSector(index);
			require(sector != nullptr, "World reported a null Sector while signing");
			signature += std::format("{}:{}@{},{},{}x{} name={}", index,
				core::getSectorTypeString(sector->getType()), sector->getLayerIndex(),
				sector->getCellX(), sector->getCellY(), sector->getCellsWide(), sector->getDecksHigh(),
				sector->getName());
			if (sector->getType() == core::SectorType::Facade)
			{
				auto const facade = std::dynamic_pointer_cast<const core::Facade>(sector);
				require(facade != nullptr, "Signed Facade is not a core::Facade");
				signature += std::format(" colour={:06X} topDeck={}",
					core::packBackgroundColour(facade->getColour()), facade->getTopDeckHeight());
			}
			signature += ";\n";
		}
		return signature;
	}

	// The open-perimeter invariant: every end on every deck and both sides is
	// open, and nothing has crept in as a Wall.
	void everyEndIsOpen(core::Sector const& sector)
	{
		for (uint32_t deck = 0; deck < sector.getDecksHigh(); ++deck)
		{
			for (int side : { CORE_SIDE_LEFT, CORE_SIDE_RIGHT })
			{
				require(sector.getEndType(deck, side) == core::SectorEndType::None,
					std::format("A Facade end is not open: deck {}, side {}", deck, side).c_str());
			}
		}
	}

	// How many times a needle appears in a haystack. Used to count record kinds
	// in a saved file without going through the parser.
	int countOccurrences(std::string const& haystack, std::string const& needle)
	{
		int count = 0;
		for (auto pos = haystack.find(needle); pos != std::string::npos;
			pos = haystack.find(needle, pos + needle.size()))
			++count;
		return count;
	}

	// Per-Sector object inventory. Sector-referencing records - Markers, Light
	// Switches, Transit landings - are authored against a Sector index, so a
	// replay that remapped indices wrongly would show up here as an object
	// changing Sector rather than as a load failure.
	std::string objectSignature(core::World const& world)
	{
		std::string signature;
		for (uint32_t index = 0; index < world.getNumSectors(); ++index)
		{
			auto const sector = world.getSector(index);
			require(sector != nullptr, "World reported a null Sector while signing objects");
			signature += std::format("{}[{}]:", index, core::getSectorTypeString(sector->getType()));
			for (uint32_t object = 0; object < sector->getNumObjects(); ++object)
			{
				auto const item = sector->getObject(object);
				if (!item)
				{
					signature += "Tombstone; ";
					continue;
				}
				signature += std::format("{}@{},{}; ", nameOf(item->getObjectType()),
						item->getCellX(), item->getCellY());
			}
			signature += "\n";
		}
		return signature;
	}

	// Whether a Sector currently carries an object of the given type. Used to
	// follow a sector-referencing record through a replay or a remapping edit.
	bool sectorHasObject(core::World const& world, uint32_t sectorIndex,
		core::SectorObjectType type)
	{
		auto const sector = world.getSector(sectorIndex);
		if (!sector) return false;
		for (uint32_t object = 0; object < sector->getNumObjects(); ++object)
		{
			auto const item = sector->getObject(object);
			if (item && item->getObjectType() == type) return true;
		}
		return false;
	}

	// The construction record names a build from before Facades existed: the
	// version 5 writer's whole vocabulary. "facade" is not among them.
	std::set<std::string> const PreFacadeRecordNames{
		"corridor", "room", "ladder", "stairwell", "staircase", "lift", "shuttle", "door",
		"window", "bulkheadDoor", "lightSwitch", "forceBridge", "sectorLadder", "platformLift",
		"walkway", "marker", "removeWall", "removeMarker", "objectTombstone", "background" };

	uint32_t const PreFacadeVersionCeiling{ 5 };

	// A stand-in for a pre-Facade build's reader: the version ceiling that
	// build accepted, and its record-name table, failing on an unknown name
	// the same loud way the old parser did. A Facade map has to be refused
	// here rather than have its Facades quietly dropped.
	void preFacadeReaderReads(std::string const& yaml)
	{
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		reader->beginMap("world");
		auto const version = reader->readUint32("version");
		if (version > PreFacadeVersionCeiling)
			throw core::SerializationException("Unsupported World serialization version");
		reader->beginArray("construction");
		while (reader->nextArrayItem())
		{
			reader->beginMap("");
			auto const type = reader->readString("type");
			if (PreFacadeRecordNames.find(type) == PreFacadeRecordNames.end())
				throw core::SerializationException(
					std::format("Unknown World construction record type: {}", type));
			reader->endMap();
		}
		reader->endArray();
		reader->endMap();
	}

	// The type is named, and a Facade is a Location - not a Background, not a
	// Transit - whose perimeter is open from the moment it exists.
	void theTypeIsAKnownLocationKind()
	{
		require(core::getSectorTypeString(core::SectorType::Facade) == "Facade",
			"getSectorTypeString() does not name SectorType::Facade");

		auto const facade = makeFacade();
		core::Sector const& asSector = facade;

		require(facade.getType() == core::SectorType::Facade,
			"A Facade does not report SectorType::Facade");
		require(dynamic_cast<core::Location const*>(&asSector) != nullptr,
			"A Facade is not reachable as a Location");
		require(dynamic_cast<core::Background const*>(&asSector) == nullptr,
			"A Facade is reachable as a Background");
		require(dynamic_cast<core::Transit const*>(&asSector) == nullptr,
			"A Facade is reachable as a Transit");
		// Ticket #55: the description leads with the Facade's own name, the
		// way Location::getDescription() does, not a hardcoded type word.
		require(facade.getDescription().rfind("Frontage", 0) == 0,
			("A Facade's description does not lead with its name: "
				+ facade.getDescription()).c_str());
		require(!facade.isCorridor(), "A Facade reports itself as a corridor");
		everyEndIsOpen(facade);
	}

	// The Facade carries its own opaque colour, default (176, 160, 128),
	// distinct from the Background default and the Location colours, and the
	// setter round-trips through Background's packing helpers.
	void theColourDefaultsAndRoundTrips()
	{
		auto facade = makeFacade();

		require(facade.getColour().r == 176 && facade.getColour().g == 160
			&& facade.getColour().b == 128,
			"The default Facade colour is not (176, 160, 128)");
		require(core::Facade::defaultColour() == core::BackgroundColour{ 176, 160, 128 },
			"Facade::defaultColour() is not (176, 160, 128)");
		require(core::Facade::defaultColour() != core::BackgroundColour{},
			"The Facade default colour collides with the Background default");

		facade.setColour({ 7, 200, 44 });
		require(facade.getColour() == core::BackgroundColour{ 7, 200, 44 },
			"Facade::setColour() did not round-trip");
		require(core::unpackBackgroundColour(core::packBackgroundColour(facade.getColour()))
			== facade.getColour(),
			"The Facade colour does not survive Background's pack/unpack pair");
	}

	// canAddFacade accepts the same placements a Room plays by and refuses the
	// same ones: minimum (1,1), inside the bounds, free cells on its own Layer,
	// and a legal top deck height.
	void placementValidationFollowsTheRoomRule()
	{
		core::World world("Placement", 12, 3);
		world.addRoom("Occupier", 1, 0, 0, 3, 1);
		world.finishBuild();
		world.pauseSimulation();

		std::string diagnostic;
		require(world.canAddFacade(0, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was refused on a free front Layer cell block");
		require(world.canAddFacade(1, 0, 3, 2, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was refused beside an existing Room on a free block");
		require(!world.canAddFacade(1, 0, 0, 1, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was accepted over a cell an existing Sector owns");
		require(!world.canAddFacade(0, 0, 0, 0, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A zero-wide Facade was accepted");
		require(!world.canAddFacade(0, 0, 0, 1, 0, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A zero-high Facade was accepted");
		require(!world.canAddFacade(0, 0, 0, 1, 1, CORE_ROOM_MAX_HEIGHT + 0.1f, &diagnostic),
			"A Facade was accepted with a top deck height above CORE_ROOM_MAX_HEIGHT");
		require(!world.canAddFacade(0, 0, 0, 1, 1, CORE_ROOM_MIN_HEIGHT - 0.1f, &diagnostic),
			"A Facade was accepted with a top deck height below CORE_ROOM_MIN_HEIGHT");
		require(!world.canAddFacade(0, 0, 11, 2, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was accepted across the World bounds");
		require(!world.canAddFacade(9, 0, 0, 1, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was accepted on a nonexistent Layer");

		world.addFacade(0, 1, 4, 4, 2, CORE_ROOM_MAX_HEIGHT, { 200, 30, 99 });
		world.finishBuild();
		auto const facade = facadeIn(world, 1);
		require(facade->getLayerIndex() == 0 && facade->getCellY() == 1 && facade->getCellX() == 4
			&& facade->getCellsWide() == 4 && facade->getDecksHigh() == 2,
			"The Facade footprint is not where it was placed");
		require(facade->getColour() == core::BackgroundColour{ 200, 30, 99 },
			"The Facade colour was not carried to the live Sector");

		// A Facade owns walkable floor exactly as a Location does: ground on
		// the bottom deck.
		auto const layer = std::as_const(world).getLayer(0);
		require(layer->getCellDefinition(4, 1).floorType == core::CellFloorType::Ground
			&& layer->getCellDefinition(4, 1).isTraversableOnFoot(),
			"A Facade's ground deck is not walkable");
	}

	// The open-end invariant on the live World: every deck, both sides.
	void everyWallEndIsOpenOnEveryDeck()
	{
		core::World world("Open perimeter", 12, 3);
		auto const index = world.addFacade(0, 0, 0, 5, 3);
		world.finishBuild();

		auto const facade = facadeIn(world, index);
		require(facade->getDecksHigh() == 3, "The Facade did not keep its deck count");
		everyEndIsOpen(*facade);
	}

	// An Agent belongs to a Facade exactly as it belongs to a Room.
	void agentsMayBePlacedInAFacade()
	{
		core::World world("Host", 12, 3);
		auto const facadeIndex = world.addFacade(0, 0, 0, 4, 1);
		world.finishBuild();

		auto const agentId = world.createAgent("Frontage dweller", facadeIndex, 0, 1.5f);
		auto const agent = world.lookupAgent(agentId).entity;
		require(agent != nullptr, "The Facade Agent was not created");
		require(agent->getSector() != nullptr
			&& agent->getSector()->getIndex() == facadeIndex,
			"The Agent does not belong to the Facade");
		require(facadeIn(world, facadeIndex)->getAgents().size() == 1,
			"The Facade does not report its Agent");
	}

	// Object-placement parity: a Facade hosts every object type a Room hosts,
	// with the single exception of the Bulkhead Door, which needs wall ends a
	// Facade does not have.
	void objectHostingParityWithARoom()
	{
		core::World world("Parity", 12, 3);
		auto const roomIndex = world.addRoom("Comparator", 0, 0, 0, 4, 2);
		auto const facadeIndex = world.addFacade(0, 0, 4, 4, 2);
		world.finishBuild();

		auto const room = world.getSector(roomIndex);
		auto const facade = world.getSector(facadeIndex);
		require(room->getType() == core::SectorType::Location,
			"The comparison Sector is not a plain Location");

		for (auto const type : AllSectorObjectTypes)
		{
			auto const expected = room->sectorSupportsObjectType(type)
				&& type != core::SectorObjectType::BulkheadDoor;
			require(facade->sectorSupportsObjectType(type) == expected,
				("The Facade's hosting answer for " + nameOf(type) + " is "
					+ std::string(facade->sectorSupportsObjectType(type) ? "true" : "false")
					+ " but a Room's is "
					+ std::string(room->sectorSupportsObjectType(type) ? "true" : "false")).c_str());
		}

		// Sanity: the exception is real on both sides.
		require(room->sectorSupportsObjectType(core::SectorObjectType::BulkheadDoor),
			"A Room no longer hosts Bulkhead Doors; the parity baseline moved");
		require(!facade->sectorSupportsObjectType(core::SectorObjectType::BulkheadDoor),
			"A Facade claims to host Bulkhead Doors");
	}

	// Markers and other Room-supported objects place inside a Facade for real,
	// not just by predicate.
	void roomSupportedObjectsPlaceInAFacade()
	{
		core::World world("Host of objects", 12, 3);
		auto const facadeIndex = world.addFacade(0, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT,
			{ 11, 222, 33 });
		world.finishBuild();
		world.pauseSimulation();

		std::string diagnostic;
		require(world.canAddSectorMarker(facadeIndex, 0, 1.5f, &diagnostic),
			("A Marker was refused in a Facade: " + diagnostic).c_str());
		auto const marker = world.addSectorMarker(facadeIndex, 0, 1.5f);
		require(marker.sector != nullptr, "The Facade Marker has no Sector");

		require(world.canAddSectorWalkway(facadeIndex, 1, 0, &diagnostic),
			("A Walkway was refused in a Facade: " + diagnostic).c_str());
		world.addSectorWalkway(facadeIndex, 1, 0);

		// A light switch is an InteractionPoint control; a Room hosts it, so a
		// Facade must take one too. There is no canAdd form; the add must not throw.
		world.addSectorLightSwitch(facadeIndex, 1);

		require(world.canAddRoomLadder(facadeIndex, 0, 0, nullptr, &diagnostic),
			("A Room Ladder was refused under the Facade Walkway: " + diagnostic).c_str());
		world.addRoomLadder(facadeIndex, 0, 0);

		// The Facade still hosts nothing that a Room does not, and its perimeter
		// is still open after all that object authoring.
		everyEndIsOpen(*facadeIn(world, facadeIndex));
	}

	// Ticket #51: hit-testing must resolve through a Facade exactly as it does
	// through a Room, so hosted controls - light switches, door buttons - and
	// other sector objects inside a Facade can be hovered and clicked by the
	// UI. Before the fix, World::getObjectAtPosition only delegated to the
	// Sector for a plain Location, so a Facade always hit-tested empty.
	void hostedObjectsAreHitTestableThroughAFacade()
	{
		core::World world("Hit-testing through a Facade", 12, 3);
		auto const facadeIndex = world.addFacade(0, 0, 0, 4, 2);
		auto const roomIndex = world.addRoom("Comparator", 0, 0, 4, 4, 2);
		world.finishBuild();
		world.pauseSimulation();

		auto const facadeSwitch = world.addSectorLightSwitch(facadeIndex, 1);
		auto const roomSwitch = world.addSectorLightSwitch(roomIndex, 1);

		// Hit-test the centre of a light switch's Button and require that the
		// hit resolves to that Button and its hosting SectorObject.
		auto const hitSwitch = [](core::World const& world,
			core::World::CreateObjectResult const& created, char const* what)
		{
			auto const object = created.sector->getObject(created.index);
			require(object != nullptr
				&& object->getObjectType() == core::SectorObjectType::InteractionPoint,
				(std::string(what) + ": the light switch is not an InteractionPoint").c_str());
			auto const button = std::dynamic_pointer_cast<const core::Button>(object->_getObject());
			require(button != nullptr,
				(std::string(what) + ": the InteractionPoint wraps no Button").c_str());
			auto const center = button->getPosition() + button->getSize() * 0.5f;
			std::shared_ptr<const core::SectorObject> hitObject;
			auto const hit = world.getObjectAtPosition(0, center.x, center.y, &hitObject);
			require(hit.get() == button.get(),
				(std::string(what) + ": the hover ray missed the light switch").c_str());
			require(hitObject == object,
				(std::string(what) + ": the hit resolved the wrong SectorObject").c_str());
		};

		hitSwitch(world, roomSwitch, "A light switch in a Room");
		hitSwitch(world, facadeSwitch, "A light switch in a Facade");

		// Non-control hover: a Marker inside the Facade is hit-testable too.
		auto const marker = world.addSectorMarker(facadeIndex, 0, 1.5f);
		{
			auto const object = world.getSector(facadeIndex)->getObject(marker.index);
			require(object != nullptr
				&& object->getObjectType() == core::SectorObjectType::Marker,
				"The Facade Marker went missing before its hit-test");
			auto const shape = object->_getObject();
			core::Vector2 minExtent, maxExtent;
			shape->getFullShape(minExtent, maxExtent);
			auto const center = minExtent + (maxExtent - minExtent) * 0.5f;
			std::shared_ptr<const core::SectorObject> hitObject;
			auto const hit = world.getObjectAtPosition(0, center.x, center.y, &hitObject);
			require(hit.get() == shape.get() && hitObject == object,
				"The hover ray missed the Marker inside the Facade");
		}

		// And the Facade still says "nothing here" for an empty cell: the
		// widening admits hit-tests, it does not make everything hittable.
		{
			std::shared_ptr<const core::SectorObject> hitObject;
			auto const hit = world.getObjectAtPosition(0, 3.5f, 0.5f, &hitObject);
			require(hit == nullptr && hitObject == nullptr,
				"An empty Facade cell hit-tested as if it held something");
		}
	}

	// A Bulkhead Door is set into a pair of wall ends. A Facade has none, so
	// every Bulkhead Door placement that touches one is refused - while the
	// same placement between two Rooms still works, so the refusal is
	// Facade-specific and not a broken check.
	void bulkheadDoorsAreRefusedOnAFacade()
	{
		core::World world("No bulkheads here", 16, 3);
		auto const roomA = world.addRoom("Room A", 0, 0, 0, 4, 1);
		world.addFacade(0, 0, 4, 4, 1);
		auto const roomB = world.addRoom("Room B", 0, 0, 8, 4, 1);
		world.finishBuild();
		(void)roomA; (void)roomB;

		std::string diagnostic;
		// Facade on the right of the boundary, Room A on the left.
		require(!world.canAddSectorBulkheadDoor(0, 0, 4, CORE_SIDE_LEFT,
				core::World::CreateBulkheadDoorOptions{}, &diagnostic),
			"A Bulkhead Door was accepted with a Facade on its right");
		require(diagnostic.find("Facade") != std::string::npos,
			("The Bulkhead Door refusal does not name the Facade: " + diagnostic).c_str());
		require(throws([&] { world.addSectorBulkheadDoor(0, 0, 4, CORE_SIDE_LEFT); }),
			"addSectorBulkheadDoor did not throw against a Facade boundary");

		// Facade on the left of the boundary, Room B on the right.
		require(!world.canAddSectorBulkheadDoor(0, 0, 7, CORE_SIDE_RIGHT,
				core::World::CreateBulkheadDoorOptions{}, &diagnostic),
			"A Bulkhead Door was accepted with a Facade on its left");
		require(throws([&] { world.addSectorBulkheadDoor(0, 0, 7, CORE_SIDE_RIGHT); }),
			"addSectorBulkheadDoor did not throw against the Facade's other side");

		// The same check still admits a Room-to-Room boundary away from the
		// Facade: build a second pair and compare.
		core::World control("Bulkhead control", 16, 3);
		control.addRoom("Left", 0, 0, 0, 4, 1);
		control.addRoom("Right", 0, 0, 4, 4, 1);
		control.finishBuild();
		require(control.canAddSectorBulkheadDoor(0, 0, 4, CORE_SIDE_LEFT,
				core::World::CreateBulkheadDoorOptions{}, &diagnostic),
			("The Bulkhead Door check broke for plain Rooms too: " + diagnostic).c_str());
	}

	// A Door may have a Facade as its front Sector or as its back Sector:
	// a Facade follows the Room hosting rule, so createDoor's front-Sector
	// assert must admit it (ADR 0003, ticket #48). The corridor-door UI
	// tool cannot reach this - it requires isCorridor() - so this is the
	// API and lift-landing path.
	void doorAcceptsAFacadeOnEitherSide()
	{
		// The ticket repro: Facade on the front Layer, a Room directly behind.
		core::World front("Door on Facade front", 8, 1);
		front.addLayer();
		auto const facadeIndex = front.addFacade(0, 0, 0, 4, 1);
		front.addRoom("Back", 1, 0, 0, 4, 1);
		front.finishBuild();
		front.pauseSimulation();

		auto const foreDoor = front.addSectorDoor(0, 0, 1);
		require(foreDoor.door.sector != nullptr
				&& foreDoor.door.sector->getIndex() == facadeIndex,
			"The Door was not authored in the Facade front Sector");
		require(std::as_const(front).getLayer(0)->getCellDefinition(1, 0).sectorObjectType
			== core::SectorObjectType::Door,
			"The Facade-side cell does not carry the Door");
		require(!std::as_const(front).getLayer(1)->getCellDefinition(1, 0).hasObject(),
			"The Door claimed object occupancy on its destination Layer");

		// The mirror: a Room in front with the Facade directly behind.
		core::World back("Door on Facade back", 8, 1);
		back.addLayer();
		back.addRoom("Front", 0, 0, 0, 4, 1);
		back.addFacade(1, 0, 0, 4, 1);
		back.finishBuild();
		back.pauseSimulation();

		auto const backDoor = back.addSectorDoor(0, 0, 1);
		require(backDoor.door.sector != nullptr
				&& backDoor.door.sector->getType() == core::SectorType::Location,
			"The Door was not authored in the Room front Sector");
		require(std::as_const(back).getLayer(0)->getCellDefinition(1, 0).sectorObjectType
			== core::SectorObjectType::Door,
			"The Room-side cell does not carry the Door");
		require(!std::as_const(back).getLayer(1)->getCellDefinition(1, 0).hasObject(),
			"The Door claimed Facade object occupancy on its destination Layer");
	}

	// Wall edits refuse the Facade itself - its perimeter is not editable -
	// while a neighbouring Room may still open its own wall into the Facade's
	// already-open side, so neighbours can merge inward (ADR 0003).
	void wallCommandsRefuseTheFacadeButNotTowardIt()
	{
		core::World world("Wall rules", 16, 3);
		auto const roomIndex = world.addRoom("Walled", 0, 0, 0, 4, 1);
		auto const facadeIndex = world.addFacade(0, 0, 4, 4, 1);
		world.finishBuild();
		world.pauseSimulation();

		std::string diagnostic;
		// The Facade has no walls to remove and none to add.
		require(!world.canRemoveLocationWall(facadeIndex, 0, CORE_SIDE_LEFT, &diagnostic),
			"A wall removal was accepted on a Facade");
		require(!world.canAddLocationWall(facadeIndex, 0, CORE_SIDE_LEFT, &diagnostic),
			"A wall addition was accepted on a Facade");

		// Opening the Room's wall into the Facade: the Facade side is already
		// open, so only the Room's half changes.
		require(world.canRemoveLocationWall(roomIndex, 0, CORE_SIDE_RIGHT, &diagnostic),
			("A Room could not open its own wall toward a Facade: " + diagnostic).c_str());
		world.removeLocationWall(roomIndex, 0, CORE_SIDE_RIGHT);
		require(world.getSector(roomIndex)->getEndType(0, CORE_SIDE_RIGHT)
			== core::SectorEndType::None,
			"The Room's wall toward the Facade did not open");
		everyEndIsOpen(*facadeIn(world, facadeIndex));

		// And it cannot be re-walled: the Facade refuses the other half.
		require(!world.canAddLocationWall(roomIndex, 0, CORE_SIDE_RIGHT, &diagnostic),
			"A wall was restored against a Facade");
	}

	// The Facade record round-trips: type name, footprint, top deck height,
	// and packed colour; and replay is stable across saves.
	void theFacadeRecordRoundTrips()
	{
		core::World world("Record keeper", 12, 3);
		world.addRoom("Room 0", 0, 0, 0, 4, 1);
		world.addFacade(1, 1, 4, 4, 2, CORE_ROOM_MAX_HEIGHT, { 176, 160, 128 });
		world.addFacade(1, 0, 8, 2, 1, CORE_ROOM_MAX_HEIGHT, { 12, 240, 6 });
		world.finishBuild();

		auto const yaml = serializeWorld(world);
		require(yaml.find("type: facade") != std::string::npos,
			"The Facade record was not written as 'facade'");
		require(yaml.find(std::format("colour: {}",
				core::packBackgroundColour(core::BackgroundColour{ 12, 240, 6 }))) != std::string::npos,
			"The Facade colour was not written packed");

		core::World loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getNumSectors() == 3, "The Facade records did not replay into Sectors");
		require(sectorSignature(loaded) == sectorSignature(world),
			("Facade replay did not reproduce the World\nexpected:\n"
				+ sectorSignature(world) + "actual:\n" + sectorSignature(loaded)).c_str());

		// Re-saving a replayed World changes nothing: the records are stable.
		require(serializeWorld(loaded) == yaml,
			"Re-saving a replayed Facade World changed its authored records");
	}

	// A hand-authored facade record loads, and one with the colour left out
	// takes the Facade default rather than the Background's.
	void aHandAuthoredFacadeRecordLoads()
	{
		auto const yaml = R"yaml(version: 6
name: Hand authored facade
cellsWide: 12
decksHigh: 3
layers: 2
layerNames:
  - Layer 0
  - Layer 1
construction:
  - type: facade
    layer: 0
    y: 1
    x: 2
    cellsWide: 3
    decksHigh: 2
    topDeckHeight: 0.9
    colour: 11261568
  - type: facade
    layer: 1
    y: 0
    x: 0
    cellsWide: 1
    decksHigh: 1
    topDeckHeight: 0.9
agents: []
)yaml";

		core::World loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getNumSectors() == 2, "The hand-authored Facades did not replay");

		auto const first = facadeIn(loaded, 0);
		require(first->getColour() == core::unpackBackgroundColour(11261568),
			"A hand-authored Facade colour was not read");
		require(first->getLayerIndex() == 0 && first->getCellY() == 1 && first->getCellX() == 2
			&& first->getCellsWide() == 3 && first->getDecksHigh() == 2,
			"The hand-authored Facade footprint was not read");
		everyEndIsOpen(*first);

		auto const second = facadeIn(loaded, 1);
		require(second->getColour() == core::Facade::defaultColour(),
			"A Facade record without a colour did not take the Facade default");
		everyEndIsOpen(*second);
	}

	// ---- Ticket #44: save, load, and replay ----

	// The Facade record carries the Facade's own name alongside its footprint
	// and packed colour, so a named Facade survives the round-trip; the
	// unnamed creation path keeps the generic name.
	void theFacadeRecordCarriesItsName()
	{
		core::World world("Named frontage", 12, 3);
		auto const named = world.addFacade("Shopfront", 0, 0, 0, 3, 1);
		auto const plain = world.addFacade(0, 0, 4, 3, 1);
		world.finishBuild();

		require(world.getSector(named)->getName() == "Shopfront",
			"addFacade() did not give the Sector the name it was handed");

		auto const yaml = serializeWorld(world);
		require(yaml.find("name: Shopfront") != std::string::npos,
			"The Facade record did not carry its name");

		core::World loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getSector(named)->getName() == "Shopfront",
			"The Facade name was lost in the round-trip");
		require(loaded.getSector(plain)->getName() == core::Facade::defaultName(),
			"An unnamed Facade did not keep the generic Facade name");
	}

	// A hand-authored record that leaves the name out lands on the same Sector
	// an unnamed addFacade() call produces - the default is one rule, not two.
	void aHandAuthoredFacadeWithoutANameTakesTheDefaultName()
	{
		auto const yaml = R"yaml(version: 6
name: Unnamed frontage
cellsWide: 12
decksHigh: 3
layers: 2
layerNames:
  - Layer 0
  - Layer 1
construction:
  - type: facade
    layer: 0
    y: 0
    x: 0
    cellsWide: 2
    decksHigh: 1
    topDeckHeight: 0.9
    colour: 11261568
agents: []
)yaml";

		core::World loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getNumSectors() == 1, "The nameless hand-authored Facade did not replay");
		require(loaded.getSector(0)->getName() == core::Facade::defaultName(),
			"A Facade record without a name did not take the Facade default name");
		everyEndIsOpen(*facadeIn(loaded, 0));
	}

	// Acceptance #44: the save/load round-trip preserves the Facade's colour,
	// its open ends, and the record-to-sector index mapping. The World is
	// authored with a Ladder Transit written between sector producers, so
	// canonical replay reorders the records - the exact case a Facade record
	// left out of the sector-producing set would shift out of place.
	void saveLoadPreservesColourOpenEndsAndSectorIndexMapping()
	{
		core::World world("Index mapping", 12, 3);
		auto const room = world.addRoom("Room A", 0, 0, 0, 6, 2);
		auto const facade = world.addFacade("Frontage", 0, 0, 6, 3, 3,
			CORE_ROOM_MAX_HEIGHT, { 210, 20, 130 });
		uint32_t facadeMarker = 0;
		world.addSectorMarker(facade, 0, 1.25f, &facadeMarker);
		world.addSectorLightSwitch(room, 2);
		// The Corridor behind the Room gives the Ladder its second landing, and
		// is authored after the objects that reference the first two Sectors.
		auto const backCorridor = world.addCorridor(0, 2, 0, 6, 1);
		// A reference to a Sector that comes after the Facade in the producer
		// order: this is the reference a mis-counted Facade would shift.
		uint32_t backCorridorMarker = 0;
		world.addSectorMarker(backCorridor, 0, 1.0f, &backCorridorMarker);
		auto const ladder = world.addLadder(1, 0, 3, { 3, false, true }).ladder.sector->getIndex();
		auto const background = world.addBackground(1, 0, 0, 3, 1);
		auto const corridor = world.addCorridor(1, 2, 4, 4, 1);
		world.finishBuild();

		// Authored order: Room 0, Facade 1, Corridor 2, Ladder 3, Background 4,
		// Corridor 5. Canonical replay moves the Transit behind the space
		// producers, so the Sector indices only line up if the Facade counts as
		// a producer in the same pass as the Rooms.
		require(room == 0 && facade == 1 && backCorridor == 2 && ladder == 3
			&& background == 4 && corridor == 5,
			"The authored Sector indices are not the shape this check replays against");

		auto const yaml = serializeWorld(world);
		core::World loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);

		require(loaded.getNumSectors() == world.getNumSectors(),
			"The round-trip changed the Sector count");
		require(sectorSignature(loaded) == sectorSignature(world),
			("The round-trip changed a Sector\nexpected:\n" + sectorSignature(world)
				+ "actual:\n" + sectorSignature(loaded)).c_str());
		require(objectSignature(loaded) == objectSignature(world),
			("The round-trip moved a SectorObject between Sectors\nexpected:\n"
				+ objectSignature(world) + "actual:\n" + objectSignature(loaded)).c_str());

		// Spelled out for the Facade itself: same index, same name, same
		// colour, and still every end open.
		auto const replayed = facadeIn(loaded, facade);
		require(replayed->getName() == "Frontage",
			"The Facade lost its name across the canonical replay");
		require(replayed->getColour() == core::BackgroundColour(210, 20, 130),
			"The Facade lost its colour across the canonical replay");
		everyEndIsOpen(*replayed);

		// The Marker authored inside the Facade is still inside it, and the
		// Light Switch authored in the Room is still in the Room.
		require(sectorHasObject(loaded, facade, core::SectorObjectType::Marker),
			"The Marker authored in the Facade replayed into another Sector");
		require(sectorHasObject(loaded, room, core::SectorObjectType::InteractionPoint),
			"The Light Switch authored in the Room replayed into another Sector");
		require(sectorHasObject(loaded, backCorridor, core::SectorObjectType::Marker),
			"The Marker authored in the back Corridor replayed into another Sector");

		// Re-saving the replayed World writes the same records: the
		// canonical form is a fixed point, so the Facade does not drift.
		require(serializeWorld(loaded) == yaml,
			"Re-saving a replayed Facade World changed its authored records");

		// The producer-set membership shows up again wherever records are
		// addressed by Sector index: resizing a Sector authored after the
		// Facade has to find that Sector's own record, not the one the Facade
		// would have skipped over, and has to leave the Marker inside it alone.
		loaded.pauseSimulation();
		auto const resize = loaded.planResizeLocation(backCorridor, 0, 2, 5, 1);
		require(resize.valid, ("Resizing a Sector authored after the Facade was refused: "
			+ resize.diagnostic).c_str());
		loaded.applyLocationEdit(resize);
		require(loaded.getSector(backCorridor)->getCellsWide() == 5,
			"The resized Sector is not the one the edit changed");
		require(sectorHasObject(loaded, backCorridor, core::SectorObjectType::Marker),
			"The back Corridor Marker was lost by the remapping edit");
		require(loaded.getSector(facade)->getType() == core::SectorType::Facade
			&& loaded.getSector(facade)->getName() == "Frontage",
			"The Facade was remapped out of its Sector index by the edit");
		everyEndIsOpen(*facadeIn(loaded, facade));
	}

	// Acceptance #44: a Facade never writes wall-removal records. Its open
	// perimeter is a type property rather than an edit, so however many decks
	// it spans the whole Facade is one record and zero RemoveWall records,
	// and the replay needs no wall edits to leave every end open.
	void aFacadeNeverWritesWallRemovalRecords()
	{
		core::World world("No walls to remove", 12, 3);
		world.addFacade("Tall frontage", 0, 0, 0, 4, 3);
		world.addFacade(1, 0, 6, 2, 2);
		world.finishBuild();

		auto const yaml = serializeWorld(world);
		require(countOccurrences(yaml, "removeWall") == 0,
			"A saved Facade wrote a wall-removal record");
		require(countOccurrences(yaml, "type: facade") == 2,
			"A Facade is not carried by exactly one construction record");
		require(countOccurrences(yaml, "type: ") == 2,
			"A Facade-only World wrote records beyond its own");

		core::World loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getNumSectors() == 2, "The Facade records did not replay into two Sectors");
		everyEndIsOpen(*facadeIn(loaded, 0));
		everyEndIsOpen(*facadeIn(loaded, 1));
	}

	// No order dependence: a Facade record replays with every end open
	// wherever it sits in the construction array, since it needs no follow-up
	// wall edits to reach that state.
	void facadeRecordsReplayInAnyOrder()
	{
		auto const check = [](std::string const& facadeRecord, std::string const& otherRecords,
			char const* what)
		{
			auto const yaml = std::string("version: 6\nname: Order test\ncellsWide: 12\ndecksHigh: 3\n"
				"layers: 2\nlayerNames:\n  - Layer 0\n  - Layer 1\nconstruction:\n")
				+ facadeRecord + otherRecords + "agents: []\n";

			core::World loaded("placeholder", 1, 1);
			loadInto(loaded, yaml);
			require(loaded.getNumSectors() == 2,
				(std::string(what) + ": the Facade and Room did not both replay").c_str());
			require(countOccurrences(serializeWorld(loaded), "removeWall") == 0,
				(std::string(what) + ": replaying the Facade produced a wall-removal record").c_str());
			for (uint32_t index = 0; index < loaded.getNumSectors(); ++index)
			{
				if (loaded.getSector(index)->getType() == core::SectorType::Facade)
					everyEndIsOpen(*loaded.getSector(index));
			}
		};

		auto const facadeRecord = std::string("  - type: facade\n    layer: 0\n    y: 0\n    x: 0\n"
			"    cellsWide: 3\n    decksHigh: 2\n    topDeckHeight: 0.9\n    colour: 11261568\n");
		auto const roomRecord = std::string("  - type: room\n    name: Room A\n    layer: 0\n    y: 0\n"
			"    x: 4\n    cellsWide: 3\n    decksHigh: 2\n    topDeckHeight: 0.9\n");

		check(facadeRecord, roomRecord, "Facade record first");
		check(roomRecord, facadeRecord, "Facade record last");
	}

	// Acceptance #44: a Facade map opened by pre-Facade code is rejected,
	// not silently misread. The current writer bumps the version above the
	// pre-Facade ceiling so the old reader refuses the whole file; and a
	// facade record reaching the old name table fails as an unknown
	// construction type rather than being dropped on the floor.
	void aFacadeMapIsRejectedByPreFacadeCode()
	{
		core::World world("Facade map", 12, 3);
		world.addRoom("Room A", 0, 0, 0, 3, 1);
		world.addFacade("Frontage", 0, 0, 3, 3, 1);
		world.finishBuild();

		auto const yaml = serializeWorld(world);
		require(yaml.find("version: 14") != std::string::npos,
			"The writer did not raise the version above the pre-Door-style ceiling");

		bool refusedVersion = false;
		try
		{
			preFacadeReaderReads(yaml);
		}
		catch (core::SerializationException const& error)
		{
			refusedVersion = true;
			require(std::string(error.what()).find("version") != std::string::npos,
				("A pre-Facade reader failed for a reason other than the version: "
					+ std::string(error.what())).c_str());
		}
		require(refusedVersion, "A pre-Facade reader accepted a Facade map");

		// And a facade record reaching the old record-name table - the shape an
		// un-versioned Facade write would leave behind - fails loudly as an
		// unknown construction type instead of vanishing.
		auto const legacyFacadeMap = R"yaml(version: 5
name: Legacy facade
cellsWide: 12
decksHigh: 3
layers: 2
layerNames:
  - Layer 0
  - Layer 1
construction:
  - type: room
    name: Room A
    layer: 0
    y: 0
    x: 0
    cellsWide: 3
    decksHigh: 1
    topDeckHeight: 0.9
  - type: facade
    name: Frontage
    layer: 0
    y: 0
    x: 3
    cellsWide: 3
    decksHigh: 1
    topDeckHeight: 0.9
    colour: 11261568
agents: []
)yaml";

		bool refusedType = false;
		try
		{
			preFacadeReaderReads(legacyFacadeMap);
		}
		catch (core::SerializationException const& error)
		{
			refusedType = true;
			require(std::string(error.what()).find("facade") != std::string::npos,
				("The pre-Facade unknown-record error did not name the type: "
					+ std::string(error.what())).c_str());
		}
		require(refusedType, "A pre-Facade reader silently accepted a 'facade' record");

		// Control: the same reader is content with the version 5 map it was
		// written for, so the refusals above are the Facade and not the harness.
		auto const legacyPlainMap = R"yaml(version: 5
name: Legacy plain
cellsWide: 12
decksHigh: 3
layers: 2
layerNames:
  - Layer 0
  - Layer 1
construction:
  - type: room
    name: Room A
    layer: 0
    y: 0
    x: 0
    cellsWide: 3
    decksHigh: 1
    topDeckHeight: 0.9
  - type: background
    layer: 1
    y: 0
    x: 0
    cellsWide: 3
    decksHigh: 1
    colour: 6340864
agents: []
)yaml";
		require(!throws([&] { preFacadeReaderReads(legacyPlainMap); }),
			"The pre-Facade reader harness refused a map it should accept");
	}

	// The Facade takes part in the Graph: with the shared wall opened, a
	// Marker inside the Facade is reachable from the neighbouring Room's
	// side of the row, and with the Room's wall left standing there is no
	// route at all - the opened boundary is what connects them.
	void theFacadeTakesPartInTheGraph()
	{
		core::World open("Merged", 16, 3);
		auto const roomIndex = open.addRoom("Neighbour", 0, 0, 0, 4, 1);
		auto const facadeIndex = open.addFacade(0, 0, 4, 4, 1);
		uint32_t roomMarkerIdentifier = 0;
		uint32_t facadeMarkerIdentifier = 0;
		open.addSectorMarker(roomIndex, 0, 1.0f, &roomMarkerIdentifier);
		open.addSectorMarker(facadeIndex, 0, 1.5f, &facadeMarkerIdentifier);
		open.removeLocationWall(roomIndex, 0, CORE_SIDE_RIGHT);
		open.finishBuild();
		auto const openAgentId = open.createAgent("Route checker", roomIndex, 0, 1.0f);
		auto const openAgent = open.lookupAgent(openAgentId).entity;
		require(openAgent != nullptr, "The open-boundary route checker was not created");

		auto const source = open.getGraph()->getVertexByIdentifier(roomMarkerIdentifier);
		auto const destination = open.getGraph()->getVertexByIdentifier(facadeMarkerIdentifier);
		require(source != nullptr && destination != nullptr,
			"A Marker in the Room/Facade pair has no Graph vertex");
		auto path = open.getGraph()->calculatePath(openAgent, source, destination);
		require(path && !path->nodes.empty(),
			"No path exists from a Room into a Facade through the opened boundary");

		core::World sealed("Sealed", 16, 3);
		auto const sealedRoom = sealed.addRoom("Neighbour", 0, 0, 0, 4, 1);
		auto const sealedFacade = sealed.addFacade(0, 0, 4, 4, 1);
		uint32_t sealedRoomMarker = 0;
		uint32_t sealedFacadeMarker = 0;
		sealed.addSectorMarker(sealedRoom, 0, 1.0f, &sealedRoomMarker);
		sealed.addSectorMarker(sealedFacade, 0, 1.5f, &sealedFacadeMarker);
		sealed.finishBuild();
		auto const sealedAgentId = sealed.createAgent("Route checker", sealedRoom, 0, 1.0f);
		auto const sealedAgent = sealed.lookupAgent(sealedAgentId).entity;
		require(sealedAgent != nullptr, "The sealed-boundary route checker was not created");

		auto const blocked = sealed.getGraph()->calculatePath(sealedAgent,
			sealed.getGraph()->getVertexByIdentifier(sealedRoomMarker),
			sealed.getGraph()->getVertexByIdentifier(sealedFacadeMarker));
		require(!blocked || blocked->nodes.empty(),
			"A walled boundary still routed into the Facade");
	}

	// A Layer deletion drops the Facades on it like any other Location and
	// keeps the rest of the World replayable.
	void layerDeletionHandlesFacadeRecords()
	{
		core::World world("Compaction", 12, 3);
		world.addRoom("Front", 0, 0, 0, 4, 1);
		world.addFacade(1, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT, { 99, 99, 99 });
		world.addLayer();
		world.finishBuild();

		auto const plan = world.planDeleteLayer(1);
		require(plan.valid, ("Deleting the Facade's Layer was refused: " + plan.diagnostic).c_str());
		require(plan.locationsRemoved == 1,
			"The deleted Facade was not counted as a removed Location");
		require(world.applyDeleteLayer(plan), "The Facade Layer deletion did not apply");
		require(world.getNumSectors() == 1,
			"The Facade Sector survived the deletion of its Layer");
		require(world.getSector(0)->getType() == core::SectorType::Location,
			"The surviving Sector is not the front Room");
	}

	// Ticket #55: the Agent restore loops after a Layer delete, a Location
	// edit, and a Background edit drop Agents left standing on non-traversable
	// cells - but the guard used to fire only for a plain Location, so an Agent
	// saved on a Facade's upper deck with no Walkway came back standing on air.
	// The guard follows isLocationLike(), so a Facade is held to the same floor.
	void restoreDropsAgentsOnNonTraversableFacadeCells()
	{
		auto const requireRestore = [](core::World& world,
			core::AgentId groundId, core::AgentId airId, char const* path)
		{
			require(world.lookupAgent(groundId).entity != nullptr,
				(std::string("The Agent on the Facade's walkable floor was not restored across ")
					+ path).c_str());
			require(world.lookupAgent(airId).entity == nullptr,
				(std::string("The Agent on the Facade's non-traversable deck was restored across ")
					+ path).c_str());
		};

		// Path 1: applyDeleteLayer.
		{
			core::World world("Restore floor", 12, 3);
			auto const facadeIndex = world.addFacade(1, 0, 0, 2, 2);
			world.addLayer();
			world.finishBuild();
			require(!std::as_const(world).getLayer(1)->getCellDefinition(0, 1).isTraversableOnFoot(),
				"The Facade's upper deck is unexpectedly walkable; the restore guard would be vacuous");
			auto const groundId = world.createAgent("Grounded", facadeIndex, 0, 0.5f);
			auto const airId = world.createAgent("On air", facadeIndex, 1, 0.5f);
			auto const plan = world.planDeleteLayer(0);
			require(plan.valid, ("The front Layer delete plan was refused: " + plan.diagnostic).c_str());
			require(world.applyDeleteLayer(plan), "The Layer delete did not apply");
			requireRestore(world, groundId, airId, "a Layer delete");
		}

		// Path 2: applyLocationEdit (a Room resize elsewhere replays the world).
		{
			core::World world("Restore edit", 12, 2);
			auto const roomIndex = world.addRoom("Shifter", 0, 0, 0, 2, 1);
			auto const facadeIndex = world.addFacade(0, 0, 4, 2, 2);
			world.finishBuild();
			auto const groundId = world.createAgent("Grounded", facadeIndex, 0, 0.5f);
			auto const airId = world.createAgent("On air", facadeIndex, 1, 0.5f);
			auto const plan = world.planResizeLocation(roomIndex, 0, 0, 3, 1);
			require(plan.valid, ("The Room resize plan was refused: " + plan.diagnostic).c_str());
			world.applyLocationEdit(plan);
			requireRestore(world, groundId, airId, "a Location edit");
		}

		// Path 3: applyBackgroundEdit.
		{
			core::World world("Restore bg edit", 12, 2);
			auto const backgroundIndex = world.addBackground(1, 0, 0, 2, 1, { 10, 20, 30 });
			auto const facadeIndex = world.addFacade(0, 0, 0, 2, 2);
			world.finishBuild();
			auto const groundId = world.createAgent("Grounded", facadeIndex, 0, 0.5f);
			auto const airId = world.createAgent("On air", facadeIndex, 1, 0.5f);
			auto const plan = world.planResizeBackground(backgroundIndex, 0, 0, 3, 1);
			require(plan.valid, ("The Background resize plan was refused: " + plan.diagnostic).c_str());
			world.applyBackgroundEdit(plan);
			requireRestore(world, groundId, airId, "a Background edit");
		}
	}

	// Ticket #55: a Facade describes itself by its own name, the way Location
	// does; only an unnamed Facade reads as "Facade".
	void theDescriptionCarriesTheFacadeName()
	{
		core::World world("Naming", 12, 2);
		auto const named = world.addFacade("Frontage", 0, 0, 0, 2, 1);
		auto const unnamed = world.addFacade(0, 0, 2, 2, 1);
		world.finishBuild();
		require(facadeIn(world, named)->getDescription() == "Frontage at 0,0 on Layer 0",
			("The named Facade describes itself as '" + facadeIn(world, named)->getDescription() + "'").c_str());
		require(facadeIn(world, unnamed)->getDescription() == "Facade at 2,0 on Layer 0",
			("The unnamed Facade describes itself as '" + facadeIn(world, unnamed)->getDescription() + "'").c_str());
	}

	// Ticket #55: the height range check is a negated in-range test so a NaN
	// topDeckHeight is rejected instead of sailing through both one-sided
	// comparisons - the same flaw the plain < / > pair had in addRoom.
	void nanTopDeckHeightIsRejected()
	{
		core::World world("NaN", 12, 2);
		world.finishBuild();
		float const nan = std::numeric_limits<float>::quiet_NaN();
		std::string diagnostic;
		require(!world.canAddFacade(0, 0, 0, 2, 1, nan, &diagnostic),
			"canAddFacade accepted a NaN topDeckHeight");
		require(throws([&] { world.addFacade(0, 0, 0, 2, 1, nan); }),
			"addFacade accepted a NaN topDeckHeight");
		require(throws([&] { world.addRoom("NaN room", 0, 0, 0, 2, 1, nan); }),
			"addRoom accepted a NaN topDeckHeight");
	}

	bool hasPath(core::World const& world, core::Agent const* agent,
		uint32_t fromIdentifier, uint32_t toIdentifier)
	{
		auto const source = world.getGraph()->getVertexByIdentifier(fromIdentifier);
		auto const target = world.getGraph()->getVertexByIdentifier(toIdentifier);
		if (!source || !target) return false;
		auto const path = world.getGraph()->calculatePath(agent, source, target);
		return path && !path->nodes.empty();
	}

	// Ticket #45: a Facade placed between two floor-aligned Rooms, each Room
	// opening its own wall into the Facade's already-open half, is one
	// continuous floor. The Graph merges the three Sectors across both
	// boundaries, and the Room-to-Room route runs through the Facade on
	// ordinary Sector Edges - no threshold of any kind joins them.
	void facadeBetweenTwoAlignedRoomsIsOneContinuousFloor()
	{
		core::World world("Three in a row", 16, 3);
		auto const roomA = world.addRoom("Room A", 0, 0, 0, 4, 1);
		auto const facade = world.addFacade(0, 0, 4, 4, 1);
		auto const roomB = world.addRoom("Room B", 0, 0, 8, 4, 1);
		uint32_t markerA = 0;
		uint32_t markerF = 0;
		uint32_t markerB = 0;
		world.addSectorMarker(roomA, 0, 1.0f, &markerA);
		world.addSectorMarker(facade, 0, 2.0f, &markerF);
		world.addSectorMarker(roomB, 0, 1.0f, &markerB);
		world.pauseSimulation();
		world.removeLocationWall(roomA, 0, CORE_SIDE_RIGHT);
		world.removeLocationWall(roomB, 0, CORE_SIDE_LEFT);
		world.finishBuild();
		auto const agentAId = world.createAgent("Room A route checker", roomA, 0, 1.0f);
		auto const agentFId = world.createAgent("Facade route checker", facade, 0, 2.0f);
		auto const agentA = world.lookupAgent(agentAId).entity;
		auto const agentF = world.lookupAgent(agentFId).entity;
		require(agentA != nullptr && agentF != nullptr,
			"The continuous-floor route checkers were not created");

		auto const graph = world.getGraph();
		auto const source = graph->getVertexByIdentifier(markerA);
		auto const target = graph->getVertexByIdentifier(markerB);
		require(source != nullptr && target != nullptr,
			"A Marker in the three-Sector run has no Graph vertex");
		auto const path = graph->calculatePath(agentA, source, target);
		// Source Vertex plus one hop per boundary crossed: A -> Facade -> B.
		require(path && path->nodes.size() == 3,
			"The Room-to-Room route is not one continuous run through the Facade");

		bool throughFacade = false;
		for (auto const& node : path->nodes)
		{
			require(node.targetVertex != nullptr, "A path node carries no Vertex");
			if (node.targetVertex->getSector()->getIndex() == facade) throughFacade = true;
			// The first node is the source itself and carries no Edge; every
			// hop that follows is plain floor adjacency.
			if (!node.edge) continue;
			require(std::dynamic_pointer_cast<const core::SectorEdge>(node.edge) != nullptr,
				"The continuous floor was joined by something other than a Sector Edge");
		}
		require(throughFacade,
			"The Room-to-Room path did not cross the Facade");
		require(hasPath(world, agentA, markerA, markerF)
			&& hasPath(world, agentF, markerF, markerB),
			"The merged run is not walkable in both directions through the Facade");
	}

	// Ticket #45: floors must still match at the boundary. A Facade beside a
	// Room one deck higher does not merge - no route crosses, and the
	// wall-removal command refuses the boundary exactly as it does between
	// mismatched-floor Rooms - while the same pair sharing a deck does merge
	// once the Room opens its wall, so the refusal is the mismatch and not
	// the Facade.
	void facadeBesideHigherFloorDoesNotMerge()
	{
		std::string diagnostic;

		// Facade floor: row 0. Room floors: rows 1-2, one deck higher at the
		// boundary.
		core::World mismatched("Mismatched", 12, 3);
		auto const facade = mismatched.addFacade(0, 0, 0, 4, 1);
		auto const room = mismatched.addRoom("Higher", 0, 1, 4, 2, 1);
		uint32_t facadeMarker = 0;
		uint32_t roomMarker = 0;
		mismatched.addSectorMarker(facade, 0, 2.0f, &facadeMarker);
		mismatched.addSectorMarker(room, 0, 1.0f, &roomMarker);
		mismatched.finishBuild();
		auto const mismatchedAgentId = mismatched.createAgent("Route checker", facade, 0, 2.0f);
		auto const mismatchedAgent = mismatched.lookupAgent(mismatchedAgentId).entity;
		require(mismatchedAgent != nullptr, "The mismatched-floor route checker was not created");

		require(!hasPath(mismatched, mismatchedAgent, facadeMarker, roomMarker),
			"A Facade merged with a Room one deck higher at the boundary");
		require(!mismatched.canRemoveLocationWall(room, 0, CORE_SIDE_LEFT, &diagnostic),
			"A wall removal was accepted against a Facade that does not share the deck");

		// Control: the same pair sharing row 0 merges once the Room opens its
		// wall into the Facade's open half.
		core::World aligned("Aligned", 12, 3);
		auto const facade2 = aligned.addFacade(0, 0, 0, 1, 1);
		auto const room2 = aligned.addRoom("Sharing", 0, 0, 1, 2, 1);
		uint32_t facadeMarker2 = 0;
		uint32_t roomMarker2 = 0;
		aligned.addSectorMarker(facade2, 0, 0.5f, &facadeMarker2);
		aligned.addSectorMarker(room2, 0, 1.0f, &roomMarker2);
		aligned.pauseSimulation();
		require(aligned.canRemoveLocationWall(room2, 0, CORE_SIDE_LEFT, &diagnostic),
			("A Room could not open its wall into a Facade sharing its deck: " + diagnostic).c_str());
		aligned.removeLocationWall(room2, 0, CORE_SIDE_LEFT);
		aligned.finishBuild();
		auto const alignedAgentId = aligned.createAgent("Route checker", facade2, 0, 0.5f);
		auto const alignedAgent = aligned.lookupAgent(alignedAgentId).entity;
		require(alignedAgent != nullptr, "The aligned-floor route checker was not created");
		require(hasPath(aligned, alignedAgent, facadeMarker2, roomMarker2),
			"Floor-aligned Facade and Room did not merge across the opened boundary");
	}

	// Ticket #45: the wall-removal command accepts a Facade neighbour on
	// either side of the boundary - the Room opens its own wall into the
	// Facade's already-open half - while the Facade's own perimeter stays
	// uneditable. (The Facade-side refusal is the ADR 0003 invariant; the
	// neighbour-side acceptance is the widening.)
	void wallRemovalAcceptsFacadeNeighboursBothWays()
	{
		core::World world("Both ways", 16, 3);
		auto const facade = world.addFacade(0, 0, 4, 4, 1);
		auto const roomLeft = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto const roomRight = world.addRoom("Right", 0, 0, 8, 4, 1);
		world.finishBuild();
		world.pauseSimulation();

		std::string diagnostic;
		require(world.canRemoveLocationWall(roomLeft, 0, CORE_SIDE_RIGHT, &diagnostic),
			("A Room could not open its wall toward a Facade on its right: " + diagnostic).c_str());
		require(world.canRemoveLocationWall(roomRight, 0, CORE_SIDE_LEFT, &diagnostic),
			("A Room could not open its wall toward a Facade on its left: " + diagnostic).c_str());
		require(!world.canRemoveLocationWall(facade, 0, CORE_SIDE_LEFT, &diagnostic),
			"A wall removal was accepted on a Facade (left side)");
		require(!world.canRemoveLocationWall(facade, 0, CORE_SIDE_RIGHT, &diagnostic),
			"A wall removal was accepted on a Facade (right side)");

		world.removeLocationWall(roomLeft, 0, CORE_SIDE_RIGHT);
		world.removeLocationWall(roomRight, 0, CORE_SIDE_LEFT);
		require(world.getSector(roomLeft)->getEndType(0, CORE_SIDE_RIGHT) == core::SectorEndType::None
			&& world.getSector(roomRight)->getEndType(0, CORE_SIDE_LEFT) == core::SectorEndType::None,
			"An opened Room wall did not open toward the Facade");
		everyEndIsOpen(*facadeIn(world, facade));
	}

	// Ticket #45: a Facade beside a Background has no pathing interaction in
	// either direction. The Background contributes no Vertices, and the
	// Facade grows nothing toward it: the only Vertex in the World is the
	// Facade's own Marker.
	void facadeBesideBackgroundHasNoPathingInteraction()
	{
		auto const check = [](uint32_t facadeX, uint32_t backgroundX, char const* what)
		{
			core::World world("Facade beside Background", 12, 3);
			auto const facade = world.addFacade(0, 0, facadeX, 3, 1);
			auto const background = world.addBackground(0, 0, backgroundX, 3, 1);
			uint32_t marker = 0;
			world.addSectorMarker(facade, 0, 1.0f, &marker);
			world.finishBuild();

			auto const graph = world.getGraph();
			require(graph->getVertexByIdentifier(marker) != nullptr,
				(std::string(what) + ": the Facade Marker lost its Vertex").c_str());
			for (auto const& vertex : graph->getVertices())
			{
				require(vertex->getSector()->getType() != core::SectorType::Background,
					(std::string(what) + ": the Background contributed a Graph Vertex").c_str());
				require(vertex->getSector()->getIndex() == facade,
					(std::string(what) + ": an unexpected Sector reached the Graph").c_str());
			}
			require(graph->getVertices().size() == 1,
				(std::string(what) + ": the Facade grew Vertices toward the Background").c_str());
			require(world.getSector(background)->getType() == core::SectorType::Background,
				(std::string(what) + ": the Background Sector went missing").c_str());
		};

		check(0, 3, "Facade left of Background");
		check(3, 0, "Background left of Facade");
	}

	//
	// Ticket #52: Transit landings.
	//
	// Before #52 each Transit kept its own landing rule - Lift and Staircase
	// accepted a Facade through the Location cast, Ladder, Stairwell and
	// Shuttle refused it on a plain SectorType::Location identity test. The
	// rule is now one rule: a Transit lands on anything isLocationLike()
	// admits, so a Facade lands exactly as a Room does and a Background -
	// which is not in the pathing world at all - still does not.
	//
	// The three Sectors a Transit can be offered on the Layer in front of it.
	enum class LandingKind { Facade, Room, Background };

	std::string nameOf(LandingKind kind)
	{
		switch (kind)
		{
		case LandingKind::Facade: return "Facade";
		case LandingKind::Room: return "Room";
		case LandingKind::Background: return "Background";
		}
		return "Unknown";
	}

	uint32_t addLanding(core::World& world, LandingKind kind, uint32_t layer,
		uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh)
	{
		switch (kind)
		{
		case LandingKind::Facade:
			return world.addFacade(layer, y, x, cellsWide, decksHigh);
		case LandingKind::Room:
			return world.addRoom("Landing", layer, y, x, cellsWide, decksHigh);
		case LandingKind::Background:
			return world.addBackground(layer, y, x, cellsWide, decksHigh);
		}
		throw std::runtime_error("Unknown landing kind");
	}

	// What the add itself says about a landing, so a refusal arrives with its
	// reason rather than as a bare boolean.
	std::string addRefusal(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (std::exception const& e)
		{
			return e.what();
		}
		return {};
	}

	// Every Transit type offered the same landing geometry with each of the
	// three landing Sectors. The Facade column must match the Room column and
	// the Background column must stay refused, on both the canAdd query and
	// the add itself, so the palette preview and the build cannot disagree.
	void everyTransitLandsOnAFacadeAsItDoesOnARoom()
	{
		for (auto const kind : { LandingKind::Facade, LandingKind::Room, LandingKind::Background })
		{
			auto const expected = kind != LandingKind::Background;
			std::string diagnostic;

			auto const verdict = [&](char const* transit, bool accepted)
			{
				require(accepted == expected,
					std::format("A {} landing on a {} was {} (expected {}): {}",
						transit, nameOf(kind), accepted ? "accepted" : "refused",
						expected ? "accepted" : "refused", diagnostic).c_str());
			};

			// Ladder: the Sector under test is the lower landing, a fixed Room
			// the upper one, both on the Layer in front of the Ladder's own.
			{
				core::World world("Ladder landing", 8, 3);
				addLanding(world, kind, 0, 0, 0, 1, 1);
				world.addRoom("Above", 0, 1, 0, 1, 2);
				verdict("Ladder", world.canAddLadder(1, 0, 0, 2, &diagnostic));
				auto const refusal = addRefusal([&] { world.addLadder(1, 0, 0, { 2, false, true }); });
				require(refusal.empty() == expected,
					std::format("addLadder() over a {} landing said: {}", nameOf(kind), refusal).c_str());
			}

			// Stairwell: two cells of the Sector under test on each landing deck.
			{
				core::World world("Stairwell landing", 8, 3);
				addLanding(world, kind, 0, 0, 0, 2, 1);
				world.addRoom("Above", 0, 1, 0, 2, 2);
				verdict("Stairwell", world.canAddStairwell(1, 0, 0, 2, &diagnostic));
				auto const refusal = addRefusal([&] { world.addStairwell(1, 0, 0, 2, CORE_SIDE_RIGHT); });
				require(refusal.empty() == expected,
					std::format("addStairwell() over a {} landing said: {}", nameOf(kind), refusal).c_str());
			}

			// Lift: two stacked landing Sectors, one per stop row - only a
			// Sector's own bottom deck is walkable floor - with a one-cell shaft
			// inside them so each stop keeps its call-button space. The landing
			// rows are the same read the palette preview uses, and the add
			// follows them.
			{
				core::World world("Lift landing", 8, 3);
				addLanding(world, kind, 0, 0, 0, 3, 1);
				addLanding(world, kind, 0, 1, 0, 3, 1);
				core::World::CreateLiftOptions options;
				options.cellsWide = 1;
				options.stopOffsets = { 0, 1 };
				auto const rows = world.getLiftLandingRows(1, 0, 1, 1, 2);
				size_t usable = 0;
				for (auto const& row : rows)
					if (row.usableForStop()) ++usable;
				require(usable == (expected ? 2u : 0u),
					std::format("The Lift found {} usable landing rows over a {} (expected {})",
						usable, nameOf(kind), expected ? 2u : 0u).c_str());
				auto const refusal = addRefusal([&] { world.addLift(1, 0, 1, options); });
				require(refusal.empty() == expected,
					std::format("addLift() over a {} landing said: {}", nameOf(kind), refusal).c_str());
			}

			// Shuttle: one carriage whose door cell lands on the Sector under
			// test at both stops.
			{
				core::World world("Shuttle landing", 16, 3);
				addLanding(world, kind, 0, 0, 6, 7, 1);
				core::World::CreateShuttleOptions options{ 1, 3, { 0, 4 }, 0 };
				auto const refusal = addRefusal([&] { world.addShuttle(1, 0, 6, 7, options); });
				require(refusal.empty() == expected,
					std::format("addShuttle() landing its carriage doors on a {} said: {}",
						nameOf(kind), refusal).c_str());
			}

			// Staircase: the Sector under test is the lower landing and the
			// upper landing's own Sector; the upper landing meets it through
			// the open right wall the two share (a Facade's half is already
			// open, so only the neighbour's wall comes down).
			{
				core::World world("Staircase landing", 8, 3);
				auto const landing = addLanding(world, kind, 0, 0, 0, 2, 2);
				auto const next = world.addRoom("Next", 0, 1, 2, 1, 1);
				if (expected)
				{
					world.pauseSimulation();
					// One removal opens both halves of the boundary; a Facade's
					// half is already open, so there the neighbour's wall is the
					// only one standing.
					if (kind == LandingKind::Room)
						world.removeLocationWall(landing, 1, CORE_SIDE_RIGHT);
					else
						world.removeLocationWall(next, 0, CORE_SIDE_LEFT);
				}
				verdict("Staircase", world.canAddStaircase(1, 0, 0, 2, CORE_SIDE_RIGHT, &diagnostic));
				auto const refusal = addRefusal([&] { world.addStaircase(1, 0, 0, 2, CORE_SIDE_RIGHT, 0.5f); });
				require(refusal.empty() == expected,
					std::format("addStaircase() over a {} landing said: {}", nameOf(kind), refusal).c_str());
			}
		}
	}

	// One World with every Transit type landing on a Facade: the Ladder and
	// the Stairwell take a Facade below and a Room above, the Lift shaft sits
	// inside a column of Facades one per stop row, the Shuttle's carriage
	// doors land on one long Facade, and the Staircase rises from a Facade
	// through its open right wall into the Facade beside it.
	//
	//   Layer 1  La | St St |   Li  | Sh Sh Sh Sh Sh Sh Sh | Sc Sc
	//   Layer 0  F  | U  U  | FFF   | F  F  F  F  F  F  F  | F  F  F
	//
	// Every Facade landing row is its own Facade, because only a Sector's own
	// bottom deck carries walkable floor.
	void authorFacadeLandingMenagerie(core::World& world,
		uint32_t* facadeMarker = nullptr, uint32_t* upperMarker = nullptr)
	{
		auto const belowLadder = world.addFacade(0, 0, 0, 1, 1);
		auto const aboveLadder = world.addRoom("Above the ladder", 0, 1, 0, 1, 2);
		if (facadeMarker != nullptr)
			world.addSectorMarker(belowLadder, 0, 0.5f, facadeMarker);
		if (upperMarker != nullptr)
			world.addSectorMarker(aboveLadder, 0, 0.5f, upperMarker);
		world.addLadder(1, 0, 0, { 2, false, true });

		world.addFacade(0, 0, 2, 2, 1);
		world.addRoom("Above the stairwell", 0, 1, 2, 2, 2);
		world.addStairwell(1, 0, 2, 2, CORE_SIDE_RIGHT);

		world.addFacade(0, 0, 5, 3, 1);
		world.addFacade(0, 1, 5, 3, 1);
		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 1 };
		world.addLift(1, 0, 6, liftOptions);

		world.addFacade(0, 0, 9, 7, 1);
		core::World::CreateShuttleOptions shuttleOptions{ 1, 3, { 0, 4 }, 0 };
		world.addShuttle(1, 0, 9, 7, shuttleOptions);

		world.addFacade(0, 0, 17, 2, 2);
		world.addFacade(0, 1, 19, 1, 1);
		world.addStaircase(1, 0, 17, 2, CORE_SIDE_RIGHT, 0.5f);

		world.finishBuild();
	}

	// The Vertex constructors pick a landing Vertex's VertexType from the
	// Sector it lands on. A Facade landing must be a VertexType::Location -
	// a Ladder- or Lift-typed Facade Vertex would route agents through the
	// transit's own traversal rules instead of the floor's.
	void aFacadeLandingVertexIsALocationVertex()
	{
		core::World world("Facade landing vertices", 22, 3);
		authorFacadeLandingMenagerie(world);

		auto const graph = world.getGraph();
		require(graph != nullptr, "The menagerie World has no Graph");

		uint32_t facadeVertices = 0;
		uint32_t transitVertices = 0;
		for (auto const& vertex : graph->getVertices())
		{
			auto const sector = vertex->getSector();
			require(sector != nullptr, "A Graph Vertex reported no Sector");
			if (sector->getType() == core::SectorType::Facade)
			{
				++facadeVertices;
				require(vertex->getType() == core::VertexType::Location,
					std::format("A Facade landing Vertex is VertexType {} (subtype {}) on Sector {}, not a Location Vertex",
						(int)vertex->getType(), (int)vertex->getSubType(), sector->getIndex()).c_str());
			}
			else if (vertex->getType() != core::VertexType::Location)
			{
				++transitVertices;
			}
		}

		require(facadeVertices > 0, "The Facade landings produced no Facade Vertices to check");
		require(transitVertices > 0,
			"The menagerie produced no transit Vertices, which makes the Facade Vertex check vacuous");
	}

	// A Facade landing is not only accepted, it is wired: the route from the
	// Facade floor up the Ladder and back down exists in both directions, so
	// the Facade really is in the pathing world at the transit's top of the
	// shaft and not merely tolerated at validation time.
	void agentsCanRouteFromAFacadeThroughATransit()
	{
		core::World world("Facade routing", 22, 3);
		uint32_t facadeMarker = 0;
		uint32_t upperMarker = 0;
		authorFacadeLandingMenagerie(world, &facadeMarker, &upperMarker);
		auto const agentId = world.createAgent("Transit route checker", 0, 0, 0.5f);
		auto const agent = world.lookupAgent(agentId).entity;
		require(agent != nullptr, "The Transit route checker was not created");

		require(hasPath(world, agent, facadeMarker, upperMarker),
			"No route from a Facade floor up the Ladder to the Room above it");
		require(hasPath(world, agent, upperMarker, facadeMarker),
			"No route back down the Ladder into the Facade");
	}

	// The widened landing rule has to survive the writer too: a map with
	// every Transit landing on a Facade saves, replays, and re-saves
	// unchanged, with each landing still attached to the Facade it was built
	// against.
	void transitLandingsOnAFacadeRoundTrip()
	{
		core::World world("Facade landing replay", 22, 3);
		authorFacadeLandingMenagerie(world);

		auto const yaml = serializeWorld(world);
		core::World loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);

		require(loaded.getNumSectors() == world.getNumSectors(),
			"The Facade-landing replay changed the Sector count");
		require(sectorSignature(loaded) == sectorSignature(world),
			("The Facade-landing replay moved a Sector\nexpected:\n" + sectorSignature(world)
				+ "actual:\n" + sectorSignature(loaded)).c_str());
		require(objectSignature(loaded) == objectSignature(world),
			("The Facade-landing replay moved a SectorObject\nexpected:\n"
				+ objectSignature(world) + "actual:\n" + objectSignature(loaded)).c_str());
		require(serializeWorld(loaded) == yaml,
			"Re-saving the replayed Facade-landing World changed its authored records");
	}
}

void runFacadeSmokeChecks()
{
	theTypeIsAKnownLocationKind();
	theColourDefaultsAndRoundTrips();
	placementValidationFollowsTheRoomRule();
	everyWallEndIsOpenOnEveryDeck();
	agentsMayBePlacedInAFacade();
	objectHostingParityWithARoom();
	roomSupportedObjectsPlaceInAFacade();
	hostedObjectsAreHitTestableThroughAFacade();
	bulkheadDoorsAreRefusedOnAFacade();
	doorAcceptsAFacadeOnEitherSide();
	wallCommandsRefuseTheFacadeButNotTowardIt();
	theFacadeRecordRoundTrips();
	aHandAuthoredFacadeRecordLoads();
	theFacadeRecordCarriesItsName();
	aHandAuthoredFacadeWithoutANameTakesTheDefaultName();
	saveLoadPreservesColourOpenEndsAndSectorIndexMapping();
	aFacadeNeverWritesWallRemovalRecords();
	facadeRecordsReplayInAnyOrder();
	aFacadeMapIsRejectedByPreFacadeCode();
	theFacadeTakesPartInTheGraph();
	layerDeletionHandlesFacadeRecords();
	restoreDropsAgentsOnNonTraversableFacadeCells();
	theDescriptionCarriesTheFacadeName();
	nanTopDeckHeightIsRejected();
	facadeBetweenTwoAlignedRoomsIsOneContinuousFloor();
	facadeBesideHigherFloorDoesNotMerge();
	wallRemovalAcceptsFacadeNeighboursBothWays();
	facadeBesideBackgroundHasNoPathingInteraction();
	everyTransitLandsOnAFacadeAsItDoesOnARoom();
	aFacadeLandingVertexIsALocationVertex();
	agentsCanRouteFromAFacadeThroughATransit();
	transitLandingsOnAFacadeRoundTrip();
}
