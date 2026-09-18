// A Window looks into a single Background, for ticket #32.
//
// This ticket proves rather than builds: the acceptance falls out of the hosting /
// look-target predicate split (#28) and the Background look-target override (#31),
// and the one real guard - a traversable Window may never mint a traversal
// resource into a Background - landed with the threshold audit (#31). What is
// pinned down here is the shape of the result:
//
//   Window in front of a Background            -> accepted; the old "back Layer
//                                               cell is not occupied" refusal is
//                                               gone for this arrangement
//   createWindow() wiring                      -> the Background is the Window's
//                                               back Sector, and the Background
//                                               holds the Window's SectorObject
//   traversable Window into a Background       -> refused, and nothing is minted:
//                                               no resource, no edge, no vertex
//   Background in the Graph                    -> wholly absent: no vertices, no
//                                               edges, no traversal resources
//   Serialisation replay                       -> the back-sector object
//                                               reference is rebuilt from the
//                                               records, colour and all
//   Hand-authored traversable Window into a
//   Background                                 -> refused on load too: the guard
//                                               is not an authoring-only courtesy

#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Background.h"
#include "core/Building.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Edge.h"
#include "core/EdgeType.h"
#include "core/Graph.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Simulation.h"
#include "core/Vector2.h"
#include "core/Vertex.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
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

	std::string exceptionMessage(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (std::exception const& error)
		{
			return error.what();
		}
		return {};
	}

	// Every distinct Window the Building holds.  A Window's SectorObject is
	// registered in both of its Sectors, so the same Window is seen twice while
	// scanning and is reported once.
	std::vector<std::shared_ptr<const core::Window>> windowsIn(core::Building const& building)
	{
		std::vector<std::shared_ptr<const core::Window>> found;
		std::set<core::Window const*> seen;
		for (uint32_t sectorIndex = 0; sectorIndex < building.getNumSectors(); ++sectorIndex)
		{
			auto const sector = building.getSector(sectorIndex);
			require(sector != nullptr, "Building reported a null Sector while finding Windows");
			for (uint32_t objectIndex = 0; objectIndex < sector->getNumObjects(); ++objectIndex)
			{
				auto windowObject = std::dynamic_pointer_cast<const core::WindowSectorObject>(
					sector->getObject(objectIndex));
				if (!windowObject) continue;
				auto window = windowObject->getWindow();
				require(window != nullptr, "A Window SectorObject holds no Window");
				if (seen.insert(window.get()).second) found.push_back(window);
			}
		}
		return found;
	}

	// The WindowSectorObject of a given Window as registered in one Sector.
	std::shared_ptr<const core::WindowSectorObject> windowObjectIn(
		std::shared_ptr<const core::Sector> sector,
		std::shared_ptr<const core::Window> const& window)
	{
		require(sector != nullptr, "Looked for a Window in a null Sector");
		for (uint32_t objectIndex = 0; objectIndex < sector->getNumObjects(); ++objectIndex)
		{
			auto windowObject = std::dynamic_pointer_cast<const core::WindowSectorObject>(
				sector->getObject(objectIndex));
			if (windowObject && windowObject->getWindow() == window) return windowObject;
		}
		return nullptr;
	}

	std::shared_ptr<const core::Sector> sectorByIndex(core::Building const& building, uint32_t index)
	{
		auto sector = building.getSector(index);
		require(sector != nullptr, "Building reported a null Sector");
		return sector;
	}

	std::shared_ptr<const core::Background> backgroundByIndex(core::Building const& building,
		uint32_t index)
	{
		auto background = std::dynamic_pointer_cast<const core::Background>(
			sectorByIndex(building, index));
		require(background != nullptr,
			std::format("Sector {} is not a Background", index).c_str());
		return background;
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

	uint32_t windowTraversalResources(core::Building const& building)
	{
		uint32_t count{ 0 };
		for (auto const& resource : building.getSimulationSnapshot().traversalResources)
			if (resource.isWindow) ++count;
		return count;
	}

	uint32_t edgesOfType(core::Building const& building, core::EdgeType type)
	{
		uint32_t count{ 0 };
		for (auto const& edge : building.getGraph()->getEdges())
			if (edge && edge->getType() == type) ++count;
		return count;
	}

	// The whole ticket in one arrangement: a Room up front, a Background directly
	// behind part of it, and a Room behind the rest.
	//
	//   Layer 0  Front Room                    (Windows are authored here)
	//   Layer 1  Backdrop (bg)  |  Behind Room
	//   Layer 2  Deep Room
	//
	// The Behind Room gives the traversable control something legal to cross into,
	// so "no edge into a Background" is measured against a real edge elsewhere
	// rather than being vacuously true.
	struct BackdropLayout
	{
		uint32_t front{ 0 };
		uint32_t backdrop{ 0 };
		uint32_t behind{ 0 };
		uint32_t deep{ 0 };
	};

	BackdropLayout authorBackdrop(core::Building& building)
	{
		BackdropLayout layout;
		while (building.getLayerCount() < 3) building.addLayer();
		layout.front = building.addRoom("Front", 0, 0, 0, 12, 1);
		layout.backdrop = building.addBackground(1, 0, 0, 6, 1, { 200, 120, 40 });
		layout.behind = building.addRoom("Behind", 1, 0, 6, 6, 1);
		layout.deep = building.addRoom("Deep", 2, 0, 0, 12, 1);
		return layout;
	}

	// A Window on the Layer in front of a Background is accepted, and the old
	// "back Layer cell is not occupied" refusal does not come with it.
	void aWindowLookingIntoABackgroundIsAccepted()
	{
		// Control: the same Window with an empty Layer behind it is still refused,
		// and refused with the very message the Background makes disappear.
		core::Building bare("Nothing behind", 12, 3);
		while (bare.getLayerCount() < 3) bare.addLayer();
		bare.addRoom("Front", 0, 0, 0, 12, 1);
		std::string emptyDiagnostic;
		require(!bare.canAddSectorWindow(0, 0, 2, 2, 1, &emptyDiagnostic),
			"A Window with an empty Layer behind it was accepted");
		require(emptyDiagnostic.find("not occupied") != std::string::npos,
			("The empty-Layer refusal changed shape: " + emptyDiagnostic).c_str());

		// With a Background behind, the same query passes cleanly.
		core::Building building("Window into Background", 12, 3);
		auto const layout = authorBackdrop(building);
		std::string diagnostic;
		require(building.canAddSectorWindow(0, 0, 2, 2, 1, &diagnostic),
			("A Window was refused looking into a Background: " + diagnostic).c_str());
		require(diagnostic.empty(),
			("An accepted Window still carried a diagnostic: " + diagnostic).c_str());

		auto created = building.addSectorWindow(0, 0, 2, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		require(created.window.sector != nullptr, "The looking Window has no front Sector");
		require(created.object != nullptr, "The looking Window has no Window");
		require(!created.traversalResource,
			"A looking Window into a Background was given a traversal resource");

		auto const window = created.object;
		require(window->getFrontSector() != nullptr
			&& window->getFrontSector()->getIndex() == layout.front,
			"The Window's front Sector is not the Room it was authored in");
		require(window->getBackSector() != nullptr
			&& window->getBackSector()->getType() == core::SectorType::Background,
			"The Window's back Sector is not the Background behind it");
		require(window->getBackSector()->getIndex() == layout.backdrop,
			"The Window looks into the wrong Background");
		require(window->getFrontLayer() == 0 && window->getBackLayer() == 1,
			"The Window does not cross the Layer pair it was authored on");
		require(!window->isTraversalConfigured(),
			"A looking Window reports its traversal as configured");
		require(!window->getTraversalResourceId(),
			"A looking Window into a Background holds a traversal resource handle");
	}

	// createWindow() wiring: the Background is handed the Window as its look-target
	// object, so the arrangement reads the same from both sides.
	void theBackgroundHoldsTheWindowThatLooksIntoIt()
	{
		core::Building building("Both sides", 12, 3);
		auto const layout = authorBackdrop(building);
		auto created = building.addSectorWindow(0, 0, 1, 3, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		building.finishBuild();

		auto const backdrop = backgroundByIndex(building, layout.backdrop);
		require(backdrop->getLayerIndex() == 1, "The Background is not on Layer 1");
		auto const inBackground = windowObjectIn(backdrop, created.object);
		require(inBackground != nullptr,
			"The Background does not hold the WindowSectorObject that looks into it");
		require(inBackground == windowObjectIn(created.window.sector, created.object),
			"The Window's SectorObject differs when read from the Background");
		require(inBackground->getSector() != nullptr
			&& inBackground->getSector()->getIndex() == layout.front,
			"The Window's SectorObject reports the wrong home Sector");

		// Being looked at costs the Background nothing: it still owns no floor, and
		// the cells the Window faces stay untraversable.
		core::Building const& asBuilt = building;
		auto const layer = asBuilt.getLayer(1);
		for (uint32_t x = 1; x < 4; ++x)
		{
			auto const& cell = layer->getCellDefinition(x, 0);
			require(cell.sectorIndex == layout.backdrop,
				std::format("The Background no longer owns cell {},{} on Layer 1", x, 0).c_str());
			require(cell.floorType == core::CellFloorType::None
				&& !cell.isTraversableOnFoot(),
				std::format("A looked-into Background cell became traversable: {},{}", x, 0).c_str());
		}
	}

	// The guard: authoring a traversable Window into a Background is refused, and
	// the refusal leaves nothing behind - no resource, no edge, no vertex - while
	// the looking Window stays perfectly legal in the very same spot.
	void aTraversableWindowIntoABackgroundMintsNothing()
	{
		core::Building building("Traversable refused", 12, 3);
		authorBackdrop(building);

		require(windowTraversalResources(building) == 0,
			"The backdrop Building starts with a traversal resource already");

		auto const message = exceptionMessage([&]
		{
			building.addSectorWindow(0, 0, 1, 2, 1,
				{ true, core::Window::State::Open, core::Window::Style::Clear });
		});
		require(!message.empty(), "A traversable Window into a Background was accepted");
		require(message.find("Background") != std::string::npos,
			("The refusal does not name the Background: " + message).c_str());

		require(windowsIn(building).empty(),
			"The refused traversable Window still left a Window behind");
		require(windowTraversalResources(building) == 0,
			"The refused traversable Window minted a traversal resource");
		require(edgesOfType(building, core::EdgeType::Window) == 0,
			"The refused traversable Window put a Window edge in the Graph");

		// The refusal is about crossing, not looking: the same placement without
		// traversal is accepted straight after.
		require(!throws([&]
		{
			building.addSectorWindow(0, 0, 1, 2, 1,
				{ false, core::Window::State::Closed, core::Window::Style::Clear });
		}),
			"A looking Window was refused after the traversable refusal");
		require(windowTraversalResources(building) == 0,
			"A looking Window into a Background minted a traversal resource");
	}

	// The Background is wholly absent from the Graph: no vertex, no edge, and no
	// traversal resource reaches it - while a traversable Window elsewhere does
	// produce its edge, so that absence is measured, not assumed.
	void theBackgroundIsWhollyAbsentFromTheGraph()
	{
		core::Building building("Graph absence", 12, 3);
		auto const layout = authorBackdrop(building);
		building.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		auto const control = building.addSectorWindow(0, 0, 7, 2, 1,
			{ true, core::Window::State::Open, core::Window::Style::Clear });
		building.finishBuild();

		require(building.getGraph() != nullptr, "The Building has no Graph");
		require(building.isTraversalTopologyValid(),
			("The Building's topology is invalid: " + building.getTopologyDiagnostic()).c_str());

		for (auto const& vertex : building.getGraph()->getVertices())
		{
			require(vertex == nullptr || vertex->getSector() == nullptr
				|| vertex->getSector()->getType() != core::SectorType::Background,
				"A Background contributed a vertex to the Graph");
		}

		for (auto const& edge : building.getGraph()->getEdges())
		{
			if (!edge) continue;
			for (uint32_t side = 0; side < 2; ++side)
			{
				auto const vertex = edge->getVertex(side);
				require(vertex == nullptr || vertex->getSector() == nullptr
					|| vertex->getSector()->getType() != core::SectorType::Background,
					std::format("Edge {} touches a Background at end {}", edge->getId(), side).c_str());
			}
		}

		// Nothing may be found inside the Background: it is not even a key in the
		// Graph's Sector -> Vertex lookup.
		require(throws([&]
		{
			building.getGraph()->getClosestVertexInSector(
				sectorByIndex(building, layout.backdrop).get(), { 3.0f, 0.5f });
		}),
			"A Background is present in the Graph's Sector -> Vertex lookup");

		// The control: the traversable Window into the Behind Room does have its
		// edge and its resource, so the checks above are not vacuous.
		require(edgesOfType(building, core::EdgeType::Window) == 1,
			"The traversable control Window did not contribute its edge");
		require(windowTraversalResources(building) == 1,
			"The traversable control Window did not mint its resource");
		require(static_cast<bool>(control.traversalResource),
			"The traversable control Window has no resource handle");

		// And the looking Window is not secretly that edge either.
		uint32_t lookingWindows{ 0 }, crossingWindows{ 0 };
		for (auto const& window : windowsIn(building))
		{
			if (window->isTraversalConfigured()) ++crossingWindows;
			else ++lookingWindows;
		}
		require(lookingWindows == 1 && crossingWindows == 1,
			std::format("Expected one looking and one crossing Window, got {} and {}",
				lookingWindows, crossingWindows).c_str());
	}

	// Serialisation replay reproduces the arrangement, back-sector reference
	// included: the Window still looks into the same Background, and the
	// Background still holds the Window's SectorObject.
	void theBackSectorReferenceSurvivesSerialisationReplay()
	{
		core::Building building("Replayed look", 12, 3);
		auto const layout = authorBackdrop(building);
		building.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		building.finishBuild();

		auto const yaml = serializeBuilding(building);
		require(yaml.find("traversable: false") != std::string::npos,
			"A looking Window was not written as non-traversable");

		core::Building loaded("placeholder", 1, 1);
		loadInto(loaded, yaml);

		auto const replayed = windowsIn(loaded);
		require(replayed.size() == 1, "The replay did not restore exactly one Window");
		auto const window = replayed.front();
		require(window->getBackSector() != nullptr
			&& window->getBackSector()->getType() == core::SectorType::Background,
			"The replayed Window's back Sector is not a Background");
		require(window->getBackSector()->getIndex() == layout.backdrop,
			"The replayed Window looks into a different Sector than the Background did");
		require(window->getFrontSector() != nullptr
			&& window->getFrontSector()->getName() == "Front",
			"The replayed Window's front Sector is not the Front Room");
		require(window->getFrontLayer() == 0 && window->getBackLayer() == 1,
			"The replayed Window crosses a different Layer pair");
		require(!window->isTraversalConfigured() && !window->getTraversalResourceId(),
			"The replayed Window came back traversable");

		auto const backdrop = backgroundByIndex(loaded, layout.backdrop);
		require(backdrop->getColour() == core::BackgroundColour{ 200, 120, 40 },
			"The Background lost its colour through the replay");
		require(windowObjectIn(backdrop, window) != nullptr,
			"The Background does not hold the replayed Window's SectorObject");

		// The replayed Building is just as absent from the Graph as the original.
		require(windowTraversalResources(loaded) == 0,
			"The replay minted a Window traversal resource");
		require(edgesOfType(loaded, core::EdgeType::Window) == 0,
			"The replay produced a Window edge into a Background");
		for (auto const& vertex : loaded.getGraph()->getVertices())
			require(vertex == nullptr || vertex->getSector() == nullptr
				|| vertex->getSector()->getType() != core::SectorType::Background,
				"The replay put a vertex in a Background");

		// Re-saving the replay drifts nothing, so the arrangement is stable
		// across repeated saves.
		require(serializeBuilding(loaded) == yaml,
			"Re-saving the replayed arrangement changed it");
	}

	// The guard is not an authoring-only courtesy: a hand-authored file that asks
	// for a traversable Window across a Background is refused on load rather than
	// quietly building a resource into a Sector no agent can occupy.
	void aHandAuthoredTraversableWindowIntoABackgroundIsRefusedOnLoad()
	{
		auto const yaml = R"yaml(version: 5
name: Hand authored crossing
cellsWide: 12
decksHigh: 3
layers: 3
construction:
  - type: room
    name: Front
    layer: 0
    y: 0
    x: 0
    cellsWide: 12
    decksHigh: 1
    topDeckHeight: 0.9
  - type: background
    layer: 1
    y: 0
    x: 0
    cellsWide: 6
    decksHigh: 1
    colour: 13148200
  - type: room
    name: Behind
    layer: 1
    y: 0
    x: 6
    cellsWide: 6
    decksHigh: 1
    topDeckHeight: 0.9
  - type: room
    name: Deep
    layer: 2
    y: 0
    x: 0
    cellsWide: 12
    decksHigh: 1
    topDeckHeight: 0.9
  - type: window
    layer: 0
    y: 0
    x: 1
    cellsWide: 2
    decksHigh: 1
    traversable: true
    initialState: open
    style: clear
agents: []
)yaml";

		core::Building loaded("placeholder", 1, 1);
		auto const message = exceptionMessage([&] { loadInto(loaded, yaml); });
		require(!message.empty(),
			"A hand-authored traversable Window into a Background loaded silently");
		require(message.find("Background") != std::string::npos,
			("The load refusal does not name the Background: " + message).c_str());

		// Nothing half-built survives the refused load.
		require(windowTraversalResources(loaded) == 0,
			"The refused load left a Window traversal resource behind");
		for (auto const& window : windowsIn(loaded))
			require(!window->isTraversalConfigured(),
				"The refused load left a configured-traversable Window behind");
	}

	// The same arrangement from the simulation side: with no vertex and no edge in
	// the Background, no route can ever cross into one, while the traversable
	// control Window across the Behind Room still carries an Agent.
	void noRouteCanEverCrossIntoABackground()
	{
		core::Building building("Unroutable backdrop", 12, 3);
		auto const layout = authorBackdrop(building);
		building.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		building.addSectorWindow(0, 0, 7, 2, 1,
			{ true, core::Window::State::Open, core::Window::Style::Clear });
		// A Door further along the Behind Room gives the route a way on to Layer 2,
		// so a successful path proves nothing about the Background beside it.
		building.addSectorDoor(1, 0, 10);
		building.addSectorMarker(layout.deep, 0, 8.5f, nullptr);
		building.finishBuild();

		auto const agentId = building.createAgent("Front walker", layout.front, 0, 0.5f);
		auto const agent = building.lookupAgent(agentId).entity;
		require(agent != nullptr, "The front Agent was not created");

		auto const target = building.getGraph()->getClosestVertexInSector(
			sectorByIndex(building, layout.deep).get(), { 8.5f, 0.0f });
		require(target != nullptr, "The Deep Room destination has no vertex");

		auto const path = building.getGraph()->calculatePath(agent, target);
		require(path != nullptr, "No route exists across the traversable control Window");
		for (auto const& node : path->nodes)
			require(node.targetVertex == nullptr || node.targetVertex->getSector() == nullptr
				|| node.targetVertex->getSector()->getType() != core::SectorType::Background,
				"A route passes through a Background");

		bool crossesWindow{ false };
		for (auto const& node : path->nodes)
			if (node.edge && node.edge->getType() == core::EdgeType::Window) crossesWindow = true;
		require(crossesWindow, "The route did not use the traversable control Window");
	}
}

void runWindowIntoBackgroundSmokeChecks()
{
	aWindowLookingIntoABackgroundIsAccepted();
	theBackgroundHoldsTheWindowThatLooksIntoIt();
	aTraversableWindowIntoABackgroundMintsNothing();
	theBackgroundIsWhollyAbsentFromTheGraph();
	theBackSectorReferenceSurvivesSerialisationReplay();
	aHandAuthoredTraversableWindowIntoABackgroundIsRefusedOnLoad();
	noRouteCanEverCrossIntoABackground();
}
