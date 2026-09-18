// Facade checks, for ticket #43.
//
// A Facade is an occupiable Location whose perimeter walls are all open by
// construction: it hosts objects and agents exactly as a Room does, owns
// walkable floor, and takes part in the Graph, but every wall end on every
// deck is open intrinsically and it is rendered as a solid opaque colour
// (ADR 0003). These checks cover creation and placement validation, the
// open-end invariant, agent placement, object-placement parity with a Room,
// Bulkhead Door refusal, wall-command refusal, and persistence of the
// ConstructionType::Facade record and its packed colour.

#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>

#include "core/Background.h"
#include "core/Building.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Facade.h"
#include "core/Graph.h"
#include "core/Location.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SectorType.h"
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

	std::shared_ptr<const core::Facade> facadeIn(core::Building const& building, uint32_t sectorIndex)
	{
		auto sector = building.getSector(sectorIndex);
		require(sector != nullptr, "Building reported a null Sector");
		require(sector->getType() == core::SectorType::Facade,
			("Sector " + std::to_string(sectorIndex) + " is not a Facade").c_str());
		auto facade = std::dynamic_pointer_cast<const core::Facade>(sector);
		require(facade != nullptr, "A Facade Sector is not a core::Facade");
		return facade;
	}

	std::string serializeBuilding(core::Building const& building)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	void loadInto(core::Building& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(target.deserialize(*reader, workData), "Building YAML did not load");
	}

	// A stable description of every Sector, Facade colour included, used to
	// compare a Building against its own replay.
	std::string sectorSignature(core::Building const& building)
	{
		std::string signature;
		for (uint32_t index = 0; index < building.getNumSectors(); ++index)
		{
			auto const sector = building.getSector(index);
			require(sector != nullptr, "Building reported a null Sector while signing");
			signature += std::format("{}:{}@{},{},{}x{}", index,
				core::getSectorTypeString(sector->getType()), sector->getLayerIndex(),
				sector->getCellX(), sector->getCellY(), sector->getCellsWide(), sector->getDecksHigh());
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
		require(facade.getDescription().rfind("Facade", 0) == 0,
			("A Facade's description does not lead with its kind: "
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
		core::Building building("Placement", 12, 3);
		building.addRoom("Occupier", 1, 0, 0, 3, 1);
		building.finishBuild();
		building.pauseSimulation();

		std::string diagnostic;
		require(building.canAddFacade(0, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was refused on a free front Layer cell block");
		require(building.canAddFacade(1, 0, 3, 2, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was refused beside an existing Room on a free block");
		require(!building.canAddFacade(1, 0, 0, 1, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was accepted over a cell an existing Sector owns");
		require(!building.canAddFacade(0, 0, 0, 0, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A zero-wide Facade was accepted");
		require(!building.canAddFacade(0, 0, 0, 1, 0, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A zero-high Facade was accepted");
		require(!building.canAddFacade(0, 0, 0, 1, 1, CORE_ROOM_MAX_HEIGHT + 0.1f, &diagnostic),
			"A Facade was accepted with a top deck height above CORE_ROOM_MAX_HEIGHT");
		require(!building.canAddFacade(0, 0, 0, 1, 1, CORE_ROOM_MIN_HEIGHT - 0.1f, &diagnostic),
			"A Facade was accepted with a top deck height below CORE_ROOM_MIN_HEIGHT");
		require(!building.canAddFacade(0, 0, 11, 2, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was accepted across the Building bounds");
		require(!building.canAddFacade(9, 0, 0, 1, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
			"A Facade was accepted on a nonexistent Layer");

		building.addFacade(0, 1, 4, 4, 2, CORE_ROOM_MAX_HEIGHT, { 200, 30, 99 });
		building.finishBuild();
		auto const facade = facadeIn(building, 1);
		require(facade->getLayerIndex() == 0 && facade->getCellY() == 1 && facade->getCellX() == 4
			&& facade->getCellsWide() == 4 && facade->getDecksHigh() == 2,
			"The Facade footprint is not where it was placed");
		require(facade->getColour() == core::BackgroundColour{ 200, 30, 99 },
			"The Facade colour was not carried to the live Sector");

		// A Facade owns walkable floor exactly as a Location does: ground on
		// the bottom deck.
		auto const layer = std::as_const(building).getLayer(0);
		require(layer->getCellDefinition(4, 1).floorType == core::CellFloorType::Ground
			&& layer->getCellDefinition(4, 1).isTraversableOnFoot(),
			"A Facade's ground deck is not walkable");
	}

	// The open-end invariant on the live Building: every deck, both sides.
	void everyWallEndIsOpenOnEveryDeck()
	{
		core::Building building("Open perimeter", 12, 3);
		auto const index = building.addFacade(0, 0, 0, 5, 3);
		building.finishBuild();

		auto const facade = facadeIn(building, index);
		require(facade->getDecksHigh() == 3, "The Facade did not keep its deck count");
		everyEndIsOpen(*facade);
	}

	// An Agent belongs to a Facade exactly as it belongs to a Room.
	void agentsMayBePlacedInAFacade()
	{
		core::Building building("Host", 12, 3);
		auto const facadeIndex = building.addFacade(0, 0, 0, 4, 1);
		building.finishBuild();

		auto const agentId = building.createAgent("Frontage dweller", facadeIndex, 0, 1.5f);
		auto const agent = building.lookupAgent(agentId).entity;
		require(agent != nullptr, "The Facade Agent was not created");
		require(agent->getSector() != nullptr
			&& agent->getSector()->getIndex() == facadeIndex,
			"The Agent does not belong to the Facade");
		require(facadeIn(building, facadeIndex)->getAgents().size() == 1,
			"The Facade does not report its Agent");
	}

	// Object-placement parity: a Facade hosts every object type a Room hosts,
	// with the single exception of the Bulkhead Door, which needs wall ends a
	// Facade does not have.
	void objectHostingParityWithARoom()
	{
		core::Building building("Parity", 12, 3);
		auto const roomIndex = building.addRoom("Comparator", 0, 0, 0, 4, 2);
		auto const facadeIndex = building.addFacade(0, 0, 4, 4, 2);
		building.finishBuild();

		auto const room = building.getSector(roomIndex);
		auto const facade = building.getSector(facadeIndex);
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
		core::Building building("Host of objects", 12, 3);
		auto const facadeIndex = building.addFacade(0, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT,
			{ 11, 222, 33 });
		building.finishBuild();
		building.pauseSimulation();

		std::string diagnostic;
		require(building.canAddSectorMarker(facadeIndex, 0, 1.5f, &diagnostic),
			("A Marker was refused in a Facade: " + diagnostic).c_str());
		auto const marker = building.addSectorMarker(facadeIndex, 0, 1.5f);
		require(marker.sector != nullptr, "The Facade Marker has no Sector");

		require(building.canAddSectorWalkway(facadeIndex, 1, 0, &diagnostic),
			("A Walkway was refused in a Facade: " + diagnostic).c_str());
		building.addSectorWalkway(facadeIndex, 1, 0);

		// A light switch is an InteractionPoint control; a Room hosts it, so a
		// Facade must take one too. There is no canAdd form; the add must not throw.
		building.addSectorLightSwitch(facadeIndex, 1);

		require(building.canAddRoomLadder(facadeIndex, 0, 0, nullptr, &diagnostic),
			("A Room Ladder was refused under the Facade Walkway: " + diagnostic).c_str());
		building.addRoomLadder(facadeIndex, 0, 0);

		// The Facade still hosts nothing that a Room does not, and its perimeter
		// is still open after all that object authoring.
		everyEndIsOpen(*facadeIn(building, facadeIndex));
	}

	// A Bulkhead Door is set into a pair of wall ends. A Facade has none, so
	// every Bulkhead Door placement that touches one is refused - while the
	// same placement between two Rooms still works, so the refusal is
	// Facade-specific and not a broken check.
	void bulkheadDoorsAreRefusedOnAFacade()
	{
		core::Building building("No bulkheads here", 16, 3);
		auto const roomA = building.addRoom("Room A", 0, 0, 0, 4, 1);
		building.addFacade(0, 0, 4, 4, 1);
		auto const roomB = building.addRoom("Room B", 0, 0, 8, 4, 1);
		building.finishBuild();
		(void)roomA; (void)roomB;

		std::string diagnostic;
		// Facade on the right of the boundary, Room A on the left.
		require(!building.canAddSectorBulkheadDoor(0, 0, 4, CORE_SIDE_LEFT,
				core::Building::CreateBulkheadDoorOptions{}, &diagnostic),
			"A Bulkhead Door was accepted with a Facade on its right");
		require(diagnostic.find("Facade") != std::string::npos,
			("The Bulkhead Door refusal does not name the Facade: " + diagnostic).c_str());
		require(throws([&] { building.addSectorBulkheadDoor(0, 0, 4, CORE_SIDE_LEFT); }),
			"addSectorBulkheadDoor did not throw against a Facade boundary");

		// Facade on the left of the boundary, Room B on the right.
		require(!building.canAddSectorBulkheadDoor(0, 0, 7, CORE_SIDE_RIGHT,
				core::Building::CreateBulkheadDoorOptions{}, &diagnostic),
			"A Bulkhead Door was accepted with a Facade on its left");
		require(throws([&] { building.addSectorBulkheadDoor(0, 0, 7, CORE_SIDE_RIGHT); }),
			"addSectorBulkheadDoor did not throw against the Facade's other side");

		// The same check still admits a Room-to-Room boundary away from the
		// Facade: build a second pair and compare.
		core::Building control("Bulkhead control", 16, 3);
		control.addRoom("Left", 0, 0, 0, 4, 1);
		control.addRoom("Right", 0, 0, 4, 4, 1);
		control.finishBuild();
		require(control.canAddSectorBulkheadDoor(0, 0, 4, CORE_SIDE_LEFT,
				core::Building::CreateBulkheadDoorOptions{}, &diagnostic),
			("The Bulkhead Door check broke for plain Rooms too: " + diagnostic).c_str());
	}

	// Wall edits refuse the Facade itself - its perimeter is not editable -
	// while a neighbouring Room may still open its own wall into the Facade's
	// already-open side, so neighbours can merge inward (ADR 0003).
	void wallCommandsRefuseTheFacadeButNotTowardIt()
	{
		core::Building building("Wall rules", 16, 3);
		auto const roomIndex = building.addRoom("Walled", 0, 0, 0, 4, 1);
		auto const facadeIndex = building.addFacade(0, 0, 4, 4, 1);
		building.finishBuild();
		building.pauseSimulation();

		std::string diagnostic;
		// The Facade has no walls to remove and none to add.
		require(!building.canRemoveLocationWall(facadeIndex, 0, CORE_SIDE_LEFT, &diagnostic),
			"A wall removal was accepted on a Facade");
		require(!building.canAddLocationWall(facadeIndex, 0, CORE_SIDE_LEFT, &diagnostic),
			"A wall addition was accepted on a Facade");

		// Opening the Room's wall into the Facade: the Facade side is already
		// open, so only the Room's half changes.
		require(building.canRemoveLocationWall(roomIndex, 0, CORE_SIDE_RIGHT, &diagnostic),
			("A Room could not open its own wall toward a Facade: " + diagnostic).c_str());
		building.removeLocationWall(roomIndex, 0, CORE_SIDE_RIGHT);
		require(building.getSector(roomIndex)->getEndType(0, CORE_SIDE_RIGHT)
			== core::SectorEndType::None,
			"The Room's wall toward the Facade did not open");
		everyEndIsOpen(*facadeIn(building, facadeIndex));

		// And it cannot be re-walled: the Facade refuses the other half.
		require(!building.canAddLocationWall(roomIndex, 0, CORE_SIDE_RIGHT, &diagnostic),
			"A wall was restored against a Facade");
	}

	// The Facade record round-trips: type name, footprint, top deck height,
	// and packed colour; and replay is stable across saves.
	void theFacadeRecordRoundTrips()
	{
		core::Building building("Record keeper", 12, 3);
		building.addRoom("Room 0", 0, 0, 0, 4, 1);
		building.addFacade(1, 1, 4, 4, 2, CORE_ROOM_MAX_HEIGHT, { 176, 160, 128 });
		building.addFacade(1, 0, 8, 2, 1, CORE_ROOM_MAX_HEIGHT, { 12, 240, 6 });
		building.finishBuild();

		auto const yaml = serializeBuilding(building);
		require(yaml.find("type: facade") != std::string::npos,
			"The Facade record was not written as 'facade'");
		require(yaml.find(std::format("colour: {}",
				core::packBackgroundColour(core::BackgroundColour{ 12, 240, 6 }))) != std::string::npos,
			"The Facade colour was not written packed");

		core::Building loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getNumSectors() == 3, "The Facade records did not replay into Sectors");
		require(sectorSignature(loaded) == sectorSignature(building),
			("Facade replay did not reproduce the Building\nexpected:\n"
				+ sectorSignature(building) + "actual:\n" + sectorSignature(loaded)).c_str());

		// Re-saving a replayed Building changes nothing: the records are stable.
		require(serializeBuilding(loaded) == yaml,
			"Re-saving a replayed Facade Building changed its authored records");
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

		core::Building loaded("placeholder", 1, 1);
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

	// The Facade takes part in the Graph: with the shared wall opened, a
	// Marker inside the Facade is reachable from the neighbouring Room's
	// side of the row, and with the Room's wall left standing there is no
	// route at all - the opened boundary is what connects them.
	void theFacadeTakesPartInTheGraph()
	{
		core::Building open("Merged", 16, 3);
		auto const roomIndex = open.addRoom("Neighbour", 0, 0, 0, 4, 1);
		auto const facadeIndex = open.addFacade(0, 0, 4, 4, 1);
		uint32_t roomMarkerIdentifier = 0;
		uint32_t facadeMarkerIdentifier = 0;
		open.addSectorMarker(roomIndex, 0, 1.0f, &roomMarkerIdentifier);
		open.addSectorMarker(facadeIndex, 0, 1.5f, &facadeMarkerIdentifier);
		open.removeLocationWall(roomIndex, 0, CORE_SIDE_RIGHT);
		open.finishBuild();

		auto const source = open.getGraph()->getVertexByIdentifier(roomMarkerIdentifier);
		auto const destination = open.getGraph()->getVertexByIdentifier(facadeMarkerIdentifier);
		require(source != nullptr && destination != nullptr,
			"A Marker in the Room/Facade pair has no Graph vertex");
		auto path = open.getGraph()->calculatePath(nullptr, source, destination);
		require(path && !path->nodes.empty(),
			"No path exists from a Room into a Facade through the opened boundary");

		core::Building sealed("Sealed", 16, 3);
		auto const sealedRoom = sealed.addRoom("Neighbour", 0, 0, 0, 4, 1);
		auto const sealedFacade = sealed.addFacade(0, 0, 4, 4, 1);
		uint32_t sealedRoomMarker = 0;
		uint32_t sealedFacadeMarker = 0;
		sealed.addSectorMarker(sealedRoom, 0, 1.0f, &sealedRoomMarker);
		sealed.addSectorMarker(sealedFacade, 0, 1.5f, &sealedFacadeMarker);
		sealed.finishBuild();

		auto const blocked = sealed.getGraph()->calculatePath(nullptr,
			sealed.getGraph()->getVertexByIdentifier(sealedRoomMarker),
			sealed.getGraph()->getVertexByIdentifier(sealedFacadeMarker));
		require(!blocked || blocked->nodes.empty(),
			"A walled boundary still routed into the Facade");
	}

	// A Layer deletion drops the Facades on it like any other Location and
	// keeps the rest of the Building replayable.
	void layerDeletionHandlesFacadeRecords()
	{
		core::Building building("Compaction", 12, 3);
		building.addRoom("Front", 0, 0, 0, 4, 1);
		building.addFacade(1, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT, { 99, 99, 99 });
		building.addLayer();
		building.finishBuild();

		auto const plan = building.planDeleteLayer(1);
		require(plan.valid, ("Deleting the Facade's Layer was refused: " + plan.diagnostic).c_str());
		require(plan.locationsRemoved == 1,
			"The deleted Facade was not counted as a removed Location");
		require(building.applyDeleteLayer(plan), "The Facade Layer deletion did not apply");
		require(building.getNumSectors() == 1,
			"The Facade Sector survived the deletion of its Layer");
		require(building.getSector(0)->getType() == core::SectorType::Location,
			"The surviving Sector is not the front Room");
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
	bulkheadDoorsAreRefusedOnAFacade();
	wallCommandsRefuseTheFacadeButNotTowardIt();
	theFacadeRecordRoundTrips();
	aHandAuthoredFacadeRecordLoads();
	theFacadeTakesPartInTheGraph();
	layerDeletionHandlesFacadeRecords();
}
