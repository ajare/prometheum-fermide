// Zero-sized Room and Corridor rejection checks, for ticket #64.
//
// A Room or Corridor must own at least one cell: both dimensions must be
// non-zero. Before this ticket a zero width or height passed validateBounds()
// by covering nothing, so the Sector landed in mSectors and the serialized
// record stream while owning no Layer cells - invisible, unhit-testable,
// unselectable, and shifting the indices of every later Sector. Background
// and Facade placement already refused this shape; these checks pin the same
// invariant down on addRoom(), both addCorridor() overloads, and the
// construction-record replay that a hand-edited save file goes through.

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#include "core/Building.h"
#include "core/Defines.h"
#include "core/Exceptions.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Runs an action expected to fail and returns the diagnostic it produced.
	// An empty string means the action was wrongly accepted.
	std::string refusalMessage(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (core::Exception const& error)
		{
			return error.getMessage();
		}
		catch (std::exception const& error)
		{
			return error.what();
		}
		return {};
	}

	// A refusal is only useful if it says which minimum was missed, not just
	// that something was out of bounds.
	void requireMinimumRefusal(std::string const& diagnostic, std::string const& what)
	{
		require(!diagnostic.empty(), ("A zero-sized " + what + " was accepted").c_str());
		require(diagnostic.find("at least one") != std::string::npos,
			("The zero-sized " + what + " refusal gave no minimum-size diagnostic: "
				+ diagnostic).c_str());
	}

	std::string loadFailure(std::string const& yaml)
	{
		core::Building target("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		target.deserialize(*reader, workData);
		return {};
	}

	// Minimal version 6 documents with one producer record; the header keeps
	// the record's zero dimension unique in the file so nothing else trips.
	std::string mapWith(std::string const& record)
	{
		return "version: 6\n"
			"name: Zero size replay\n"
			"cellsWide: 6\n"
			"decksHigh: 4\n"
			"layers: 2\n"
			"construction:\n"
			"  - " + record +
			"agents: []\n";
	}
}

namespace
{
	// addRoom() refuses both zero dimensions with a minimum-size diagnostic
	// and creates no Sector, so later Sector indices stay put.
	void theRoomApiRejectsZeroSizes()
	{
		core::Building building("Zero rooms", 6, 6);
		building.addRoom("Solid", 0, 0, 0, 2, 1);
		auto const baseline = building.getNumSectors();

		requireMinimumRefusal(refusalMessage([&] {
			building.addRoom("ghost width", 0, 1, 1, 0, 2);
		}), "Room");
		requireMinimumRefusal(refusalMessage([&] {
			building.addRoom("ghost height", 0, 1, 1, 2, 0);
		}), "Room");

		require(building.getNumSectors() == baseline,
			"A rejected addRoom() still created a Sector");

		// The ticket's exact reproduction shape.
		requireMinimumRefusal(refusalMessage([&] {
			core::Building repro("zero locations", 6, 6);
			repro.addRoom("ghost", 0, 1, 1, 0, 2);
		}), "Room");
	}

	// Both addCorridor() overloads refuse zero dimensions the same way.
	void theCorridorApiRejectsZeroSizes()
	{
		core::Building building("Zero corridors", 6, 6);
		building.addCorridor(0, 0, 2);
		auto const baseline = building.getNumSectors();

		requireMinimumRefusal(refusalMessage([&] {
			building.addCorridor(0, 1, 1, 0, 2);
		}), "Corridor");
		requireMinimumRefusal(refusalMessage([&] {
			building.addCorridor(0, 1, 1, 2, 0);
		}), "Corridor");
		requireMinimumRefusal(refusalMessage([&] {
			building.addCorridor(1u, 1u, 0u);
		}), "Corridor");
		// The Layer-less overload with an explicit zero decksHigh; the cast picks
		// that overload because the 4-argument forms are otherwise ambiguous.
		auto const layerless = static_cast<uint32_t(core::Building::*)(
			uint32_t, uint32_t, uint32_t, uint32_t)>(&core::Building::addCorridor);
		requireMinimumRefusal(refusalMessage([&] {
			(building.*layerless)(1u, 1u, 2u, 0u);
		}), "Corridor");

		require(building.getNumSectors() == baseline,
			"A rejected addCorridor() still created a Sector");

		// The ticket's exact reproduction shape.
		requireMinimumRefusal(refusalMessage([&] {
			core::Building repro("zero locations", 6, 6);
			repro.addCorridor(0, 3, 1, 2, 0);
		}), "Corridor");
	}

	// The one-cell minimum is a floor, not a ceiling: 1x1 stays legal.
	void theMinimumOneByOneLocationsAreAccepted()
	{
		core::Building building("Minimum locations", 6, 6);
		require(refusalMessage([&] {
			building.addRoom("Tiny", 0, 0, 0, 1, 1);
		}).empty(), "A 1x1 Room was refused");
		require(refusalMessage([&] {
			building.addCorridor(0, 1, 0, 1, 1);
		}).empty(), "A 1x1 Corridor was refused");
		require(building.getNumSectors() == 2,
			"The minimum 1x1 Locations did not both get created");
	}

	// A save file edited to a zero-sized Room record must refuse the open
	// rather than load an unreachable Sector.
	void replayRejectsAZeroSizedRoomRecord()
	{
		requireMinimumRefusal(refusalMessage([&] {
			loadFailure(mapWith("type: room\n"
				"    name: Ghost room\n"
				"    layer: 0\n"
				"    y: 1\n"
				"    x: 1\n"
				"    cellsWide: 0\n"
				"    decksHigh: 2\n"
				"    topDeckHeight: 0.9\n"));
		}), "replayed Room");
		requireMinimumRefusal(refusalMessage([&] {
			loadFailure(mapWith("type: room\n"
				"    name: Ghost room\n"
				"    layer: 0\n"
				"    y: 1\n"
				"    x: 1\n"
				"    cellsWide: 2\n"
				"    decksHigh: 0\n"
				"    topDeckHeight: 0.9\n"));
		}), "replayed Room");
	}

	// Same for a zero-sized Corridor record.
	void replayRejectsAZeroSizedCorridorRecord()
	{
		requireMinimumRefusal(refusalMessage([&] {
			loadFailure(mapWith("type: corridor\n"
				"    layer: 0\n"
				"    y: 1\n"
				"    x: 1\n"
				"    cellsWide: 0\n"
				"    decksHigh: 2\n"));
		}), "replayed Corridor");
		requireMinimumRefusal(refusalMessage([&] {
			loadFailure(mapWith("type: corridor\n"
				"    layer: 0\n"
				"    y: 1\n"
				"    x: 1\n"
				"    cellsWide: 2\n"
				"    decksHigh: 0\n"));
		}), "replayed Corridor");
	}

	// Positive control: the same documents with honest sizes still load.
	void honestRecordsStillLoad()
	{
		core::Building loaded("placeholder", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(mapWith("type: room\n"
			"    name: Honest room\n"
			"    layer: 0\n"
			"    y: 1\n"
			"    x: 1\n"
			"    cellsWide: 2\n"
			"    decksHigh: 2\n"
			"    topDeckHeight: 0.9\n"
			"  - type: corridor\n"
			"    layer: 0\n"
			"    y: 3\n"
			"    x: 1\n"
			"    cellsWide: 2\n"
			"    decksHigh: 1\n"));
		reader->deserialize();
		require(loaded.deserialize(*reader, workData),
			"A valid Room and Corridor document did not load");
		require(loaded.getNumSectors() == 2,
			"The valid document did not produce both Locations");
	}
}

void runZeroSizeLocationSmokeChecks()
{
	theRoomApiRejectsZeroSizes();
	theCorridorApiRejectsZeroSizes();
	theMinimumOneByOneLocationsAreAccepted();
	replayRejectsAZeroSizedRoomRecord();
	replayRejectsAZeroSizedCorridorRecord();
	honestRecordsStillLoad();
}
