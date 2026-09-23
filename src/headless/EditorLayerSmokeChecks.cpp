// Editor-facing Layer queries, for ticket #23.
//
// A bug hunt on the multi-Layer branch found the editor assuming the old two-Layer
// front pair in several places: the Lift paint preview read its landing cells from
// Layer 0, the Corridor tool was disabled on one arbitrary Layer, and three loops
// stopped after two Layers so deep Rooms, deep Windows, and deep Agents vanished
// from name dedup, the clipboard, and the Agents view.
//
// The editor itself is not reachable headlessly, so these checks pin the core
// contracts each fixed call site now depends on: landings always come from the
// Layer directly in front of a Transit's own Layer, a definition is found on the
// Layer that authored it, every Layer enumerates its Sectors and their Agents, and
// a Corridor may be authored on any Layer at all.

#include <cstdint>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Agent.h"
#include "core/World.h"
#include "core/Lift.h"
#include "core/LiftTransit.h"
#include "core/Location.h"
#include "core/Sector.h"
#include "core/Window.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::vector<uint32_t> usableStopOffsets(std::vector<core::World::LiftLandingRow> const& rows)
	{
		std::vector<uint32_t> offsets;
		for (auto const& row : rows)
			if (row.usableForStop()) offsets.push_back(row.offset);
		return offsets;
	}

	// Mirrors the editor's Lift paint preview: a corridor stop row on the Layer in
	// front of the shaft, and no stop at all where the shaft leaves no room for the
	// stop's call control.
	struct LiftPreview
	{
		uint32_t stops{ 0 };
		bool callButtonBlocked{ false };
	};

	LiftPreview previewLift(core::World const& world, uint32_t shaftLayer, uint32_t y,
		uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
	{
		LiftPreview preview;
		for (auto const& row : world.getLiftLandingRows(shaftLayer, y, x, cellsWide, levelsHigh))
		{
			if (!row.location || !row.fullyOverlapping || row.obstructed
				|| !row.location->isCorridor()) continue;
			if (!row.callButtonSpace)
			{
				preview.callButtonBlocked = true;
				break;
			}
			++preview.stops;
		}
		return preview;
	}

	// Mirrors the editor's Room-name dedup: every Location name on every Layer.
	std::set<std::string> authoredNames(core::World const& world)
	{
		std::set<std::string> names;
		for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
			for (auto const& sector : world.getSectors(layer))
				names.insert(sector->getName());
		return names;
	}

	std::string nextRoomName(core::World const& world)
	{
		auto const names = authoredNames(world);
		for (uint64_t number = 1;; ++number)
		{
			auto const name = "Room " + std::to_string(number);
			if (!names.contains(name)) return name;
		}
	}

	// Authors a four-Layer World whose Lift landings sit on Layer 1: two
	// corridor floors with room beside them for a call control.  Layer 0 is
	// deliberately carpeted with corridors so that reading the landings there -
	// the bug this ticket fixes - answers differently from reading the Layer
	// directly in front of the shaft on Layer 2.
	void authorLiftLandings(core::World& world)
	{
		while (world.getLayerCount() < 4) world.addLayer();
		for (uint32_t row = 0; row < world.getLevelsHigh(); ++row)
			world.addCorridor(0, row, 0, 40, 1);
		world.addCorridor(1, 1, 8, 12, 1);
		world.addCorridor(1, 3, 8, 12, 1);
	}

	uint32_t cellSector(core::World const& world, uint32_t layer, uint32_t x, uint32_t y)
	{
		return world.getLayer(layer)->getCellDefinition(x, y).sectorIndex;
	}
}

void liftLandingsComeFromTheLayerInFront()
{
	// Regression for ticket #23: the Lift preview read landing cells from Layer 0,
	// so a shaft drafted on Layer 2 was validated against floors a whole Layer too
	// far forward.  getLiftLandingRows() is the derivation the preview and
	// addLift() now share, and it must follow the Layer directly in front.
	core::World world("Lift landing layers", 40, 6);
	authorLiftLandings(world);

	auto const rows = world.getLiftLandingRows(2, 0, 12, 2, 5);
	require(rows.size() == 5, "A Lift shaft did not report one landing row per level");
	require(usableStopOffsets(rows) == std::vector<uint32_t>({ 1, 3 }),
		"A Lift on Layer 2 did not derive its stops from the Layer directly in front");
	for (auto const& row : rows)
	{
		if (row.offset != 1 && row.offset != 3)
		{
			require(!row.location, "A Lift landing row reported a Location over empty space");
			continue;
		}
		require(row.location && row.location->isCorridor()
			&& row.location->getLayerIndex() == 1,
			"A Lift stop row landed on something other than the Corridor one Layer in front");
	}

	// Layer 0 would have answered differently: every one of its rows is a usable
	// corridor landing, which is exactly the mis-validation the preview performed.
	require(usableStopOffsets(world.getLiftLandingRows(1, 0, 12, 2, 5)).size() == 5,
		"The Layer directly in front of Layer 1 was not the carpeted Layer 0");
	require(previewLift(world, 2, 0, 12, 2, 5).stops == 2,
		"The Lift preview did not count the two corridor floors of the landing Layer");

	// The same shaft authored through the shared derivation gets the same stops.
	auto const created = world.addLift(2, 0, 12, 2, 5);
	auto const transit = std::dynamic_pointer_cast<const core::LiftTransit>(created.lift.sector);
	require(transit && transit->getLayerIndex() == 2, "The Lift was not authored on Layer 2");
	require(transit->getLift() && transit->getLift()->getNumStops() == 2,
		"The authored Lift did not carry the two derived stops");

	// The front-most Layer has nothing in front of it, so it can land nothing.
	require(world.getLiftLandingRows(0, 0, 12, 2, 5).empty(),
		"The front-most Layer reported Lift landings");
	require(previewLift(world, 0, 0, 12, 2, 5).stops == 0,
		"A Lift preview on the front-most Layer found stops");
}

void liftLandingsReportObstructionAndCallButtonSpace()
{
	// The preview distinguishes a row it cannot serve because something stands in
	// it from one it cannot serve because the shaft fills the Location width and
	// there would be nowhere to stand to call the Lift.
	core::World world("Lift landing obstructions", 40, 6);
	authorLiftLandings(world);

	// A Marker on the Layer 1 corridor inside the shaft column blocks that row.
	auto const landingCorridor = world.getSector(cellSector(world, 1, 12, 1));
	require(landingCorridor && landingCorridor->getName() == "Corridor",
		"No Corridor was found under the blocked Lift landing column");
	world.addSectorMarker(landingCorridor->getIndex(), 0, 4.0f);

	auto const rows = world.getLiftLandingRows(2, 0, 12, 2, 5);
	require(rows[1].fullyOverlapping && rows[1].obstructed && !rows[1].usableForStop(),
		"A Marker on the landing Layer did not block its Lift stop row");
	require(rows[3].usableForStop(), "A Marker blocked a Lift stop row it does not touch");
	require(previewLift(world, 2, 0, 12, 2, 5).stops == 1,
		"The Lift preview counted a landing row blocked by a Marker");

	// A shaft spanning a Corridor's whole width leaves no space for its call control.
	world.addCorridor(1, 1, 24, 2, 1);
	world.addCorridor(1, 3, 24, 2, 1);
	auto const spanning = world.getLiftLandingRows(2, 0, 24, 2, 5);
	require(spanning[1].fullyOverlapping && !spanning[1].callButtonSpace
		&& !spanning[1].usableForStop(),
		"A shaft spanning a Corridor left room for a call control");
	require(previewLift(world, 2, 0, 24, 2, 5).callButtonBlocked,
		"The Lift preview accepted a shaft with no corridor space for its call button");
}

void windowDefinitionsAreFoundOnTheWindowLayer()
{
	// Regression for ticket #23: copying a selected Window searched its authored
	// definition on Layers 0 and 1 only, so a Window on Layer 2 reported that it
	// had no definition at all.  A Window is authored on the front Layer of the
	// pair it crosses, and that is the Layer its definition is recorded against.
	core::World world("Window copy layers", 12, 3);
	while (world.getLayerCount() < 4) world.addLayer();
	world.addCorridor(0, 0, 0, 12, 1);
	world.addRoom("Store", 1, 0, 0, 12, 1);
	world.addRoom("Depot", 2, 0, 0, 12, 1);
	world.addRoom("Back Room", 3, 0, 0, 12, 1);

	auto const created = world.addSectorWindow(2, 0, 5, 2, 1, { true });
	require(created.object != nullptr, "No Window was authored on Layer 2");
	require(created.object->getFrontLayer() == 2 && created.object->getBackLayer() == 3,
		"A Window authored on Layer 2 does not report Layer 2 as its front Layer");

	core::World::CreateWindowOptions options;
	require(world.getSectorWindowOptions(2, 0, 5, 2, 1, options),
		"A Window on Layer 2 has no definition on its own Layer");
	require(options.traversable, "The Window definition read back its traversal flag wrongly");
	require(!world.getSectorWindowOptions(0, 0, 5, 2, 1, options)
		&& !world.getSectorWindowOptions(1, 0, 5, 2, 1, options),
		"A Layer other than the Window's own reported its definition");
}

void everyLayerEnumeratesItsSectorsAndAgents()
{
	// Regression for ticket #23: the Agents view and the Room-name dedup looped
	// over two Layers, so Agents and Room names on Layer 2 or deeper were invisible.
	core::World world("Deep enumeration", 16, 2);
	while (world.getLayerCount() < 4) world.addLayer();
	auto const corridor = world.getSector(world.addCorridor(0, 0, 0, 16, 1));
	world.addRoom("Room 1", 1, 0, 0, 16, 1);
	world.addRoom("Depot", 2, 0, 0, 16, 1);
	world.addRoom("Room 2", 3, 0, 0, 16, 1);

	for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
	{
		auto const sectors = world.getSectors(layer);
		require(!sectors.empty(), "A Layer of the World enumerated no Sectors");
		bool sawDepot = false;
		for (auto const& sector : sectors)
		{
			require(sector->getLayerIndex() == layer,
				"A Sector enumerated from a Layer reported a different Layer");
			sawDepot = sawDepot || (layer == 2 && sector->getName() == "Depot");
		}
		require(layer != 2 || sawDepot, "The Depot was not enumerated from Layer 2");
	}

	world.createAgent("Front agent", corridor->getIndex(), 0, 0.5f);
	auto const depot = std::dynamic_pointer_cast<const core::Location>(
		world.getSector(cellSector(world, 2, 0, 0)));
	require(depot && depot->getName() == "Depot", "The Depot could not be resolved on Layer 2");
	world.createAgent("Deep agent", depot->getIndex(), 0, 0.5f);

	std::set<std::string> reachableAgents;
	for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
		for (auto const& sector : world.getSectors(layer))
			for (auto const& agent : sector->getAgents())
				reachableAgents.insert(agent->getName());

	require(reachableAgents.contains("Deep agent"),
		"An Agent on Layer 2 was not enumerable from the Layer it stands on");

	// Name dedup must see deep names too: "Room 2" exists on Layer 3, so the next
	// name the editor offers has to skip past it.
	auto const names = authoredNames(world);
	require(names.contains("Room 2"), "A Room name authored on Layer 3 was not collected");
	require(nextRoomName(world) == "Room 3",
		"Room-name dedup offered a name already used on a deep Layer");
}

void corridorsAreAuthorableOnEveryLayer()
{
	// Regression for ticket #23: the Corridor tool was disabled exactly on Layer 1,
	// the old CORE_LAYER_BACK.  A Corridor is a Location and a Location may sit on
	// any Layer, so no Layer may refuse one.
	core::World world("Corridor layers", 24, 3);
	while (world.getLayerCount() < 4) world.addLayer();
	for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
	{
		auto const index = world.addCorridor(layer, 0, layer * 5, 4, 1);
		auto const sector = std::dynamic_pointer_cast<const core::Location>(world.getSector(index));
		require(sector && sector->isCorridor(),
			"A Corridor authored on a deep Layer is not a corridor Location");
		require(sector->getLayerIndex() == layer,
			"A Corridor was not authored on the requested Layer");
	}
}

void runEditorLayerSmokeChecks()
{
	liftLandingsComeFromTheLayerInFront();
	liftLandingsReportObstructionAndCallButtonSpace();
	windowDefinitionsAreFoundOnTheWindowLayer();
	everyLayerEnumeratesItsSectorsAndAgents();
	corridorsAreAuthorableOnEveryLayer();
}
