// Window Layer rules, for ticket #20.
//
// A Window joins the Layer it is authored on to the Layer directly behind it.  A
// Window on the back-most Layer has nothing behind it: it pairs with no Layer, so
// it never reaches the Graph and can never be pathed through.  The core now
// refuses to author one, the editor inherits that refusal as its drop
// diagnostic, and a Layer deletion which would strand one on the new back-most
// Layer deletes it instead.  Maps saved before the rule can still replay such a
// Window unchanged - the Layer deletion is what clears them.

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

#include "core/Building.h"
#include "core/Graph.h"
#include "core/Sector.h"
#include "core/SectorObject.h"
#include "core/SectorObjectType.h"
#include "core/Vertex.h"
#include "core/Window.h"
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
		catch (...)
		{
			return true;
		}
		return false;
	}

	// A Building with a Room on every Layer over the same cells, so a Window could
	// be dropped on any of them if the rules let it.
	void authorRoomsOnEveryLayer(core::Building& building, uint32_t layers)
	{
		while (building.getLayerCount() < layers) building.addLayer();
		for (uint32_t layer = 0; layer < layers; ++layer)
			building.addRoom("Room " + std::to_string(layer), layer, 0, 0, 8, 1);
	}

	// A Window is one object shared by the Sectors on both sides of it, so count
	// distinct objects rather than the references to them.
	uint32_t countWindows(core::Building const& building)
	{
		std::set<std::shared_ptr<const core::SectorObject>> windows;
		for (uint32_t layer = 0; layer < building.getLayerCount(); ++layer)
		{
			for (auto const& sector : building.getSectors(layer))
			{
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (object && object->getObjectType() == core::SectorObjectType::Window)
						windows.insert(object);
				}
			}
		}
		return static_cast<uint32_t>(windows.size());
	}

	// A map written before a Window was required to have a Layer behind it: the
	// Window sits on the back-most Layer, where it joins nothing.
	char const* legacyBackMostWindowMap()
	{
		static std::string const yaml = R"YAML(
version: 4
name: Legacy Back-most Window
cellsWide: 8
decksHigh: 2
layers: 3
layerNames:
  - Layer 0
  - Layer 1
  - Deep
construction:
  - type: corridor
    layer: 0
    y: 0
    x: 0
    cellsWide: 8
    decksHigh: 1
  - type: room
    name: Mid
    layer: 1
    y: 0
    x: 0
    cellsWide: 8
    decksHigh: 1
    topDeckHeight: 0.9
  - type: room
    name: Deep
    layer: 2
    y: 0
    x: 0
    cellsWide: 8
    decksHigh: 1
    topDeckHeight: 0.9
  - type: window
    layer: 2
    y: 0
    x: 3
    cellsWide: 1
    decksHigh: 1
    traversable: true
    initialState: closed
    style: clear
agents: []
)YAML";
		return yaml.c_str();
	}

	void loadLegacyMap(core::Building& loaded)
	{
		auto reader = core::YamlSerializer::fromString(legacyBackMostWindowMap());
		reader->deserialize();
		core::SerializationWorkData workData;
		if (!loaded.deserialize(*reader, workData))
			throw std::runtime_error("The legacy back-most Window map did not load");
	}

	// The back-most Layer has nothing behind it, so it takes no new Window.  Every
	// other Layer does, and gains one as soon as a Layer is added behind it.
	void theBackMostLayerRefusesAWindow()
	{
		core::Building building("Back-most Window", 8, 2);
		building.pauseSimulation();
		authorRoomsOnEveryLayer(building, 3);

		std::string diagnostic;
		require(!building.canAddSectorWindow(2, 0, 1, 1, 1, &diagnostic),
			"canAddSectorWindow() allowed a Window on the back-most Layer");
		require(diagnostic.find("back-most") != std::string::npos,
			("The back-most Layer refusal did not say why: " + diagnostic).c_str());

		require(throws([&] { building.addSectorWindow(2, 0, 1, 1, 1, { true }); }),
			"addSectorWindow() accepted a Window on the back-most Layer");
		require(countWindows(building) == 0,
			"A refused Window still changed the Building");

		// The very same Window is fine once Layer 2 has a Layer behind it.
		building.addLayer();
		building.addRoom("Deeper", 3, 0, 0, 8, 1);
		require(building.canAddSectorWindow(2, 0, 1, 1, 1, &diagnostic),
			("A Window was refused after its Layer gained one behind it: " + diagnostic).c_str());
		require(building.addSectorWindow(2, 0, 1, 1, 1, { true }).object != nullptr,
			"A Window was not created once its Layer had one behind it");
	}

	// Every Layer which may hold a Window does hold one that crosses its own pair and
	// reaches the Graph.  A Window which never reaches the Graph is the symptom this
	// ticket was filed against, so pin the Vertex count rather than trust the cell.
	void everyWindowJoinsTheLayerBehindItAndReachesTheGraph()
	{
		core::Building building("Window pairs", 8, 2);
		building.pauseSimulation();
		authorRoomsOnEveryLayer(building, 4);
		building.finishBuild();

		uint32_t authored = 0;
		for (uint32_t layer = 0; layer + 1 < building.getLayerCount(); ++layer)
		{
			auto const before = building.getGraph()->getVertices().size();
			auto const created = building.addSectorWindow(layer, 0, 1 + layer, 1, 1, { true });
			building.finishBuild();
			auto const after = building.getGraph()->getVertices().size();

			require(created.object != nullptr, "addSectorWindow() returned no Window");
			require(created.window.sector->getLayerIndex() == layer,
				"The Window was not authored on the requested Layer");
			require(created.object->getFrontLayer() == layer,
				"The Window's front Layer is not the Layer it was authored on");
			require(created.object->getBackLayer() == layer + 1,
				"The Window does not reach the Layer directly behind it");
			require(created.object->getBackSector() != nullptr,
				"A Window was created with no back Sector");
			require(after > before,
				("A Window authored on Layer " + std::to_string(layer)
					+ " added no Vertex to the Graph").c_str());
			++authored;
		}

		require(countWindows(building) == authored,
			"Not every authored Window is in the Building");
	}

	// Loading a map is not where a Window is removed: the authored content
	// survives, even though nothing new may be authored that way.
	void aLegacyBackMostWindowLoadsUnchanged()
	{
		core::Building building("placeholder", 1, 1);
		loadLegacyMap(building);
		require(building.getLayerCount() == 3, "The legacy map did not keep its Layers");
		require(countWindows(building) == 1,
			"The legacy back-most Window was not loaded as authored");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		core::Building reloaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(reloaded.deserialize(*reader, workData),
			"The legacy back-most Window map did not round-trip");
		require(countWindows(reloaded) == 1,
			"The legacy back-most Window was lost in the round-trip");
	}

	// Deleting the Layer in front of the back-most Layer pulls that back-most Layer
	// forward under the Window, leaving it with nothing behind it.  The Window goes
	// with the deletion, and says so in the plan.
	void aLayerDeletionDeletesTheWindowItStrands()
	{
		core::Building building("placeholder", 1, 1);
		loadLegacyMap(building);
		building.pauseSimulation();

		auto const plan = building.planDeleteLayer(1);
		require(plan.valid, ("Deleting in front of a back-most Window was rejected: " + plan.diagnostic).c_str());
		require(plan.windowsStranded == 1,
			"The stranded Window was not reported as a casualty");
		require(plan.windowsRemoved == 0,
			"A Window which never crossed the deleted Layer was counted as crossing it");
		bool announced = false;
		for (auto const& consequence : plan.consequences)
		{
			if (consequence.find("back-most") != std::string::npos) announced = true;
		}
		require(announced, "The stranded Window was not in the plan's consequences");

		require(building.applyDeleteLayer(plan), "The stranded Window deletion was not applied");
		require(building.getLayerCount() == 2, "The Layers were not compacted");
		require(countWindows(building) == 0,
			"The stranded Window survived the Layer deletion");
		require(building.isTraversalTopologyValid(),
			("Deleting a stranded Window left an invalid topology: "
				+ building.getTopologyDiagnostic()).c_str());
	}

	// Deleting the front Layer pulls the Window's own Layer forward onto the new
	// back-most Layer, which strands it just the same.
	void aFrontLayerDeletionAlsoStrandsTheWindow()
	{
		core::Building building("placeholder", 1, 1);
		loadLegacyMap(building);
		building.pauseSimulation();

		auto const plan = building.planDeleteLayer(0);
		require(plan.valid, ("Front Layer deletion was rejected: " + plan.diagnostic).c_str());
		require(plan.windowsStranded == 1,
			"The Window stranded by the front Layer deletion was not reported");
		require(building.applyDeleteLayer(plan), "The front Layer deletion was not applied");
		require(countWindows(building) == 0,
			"The Window stranded by the front Layer deletion survived");
	}

	// A Window which really does cross the deleted Layer is still reported as
	// crossing it, not as stranded.
	void aWindowCrossingTheDeletedLayerIsStillCrossing()
	{
		core::Building building("Crossing Window", 8, 2);
		building.pauseSimulation();
		authorRoomsOnEveryLayer(building, 3);
		building.addSectorWindow(1, 0, 3, 1, 1, { true });
		building.finishBuild();

		auto const plan = building.planDeleteLayer(1);
		require(plan.valid, ("Deleting a crossed Layer was rejected: " + plan.diagnostic).c_str());
		require(plan.windowsRemoved == 1, "The crossed Window was not reported as crossing");
		require(plan.windowsStranded == 0, "A crossed Window was reported as stranded");
		require(building.applyDeleteLayer(plan), "The Layer deletion was not applied");
		require(countWindows(building) == 0, "The crossed Window survived its own Layer");
	}
}

void runWindowLayerSmokeChecks()
{
	theBackMostLayerRefusesAWindow();
	everyWindowJoinsTheLayerBehindItAndReachesTheGraph();
	aLegacyBackMostWindowLoadsUnchanged();
	aLayerDeletionDeletesTheWindowItStrands();
	aFrontLayerDeletionAlsoStrandsTheWindow();
	aWindowCrossingTheDeletedLayerIsStillCrossing();
}
