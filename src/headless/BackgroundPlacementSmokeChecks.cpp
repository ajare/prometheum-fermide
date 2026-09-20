// Background placement and persistence checks, for ticket #30.
//
// A Background is a non-occupiable Sector that exists to be seen through
// apertures from the Layer in front. Ticket #30 gives Building a creation path:
// canAddBackground()/addBackground(), the ConstructionType::Background record,
// and the version 5 writer. These checks cover the placement accept/reject
// matrix, the version 5 round-trip of the packed colour, and record replay.

#include <cstdint>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>

#include "core/Background.h"
#include "core/Building.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"
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

	// A Building with three Layers: front-most, middle, and back-most.
	void addThirdLayer(core::Building& building)
	{
		building.addLayer();
		require(building.getLayerCount() == 3, "Test Building did not get three Layers");
	}

	std::shared_ptr<const core::Background> backgroundIn(core::Building const& building,
		uint32_t sectorIndex)
	{
		auto sector = building.getSector(sectorIndex);
		require(sector != nullptr, "Building reported a null Sector");
		require(sector->getType() == core::SectorType::Background,
			("Sector " + std::to_string(sectorIndex) + " is not a Background").c_str());
		auto background = std::dynamic_pointer_cast<const core::Background>(sector);
		require(background != nullptr, "A Background Sector is not a core::Background");
		return background;
	}

	// The cell footprint a Background claims on its own Layer: occupied by that
	// Sector, with no floor and no SectorObject on every cell.
	void footprintIsStamped(core::Building const& building, uint32_t layerIndex,
		uint32_t sectorIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh)
	{
		auto const layer = building.getLayer(layerIndex);
		for (uint32_t iy = y; iy < y + decksHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cell = layer->getCellDefinition(ix, iy);
				require(cell.sectorIndex == sectorIndex,
					std::format("Background does not own cell {},{} on Layer {}", ix, iy, layerIndex).c_str());
				require(cell.floorType == core::CellFloorType::None,
					std::format("Background cell {},{} carries a walkable floor", ix, iy).c_str());
				require(!cell.isTraversableOnFoot(),
					std::format("Background cell {},{} is traversable on foot", ix, iy).c_str());
				require(!cell.hasObject(),
					std::format("Background cell {},{} carries a SectorObject", ix, iy).c_str());
			}
		}
	}

	// A stable description of every Sector the Building holds, used to compare a
	// Building against its own replay.
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
			if (sector->getType() == core::SectorType::Background)
			{
				auto const background = std::dynamic_pointer_cast<const core::Background>(sector);
				require(background != nullptr, "Signed Background is not a core::Background");
				signature += std::format(" colour={:06X}", core::packBackgroundColour(background->getColour()));
			}
			signature += ";\n";
		}
		return signature;
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

	// The record type is named on the way out and recognised on the way in.
	void theBackgroundRecordTypeRoundTripsByName()
	{
		core::Building building("Named record", 12, 3);
		building.addRoom("Room", 0, 0, 0, 4, 1);
		building.addBackground(1, 1, 6, 4, 2, { 200, 30, 99 });
		building.finishBuild();

		auto const yaml = serializeBuilding(building);
		require(yaml.find("type: background") != std::string::npos,
			"A Background record was not written as 'background'");

		core::Building loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getNumSectors() == 2, "The Background record did not replay into a Sector");
		require(backgroundIn(loaded, 1)->getColour() == core::BackgroundColour{ 200, 30, 99 },
			"The Background record did not replay its colour");
	}

	// Any Layer is legal, front-most and back-most included. A "back layers only"
	// rule would re-introduce the Fore/Back special-casing ADR 0002 removed.
	void everyLayerAcceptsABackground()
	{
		core::Building building("Every layer", 12, 3);
		addThirdLayer(building);

		for (uint32_t layer = 0; layer < building.getLayerCount(); ++layer)
		{
			std::string diagnostic;
			require(building.canAddBackground(layer, 0, layer * 2, 2, 1, &diagnostic),
				std::format("Layer {} refused a Background: {}", layer, diagnostic).c_str());
		}

		building.addBackground(0, 0, 0, 2, 1);
		building.addBackground(1, 0, 2, 2, 1);
		building.addBackground(2, 0, 4, 2, 1);
		building.finishBuild();

		require(building.getNumSectors() == 3, "Backgrounds did not each become a Sector");
		for (uint32_t layer = 0; layer < 3; ++layer)
		{
			auto const sector = building.getSector(layer);
			require(sector->getType() == core::SectorType::Background,
				std::format("Sector for Layer {} is not a Background", layer).c_str());
			require(sector->getLayerIndex() == layer,
				std::format("Background did not keep Layer {}", layer).c_str());
		}
	}

	// Occupancy is read on the Background's own Layer, the same rule a Location
	// plays by: taken cells on that Layer refuse it, free cells anywhere else do not.
	void onlyTheSameLayersOccupiedCellsRefuseABackground()
	{
		core::Building building("Per-layer occupancy", 12, 3);
		addThirdLayer(building);
		building.addRoom("Room 1", 1, 0, 0, 3, 1);

		std::string diagnostic;
		require(!building.canAddBackground(1, 0, 2, 1, 1, &diagnostic),
			"A Background was accepted over a cell another Sector owns on that Layer");
		require(!diagnostic.empty(), "A rejected Background gave no diagnostic");

		// The same footprint on a Layer with nothing on it is free.
		require(building.canAddBackground(0, 0, 2, 1, 1, &diagnostic),
			std::format("A free Layer refused a Background: {}", diagnostic).c_str());
		require(building.canAddBackground(2, 0, 0, 3, 1, &diagnostic),
			std::format("A free Layer refused a Background: {}", diagnostic).c_str());

		// Just clear of the Room on the Room's own Layer.
		require(building.canAddBackground(1, 0, 3, 2, 1, &diagnostic),
			std::format("A free run beside a Location refused a Background: {}", diagnostic).c_str());
	}

	// A Background takes space the way a Location does: nothing else may be built
	// over it on that Layer.
	void aBackgroundTakesUpTheSpaceItWasGiven()
	{
		core::Building building("Taken space", 12, 3);
		addThirdLayer(building);
		building.addRoom("Room 0", 0, 0, 0, 4, 1);
		auto const taken = building.addBackground(1, 0, 0, 4, 1);

		require(throws([&] { building.addRoom("Over the top", 1, 0, 1, 2, 1); }),
			"A Location was accepted over a Background on the same Layer");
		require(throws([&] { building.addCorridor(1, 0, 2, 2, 1); }),
			"A Corridor was accepted over a Background on the same Layer");
		require(throws([&] { building.addBackground(1, 0, 3, 2, 1); }),
			"A Background was accepted over a Background on the same Layer");

		// The Layers above and below are untouched by the Background's footprint.
		require(!throws([&] { building.addRoom("Room 1", 1, 1, 0, 4, 1); }),
			"A Location beside the Background was refused");
		require(!throws([&] { building.addRoom("Room 2", 2, 0, 0, 4, 1); }),
			"A Location on another Layer over the same cells was refused");

		footprintIsStamped(building, 1, taken, 0, 0, 4, 1);
	}

	// Minimum (1,1); a zero-sized block covers nothing and is refused.
	void theMinimumFootprintIsOneByOne()
	{
		core::Building building("Minimum size", 12, 3);
		addThirdLayer(building);

		std::string diagnostic;
		require(building.canAddBackground(1, 0, 0, 1, 1, &diagnostic),
			"The minimum 1x1 Background was refused");
		require(!building.canAddBackground(1, 0, 0, 0, 1, &diagnostic),
			"A zero-width Background was accepted");
		require(!building.canAddBackground(1, 0, 0, 1, 0, &diagnostic),
			"A zero-height Background was accepted");
		require(throws([&] { building.addBackground(1, 0, 0, 0, 1); }),
			"addBackground() accepted a zero-width block");
		require(throws([&] { building.addBackground(1, 0, 0, 1, 0); }),
			"addBackground() accepted a zero-height block");
	}

	// The Layer bounds and the Building bounds both cap the footprint.
	void boundsAndLayerCountAreEnforced()
	{
		core::Building building("Bounds", 12, 3);
		addThirdLayer(building);

		std::string diagnostic;
		require(building.canAddBackground(1, 2, 0, 12, 1, &diagnostic),
			"A Background filling the Building bounds was refused");
		require(!building.canAddBackground(1, 2, 1, 12, 1, &diagnostic),
			"A Background running past the Building width was accepted");
		require(!building.canAddBackground(1, 3, 0, 1, 1, &diagnostic),
			"A Background starting past the Building height was accepted");
		require(!building.canAddBackground(1, 2, 0, 1, 2, &diagnostic),
			"A Background running past the Building height was accepted");
		require(!building.canAddBackground(3, 0, 0, 1, 1, &diagnostic),
			"A Background on a Layer the Building does not have was accepted");
		require(!building.canAddBackground(~0u, 0, 0, 1, 1, &diagnostic),
			"A Background on an unset Layer index was accepted");
	}

	// Adjacent Backgrounds are allowed and never merge: each keeps its own colour
	// and its own Sector.
	void adjacentBackgroundsDoNotMerge()
	{
		core::Building building("No merging", 12, 3);
		addThirdLayer(building);
		auto const left = building.addBackground(1, 0, 0, 2, 2, { 250, 10, 10 });
		auto const right = building.addBackground(1, 0, 2, 2, 2, { 10, 250, 10 });

		require(left != right, "Two adjacent Backgrounds became one Sector");
		require(building.getNumSectors() == 2, "Adjacent Backgrounds merged into one Sector");
		require(backgroundIn(building, left)->getColour() == core::BackgroundColour{ 250, 10, 10 },
			"The first Background lost its colour to its neighbour");
		require(backgroundIn(building, right)->getColour() == core::BackgroundColour{ 10, 250, 10 },
			"The second Background lost its colour to its neighbour");
		require(backgroundIn(building, left)->getCellsWide() == 2
			&& backgroundIn(building, right)->getCellX() == 2,
			"Adjacent Backgrounds grew into each other");

		footprintIsStamped(building, 1, left, 0, 0, 2, 2);
		footprintIsStamped(building, 1, right, 0, 2, 2, 2);
	}

	// The colour travels as one packed 0xRRGGBB integer in the record.
	void thePackedColourRoundTrips()
	{
		constexpr uint32_t packed[] = { 0x000000u, 0x010203u, 0x6080A0u, 0xFEFDFCu, 0xFFFFFFu };
		for (auto const value : packed)
		{
			auto const colour = core::unpackBackgroundColour(value);
			require(core::packBackgroundColour(colour) == value,
				std::format("Colour {:06X} did not survive pack/unpack", value).c_str());
		}

		require(core::packBackgroundColour(core::BackgroundColour{ 96, 128, 160 }) == 0x6080A0u,
			"The default Background colour does not pack to 0x6080A0");
		require(core::unpackBackgroundColour(0xFF6080A0u) == core::BackgroundColour{ 96, 128, 160 },
			"Bits above 0xFFFFFF changed the unpacked colour");
	}

	// A version 5 round-trip preserves the packed colour, and a record with the
	// colour left out takes the default.
	void versionFiveRoundTripsThePackedColour()
	{
		core::Building building("Colour keeper", 12, 3);
		addThirdLayer(building);
		building.addRoom("Room 0", 0, 0, 0, 4, 1);
		building.addBackground(1, 1, 6, 4, 2, { 12, 240, 6 });
		building.addBackground(2, 0, 0, 1, 1);
		building.finishBuild();

		auto const yaml = serializeBuilding(building);
		require(yaml.find("version: 9") != std::string::npos,
			"The Building writer did not emit the current schema version");
		require(yaml.find("type: background") != std::string::npos,
			"The Background record was not written");
		require(yaml.find(std::format("colour: {}",
				core::packBackgroundColour(core::BackgroundColour{ 12, 240, 6 }))) != std::string::npos,
			"The Background colour was not written as a packed integer");

		core::Building loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getNumSectors() == 3, "Replay did not restore every Sector");
		require(backgroundIn(loaded, 1)->getColour() == core::BackgroundColour{ 12, 240, 6 },
			"The Background colour did not survive the round-trip");
		require(backgroundIn(loaded, 2)->getColour() == core::BackgroundColour{ 96, 128, 160 },
			"The default Background colour did not survive the round-trip");
		require(backgroundIn(loaded, 1)->getLayerIndex() == 1
			&& backgroundIn(loaded, 1)->getCellY() == 1 && backgroundIn(loaded, 1)->getCellX() == 6
			&& backgroundIn(loaded, 1)->getCellsWide() == 4
			&& backgroundIn(loaded, 1)->getDecksHigh() == 2,
			"The Background footprint did not survive the round-trip");
		footprintIsStamped(loaded, 1, 1, 1, 6, 4, 2);
	}

	// A hand-authored version 5 file - the shape the ticket asks for - loads, and
	// its Background reads as taken.
	void aHandAuthoredVersionFiveFileLoads()
	{
		auto const yaml = R"yaml(version: 5
name: Hand authored
cellsWide: 12
decksHigh: 3
layers: 2
layerNames:
  - Layer 0
  - Layer 1
construction:
  - type: background
    layer: 1
    y: 1
    x: 6
    cellsWide: 4
    decksHigh: 2
    colour: 12345678
  - type: room
    name: Front room
    layer: 0
    y: 0
    x: 0
    cellsWide: 6
    decksHigh: 1
    topDeckHeight: 0.9
agents: []
)yaml";

		core::Building loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);
		require(loaded.getNumSectors() == 2, "A hand-authored Background did not replay");
		require(backgroundIn(loaded, 0)->getColour() == core::unpackBackgroundColour(12345678),
			"A hand-authored Background colour was not read");
		footprintIsStamped(loaded, 1, 0, 1, 6, 4, 2);

		std::string diagnostic;
		require(!loaded.canAddBackground(1, 1, 6, 4, 2, &diagnostic),
			"The hand-authored Background footprint reads as free");
		require(!loaded.canAddBackground(1, 2, 7, 1, 1, &diagnostic),
			"A cell inside the hand-authored Background reads as free");
		require(loaded.canAddBackground(1, 1, 10, 1, 1, &diagnostic),
			"A cell clear of the hand-authored Background was refused");
	}

	// Replaying the authored records reproduces the Building, Backgrounds included,
	// and does so the same way every time.
	void recordReplayReproducesTheBuilding()
	{
		core::Building building("Replayed", 12, 3);
		addThirdLayer(building);
		building.addCorridor(0, 0, 12);
		building.addRoom("Room 1", 1, 0, 0, 4, 2);
		building.addBackground(1, 0, 4, 3, 2, { 200, 100, 50 });
		building.addBackground(1, 2, 8, 2, 1, { 5, 5, 250 });
		building.addRoom("Room 2", 2, 0, 0, 6, 3);
		building.finishBuild();

		auto const original = serializeBuilding(building);
		auto const originalSignature = sectorSignature(building);

		core::Building replay("placeholder", 1, 1);
		loadInto(replay, original);
		require(sectorSignature(replay) == originalSignature,
			("Record replay did not reproduce the Building\nexpected:\n" + originalSignature
				+ "actual:\n" + sectorSignature(replay)).c_str());

		// Replaying the replay drifts nothing: the records are stable across saves.
		auto const replayedYaml = serializeBuilding(replay);
		require(replayedYaml == original,
			"Re-saving a replayed Building changed its authored records");

		core::Building twice("placeholder", 1, 1);
		loadInto(twice, replayedYaml);
		require(sectorSignature(twice) == originalSignature,
			"A second replay of the same records drifted");
	}

	// A Background owns no walkable floor, so no Agent may belong to one.
	void anAgentCannotBelongToABackground()
	{
		core::Building building("No agents here", 12, 3);
		addThirdLayer(building);
		building.addRoom("Room 0", 0, 0, 0, 4, 1);
		auto const backdrop = building.addBackground(1, 0, 0, 4, 1);
		building.finishBuild();

		require(throws([&] { building.createAgent("Nowhere", backdrop); }),
			"An Agent was created inside a Background");
		require(throws([&] { building.createAgent("Nowhere either", backdrop, 0, 1.0f); }),
			"An Agent was created inside a Background with a deck offset");
		require(backgroundIn(building, backdrop)->getAgents().empty(),
			"A Background holds Agents after a refused placement");
	}

	// A Background takes no part in traversal: it contributes no vertices, and a
	// Window on the Layer in front may still look into it.
	void aBackgroundStaysOutOfTheGraphAndCanBeLookedInto()
	{
		core::Building building("Untraversed", 12, 3);
		addThirdLayer(building);
		building.addRoom("Room 0", 0, 0, 0, 4, 1);
		auto const backdrop = building.addBackground(1, 0, 0, 4, 1);
		building.finishBuild();

		require(building.getGraph() != nullptr, "A Building with a Background has no Graph");
		for (auto const& vertex : building.getGraph()->getVertices())
		{
			require(vertex == nullptr || vertex->getSector() == nullptr
				|| vertex->getSector()->getIndex() != backdrop,
				"A Background contributed a vertex to the Graph");
		}

		std::string diagnostic;
		require(building.canAddSectorWindow(0, 0, 0, 2, 1, &diagnostic),
			std::format("A Window may not look into a Background behind it: {}", diagnostic).c_str());
	}

	// Deleting the Layer takes the Background with it, and the record rewrite keeps
	// every surviving Layer index pointing where it should.
	void deletingALayerRemovesTheBackgroundOnIt()
	{
		core::Building building("Compacting", 12, 3);
		addThirdLayer(building);
		building.addRoom("Room 0", 0, 0, 0, 4, 1);
		building.addBackground(1, 0, 0, 4, 1, { 1, 2, 3 });
		building.addRoom("Room 2", 2, 0, 0, 4, 1);
		building.finishBuild();
		building.pauseSimulation();

		auto const plan = building.planDeleteLayer(1);
		require(plan.valid, ("Layer deletion was rejected: " + plan.diagnostic).c_str());
		require(plan.backgroundsRemoved == 1,
			"Layer deletion did not report the Background it removes");
		require(plan.locationsRemoved == 0,
			"Layer deletion counted the Background as a Location");
		require(plan.requiresConfirmation(),
			"A Background-removing Layer deletion did not require confirmation");
		require(building.applyDeleteLayer(plan), "Layer deletion was not applied");

		require(building.getLayerCount() == 2, "Layers were not compacted");
		require(building.getNumSectors() == 2, "The Background survived its Layer");
		for (uint32_t index = 0; index < building.getNumSectors(); ++index)
			require(building.getSector(index)->getType() != core::SectorType::Background,
				"A Background outlived the Layer it was on");
		require(building.getSector(1)->getLayerIndex() == 1,
			"The Layer behind the deletion did not compact forward by one");
	}

	// A Background on the back-most Layer keeps its Layer and its colour through a
	// save, which is the persistence half of "any Layer" holding true.
	void aBackMostBackgroundRoundTrips()
	{
		core::Building building("Back-most", 12, 3);
		addThirdLayer(building);
		building.addRoom("Room 0", 0, 0, 0, 4, 1);
		building.addBackground(2, 1, 1, 2, 2, { 7, 140, 250 });
		building.finishBuild();

		core::Building loaded("placeholder", 1, 1);
		loadInto(loaded, serializeBuilding(building));
		require(backgroundIn(loaded, 1)->getLayerIndex() == 2,
			"A back-most Background moved off its Layer");
		require(backgroundIn(loaded, 1)->getColour() == core::BackgroundColour{ 7, 140, 250 },
			"A back-most Background lost its colour");
	}
}

void runBackgroundPlacementSmokeChecks()
{
	theBackgroundRecordTypeRoundTripsByName();
	everyLayerAcceptsABackground();
	onlyTheSameLayersOccupiedCellsRefuseABackground();
	aBackgroundTakesUpTheSpaceItWasGiven();
	theMinimumFootprintIsOneByOne();
	boundsAndLayerCountAreEnforced();
	adjacentBackgroundsDoNotMerge();
	thePackedColourRoundTrips();
	versionFiveRoundTripsThePackedColour();
	aHandAuthoredVersionFiveFileLoads();
	recordReplayReproducesTheBuilding();
	anAgentCannotBelongToABackground();
	aBackgroundStaysOutOfTheGraphAndCanBeLookedInto();
	deletingALayerRemovesTheBackgroundOnIt();
	aBackMostBackgroundRoundTrips();
}
