// The Selection panel's Background branch, for ticket #39.
//
// The panel itself is not reachable headlessly, so these checks mirror what it
// does: read the selected Background's readouts, take its colour through the same
// 0..1 float triple ImGui::ColorEdit3 edits (three floats, never four - there is
// no alpha control and no alpha to control), push the edit through the same
// Building call the panel makes, and delete through the same plan the panel's
// Delete button queues.
//
// What gets pinned down:
//
//   a panel colour edit round-trips through serialisation, geometry and all
//   the colour arithmetic carries no alpha channel, in either direction
//   a recolour touches nothing but the Background it was aimed at
//   anything which is not a Background is refused a recolour
//   the panel's Delete names every dependent Window before it fires
//
// The delete consequences themselves are the subject of #33 and are covered at
// length in BackgroundCascadeDeleteSmokeChecks.cpp; here they are checked only in
// the shape the panel puts them in.

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Background.h"
#include "core/Building.h"
#include "core/CellDefinition.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
#include "core/YamlSerializer.h"
#include "UI.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::shared_ptr<const core::Background> backgroundIn(core::Building const& building,
		uint32_t sectorIndex)
	{
		require(sectorIndex < building.getNumSectors(),
			"Building reported a Sector index outside itself");
		auto sector = building.getSector(sectorIndex);
		require(sector != nullptr, "Building reported a null Sector");
		require(sector->getType() == core::SectorType::Background,
			("Sector " + std::to_string(sectorIndex) + " is not a Background").c_str());
		auto background = std::dynamic_pointer_cast<const core::Background>(sector);
		require(background != nullptr, "A Background Sector is not a core::Background");
		return background;
	}

	// Mirrors the panel's colour edit: the widget is handed the live colour as
	// three floats, the user's edit comes back as three floats, and the panel
	// pushes what it got through Building::setBackgroundColour().
	bool panelRecolour(core::Building& building, uint32_t sectorIndex,
		core::BackgroundColour const& edited, std::string& diagnostic)
	{
		float rgb[3];
		core::backgroundColourToFloats(edited, rgb);
		return building.setBackgroundColour(sectorIndex,
			core::backgroundColourFromFloats(rgb), &diagnostic);
	}

	std::string snapshotYaml(core::Building& building)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	void reloadInto(core::Building& reloaded, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(reader != nullptr, "The serialised Building could not be read back");
		require(reloaded.deserialize(*reader, workData),
			"The recoloured Building did not reload");
	}

	std::shared_ptr<const core::Background> onlyBackground(core::Building const& building)
	{
		std::shared_ptr<const core::Background> found;
		for (uint32_t sector = 0; sector < building.getNumSectors(); ++sector)
		{
			auto const sectorPtr = building.getSector(sector);
			if (!sectorPtr || sectorPtr->getType() != core::SectorType::Background) continue;
			require(found == nullptr, "Expected exactly one Background left in the Building");
			found = std::dynamic_pointer_cast<const core::Background>(sectorPtr);
		}
		require(found != nullptr, "Expected a Background in the Building and found none");
		return found;
	}

	// Every Window in the Building, keyed by the Layer it is authored on and the
	// cell it sits on, so the set survives the index reshuffle a delete causes.
	std::set<std::string> windowKeys(core::Building const& building)
	{
		std::set<std::string> keys;
		for (uint32_t sector = 0; sector < building.getNumSectors(); ++sector)
		{
			auto const sectorPtr = building.getSector(sector);
			require(sectorPtr != nullptr, "Building reported a null Sector while finding Windows");
			for (uint32_t object = 0; object < sectorPtr->getNumObjects(); ++object)
			{
				auto windowObject = std::dynamic_pointer_cast<const core::WindowSectorObject>(
					sectorPtr->getObject(object));
				if (!windowObject || !windowObject->getWindow()) continue;
				keys.insert(std::format("Layer {}.cell {},{}",
					windowObject->getWindow()->getFrontLayer(),
					windowObject->getCellX(), windowObject->getCellY()));
			}
		}
		return keys;
	}

	bool namesWindow(std::vector<std::string> const& consequences, uint32_t layer,
		uint32_t x, uint32_t y)
	{
		auto const needle = std::format("Window at {},{} on Layer {}", x, y, layer);
		return std::any_of(consequences.begin(), consequences.end(), [&](std::string const& line)
		{
			return line.find(needle) != std::string::npos;
		});
	}

	std::set<std::string> namedWindows(core::Building const& building,
		std::vector<std::string> const& consequences)
	{
		std::set<std::string> named;
		for (uint32_t layer = 0; layer < building.getLayerCount(); ++layer)
			for (uint32_t x = 0; x < building.getCellsWide(); ++x)
				for (uint32_t y = 0; y < building.getDecksHigh(); ++y)
					if (namesWindow(consequences, layer, x, y))
						named.insert(std::format("Layer {}.cell {},{}", layer, x, y));
		return named;
	}

	std::string keysToString(std::set<std::string> const& keys)
	{
		std::string text;
		for (auto const& key : keys)
		{
			if (!text.empty()) text += " | ";
			text += key;
		}
		return text.empty() ? "(none)" : text;
	}

	std::string consequencesToString(std::vector<std::string> const& consequences)
	{
		std::string text;
		for (auto const& line : consequences)
		{
			if (!text.empty()) text += " | ";
			text += line;
		}
		return text.empty() ? "(none)" : text;
	}

	// The panel's map: a front Room with two Backgrounds behind it and one Window
	// looking into each of three things - two into the selected Background, one
	// into its neighbour as the control which must survive.
	//
	//   Layer 0  Front Room, Windows at x=1, x=3 (into A) and x=6 (into B)
	//   Layer 1  Backdrop A (0-4) | Backdrop B (5-9) | Behind Room (10-11)
	//   Layer 2  Deep Room
	struct PanelMap
	{
		uint32_t selected{ 0 };
		uint32_t neighbour{ 0 };
	};

	PanelMap authorPanelMap(core::Building& building)
	{
		PanelMap map;
		while (building.getLayerCount() < 3) building.addLayer();
		building.addRoom("Front", 0, 0, 0, 12, 1);
		map.selected = building.addBackground(1, 0, 0, 5, 1, { 20, 60, 100 });
		map.neighbour = building.addBackground(1, 0, 5, 5, 1, { 180, 90, 30 });
		building.addRoom("Behind", 1, 0, 10, 2, 1);
		building.addRoom("Deep", 2, 0, 0, 12, 1);
		building.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		building.addSectorWindow(0, 0, 3, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		building.addSectorWindow(0, 0, 6, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		return map;
	}
}

// What the panel prints for a selected Background: name, sector index, Layer,
// position, size, and the colour its colour widget starts from.
void thePanelReadsTheSelectedBackground()
{
	core::Building building("Panel readouts", 12, 3);
	while (building.getLayerCount() < 3) building.addLayer();
	building.addRoom("Front", 0, 0, 0, 12, 1);
	auto const index = building.addBackground(1, 1, 2, 3, 2, { 10, 20, 30 });
	building.addRoom("Deep", 2, 0, 0, 12, 1);
	building.finishBuild();

	auto const background = backgroundIn(building, index);
	require(background->getLayerIndex() == 1, "The Background is on the wrong Layer");
	require(background->getCellX() == 2 && background->getCellY() == 1,
		"The Background is not where the panel would say it is");
	require(background->getCellsWide() == 3 && background->getDecksHigh() == 2,
		"The Background size the panel reports is wrong");
	require(!background->getName().empty(), "The panel has no name to show");
	require(background->getColour() == (core::BackgroundColour{ 10, 20, 30 }),
		"The colour widget would open on the wrong colour");

	// A Background is selectable so the panel can be reached at all, and it is
	// still nothing an agent can occupy.
	require(core::Background::cellFloorType() == core::CellFloorType::None,
		"A Background started offering walkable floor");
	require(!background->sectorSupportsObjectType(core::SectorObjectType::Marker),
		"A Background started hosting objects");

	// The panel is reached by clicking a Background, so the canvas hit-test has to
	// accept the type; without this the panel below is unreachable.
	require(isCanvasSelectableSectorType(core::SectorType::Background),
		"A Background cannot be selected by the canvas hit-test, so the Selection panel can never open on one");
}

// A colour edit made through the panel's own path survives a save and reload, and
// drags nothing else along with it.
void aPanelColourEditRoundTripsThroughSerialisation()
{
	core::Building building("Panel recolour", 12, 3);
	while (building.getLayerCount() < 3) building.addLayer();
	building.addRoom("Front", 0, 0, 0, 12, 1);
	auto const index = building.addBackground(1, 0, 0, 4, 1, { 10, 20, 30 });
	building.addRoom("Deep", 2, 0, 0, 12, 1);
	building.finishBuild();

	auto const before = backgroundIn(building, index);
	require(before->getColour() == (core::BackgroundColour{ 10, 20, 30 }),
		"The Background did not start at its authored colour");

	std::string diagnostic;
	require(panelRecolour(building, index, { 220, 35, 180 }, diagnostic),
		"The panel recolour was refused: " + diagnostic);
	require(backgroundIn(building, index)->getColour() == (core::BackgroundColour{ 220, 35, 180 }),
		"The recolour did not reach the live Background");

	auto const yaml = snapshotYaml(building);
	require(yaml.find(std::format("colour: {}", core::packBackgroundColour(
		core::BackgroundColour{ 220, 35, 180 }))) != std::string::npos,
		"The saved record does not carry the recoloured value");

	core::Building reloaded("Panel recolour", 1, 1);
	reloadInto(reloaded, yaml);

	auto const after = backgroundIn(reloaded, index);
	require(after->getColour() == (core::BackgroundColour{ 220, 35, 180 }),
		std::format("The recolour did not survive the round-trip: got {},{},{}",
			after->getColour().r, after->getColour().g, after->getColour().b));
	// The edit was a recolour, not a move: the geometry the panel reports is untouched.
	require(after->getLayerIndex() == 1 && after->getCellX() == 0 && after->getCellY() == 0
		&& after->getCellsWide() == 4 && after->getDecksHigh() == 1,
		"The recolour changed the Background's geometry");
	require(reloaded.getNumSectors() == building.getNumSectors(),
		"The recolour changed the Sector count");
	require(reloaded.isTraversalTopologyValid(),
		"The recolour left an invalid topology: " + reloaded.getTopologyDiagnostic());
}

// The colour crosses the widget boundary as three floats and nothing else, and
// crosses back exactly.
void theColourEditCarriesNoAlphaChannel()
{
	// Every byte value survives the float round-trip exactly, on every channel.
	for (int value = 0; value < 256; ++value)
	{
		auto const byte = (uint8_t)value;
		float rgb[3];
		core::backgroundColourToFloats(core::BackgroundColour{ byte, 0, 0 }, rgb);
		require(core::backgroundColourFromFloats(rgb).r == byte,
			std::format("Red {} did not survive the widget round-trip", value));
		core::backgroundColourToFloats(core::BackgroundColour{ 0, byte, 0 }, rgb);
		require(core::backgroundColourFromFloats(rgb).g == byte,
			std::format("Green {} did not survive the widget round-trip", value));
		core::backgroundColourToFloats(core::BackgroundColour{ 0, 0, byte }, rgb);
		require(core::backgroundColourFromFloats(rgb).b == byte,
			std::format("Blue {} did not survive the widget round-trip", value));
	}

	// Nothing outside 0..1 escapes: an over-driven or negative widget clamps, and a
	// NaN collapses low rather than becoming an arbitrary byte.
	auto const clamped = core::backgroundColourFromFloats(std::array<float, 3>{ 4.0f, -1.0f, 0.5f }.data());
	require(clamped.r == 255 && clamped.g == 0,
		"An out-of-range colour did not clamp to the nearest byte");
	auto const nan = core::backgroundColourFromFloats(
		std::array<float, 3>{ std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f }.data());
	require(nan.r == 0, "A NaN channel became something other than 0");

	// And a whole colour never carries alpha bits, whichever way it was written.
	require(core::packBackgroundColour(core::backgroundColourFromFloats(
		std::array<float, 3>{ 1.0f, 1.0f, 1.0f }.data())) == 0x00FFFFFFu,
		"A recolour set bits above the RGB channels");
	require(core::unpackBackgroundColour(0xFF102030u)
		== (core::BackgroundColour{ 0x10, 0x20, 0x30 }),
		"An alpha-bearing packed colour unpacked to something else");
}

// A recolour touches the Background it was aimed at and nothing else - not the
// Background next to it, not the Rooms in front of it.
void aRecolourTouchesNothingButItsOwnBackground()
{
	core::Building building("Recolour isolation", 12, 3);
	auto const map = authorPanelMap(building);
	building.finishBuild();

	auto const beforeA = backgroundIn(building, map.selected)->getColour();
	auto const beforeB = backgroundIn(building, map.neighbour)->getColour();
	require(beforeA != beforeB, "The test map gave both Backgrounds the same colour");

	std::string diagnostic;
	require(panelRecolour(building, map.selected, { 5, 200, 240 }, diagnostic),
		"The panel recolour was refused: " + diagnostic);

	require(backgroundIn(building, map.selected)->getColour()
		== (core::BackgroundColour{ 5, 200, 240 }),
		"The targeted Background did not take the new colour");
	require(backgroundIn(building, map.neighbour)->getColour() == beforeB,
		"The neighbouring Background changed colour with its neighbour");
	require(building.isTraversalTopologyValid(),
		"The recolour left an invalid topology: " + building.getTopologyDiagnostic());

	// And the round-trip keeps both colours distinct.
	core::Building reloaded("Recolour isolation", 1, 1);
	reloadInto(reloaded, snapshotYaml(building));
	require(backgroundIn(reloaded, map.selected)->getColour()
		== (core::BackgroundColour{ 5, 200, 240 }),
		"The recolour did not reload");
	require(backgroundIn(reloaded, map.neighbour)->getColour() == beforeB,
		"The neighbouring Background reloaded with the wrong colour");
}

// Anything which is not a Background is refused a recolour, and stays exactly as
// it was.
void aRecolourIsRefusedForAnythingWhichIsNotABackground()
{
	core::Building building("Recolour refusal", 12, 3);
	while (building.getLayerCount() < 3) building.addLayer();
	auto const room = building.addRoom("Front", 0, 0, 0, 6, 1);
	building.addRoom("Deep", 1, 0, 0, 6, 1);
	building.finishBuild();

	std::string diagnostic;
	require(!building.setBackgroundColour(room, { 255, 0, 0 }, &diagnostic),
		"A Location accepted a Background recolour");
	require(!diagnostic.empty(), "The recolour refusal gave no diagnostic");
	require(!building.setBackgroundColour(building.getNumSectors() + 8, { 255, 0, 0 },
		&diagnostic),
		"An index outside the Building accepted a recolour");
	require(!diagnostic.empty(), "The out-of-range recolour refusal gave no diagnostic");

	require(building.getSector(room)->getType() == core::SectorType::Location,
		"The refused recolour changed the Sector's type");
	require(building.isTraversalTopologyValid(),
		"The refused recolour left an invalid topology: " + building.getTopologyDiagnostic());
}

// The panel's Delete button queues the cascade from #33: the plan names every
// Window which looks into the Background being deleted, and nothing else.
void thePanelDeleteNamesEveryDependentWindow()
{
	core::Building building("Panel delete", 12, 3);
	auto const map = authorPanelMap(building);
	building.finishBuild();

	auto const before = windowKeys(building);
	require(before.size() == 3,
		"The panel map did not author three Windows: " + keysToString(before));

	auto const plan = building.planRemoveBackground(map.selected);
	require(plan.valid, "The panel's delete plan was refused: " + plan.diagnostic);
	require(plan.requiresConfirmation(),
		"A delete which takes Windows did not ask for confirmation: "
		+ consequencesToString(plan.consequences));

	// The two Windows looking into the deleted Background are named; the one
	// looking into its neighbour is not.
	auto const named = namedWindows(building, plan.consequences);
	require(named == (std::set<std::string>{ "Layer 0.cell 1,0", "Layer 0.cell 3,0" }),
		"The delete named " + keysToString(named)
		+ ", expected the two Windows looking into the deleted Background");

	building.applyBackgroundEdit(plan);
	auto const after = windowKeys(building);
	require(after == (std::set<std::string>{ "Layer 0.cell 6,0" }),
		"After the delete the Building holds " + keysToString(after));
	// Sector indices compact when a Background goes, so the survivor is found by
	// what it is rather than by the index it was authored with.
	require(onlyBackground(building)->getColour() == (core::BackgroundColour{ 180, 90, 30 }),
		"The surviving Background changed with its neighbour's deletion");
	require(building.isTraversalTopologyValid(),
		"The delete left an invalid topology: " + building.getTopologyDiagnostic());
}

// A Background nobody looks into deletes with no consequence list, so the panel
// commits it without a confirmation popup.
void aDeleteWithNoDependentWindowsNeedsNoConfirmation()
{
	core::Building building("Panel delete, unwatched", 12, 3);
	while (building.getLayerCount() < 3) building.addLayer();
	building.addRoom("Front", 0, 0, 0, 12, 1);
	auto const index = building.addBackground(1, 0, 0, 4, 1, { 10, 20, 30 });
	building.addRoom("Deep", 2, 0, 0, 12, 1);
	building.finishBuild();

	auto const plan = building.planRemoveBackground(index);
	require(plan.valid, "The panel's delete plan was refused: " + plan.diagnostic);
	require(!plan.requiresConfirmation(),
		"An unwatched Background delete still asked for confirmation: "
		+ consequencesToString(plan.consequences));
}

void runBackgroundSelectionPanelSmokeChecks()
{
	thePanelReadsTheSelectedBackground();
	aPanelColourEditRoundTripsThroughSerialisation();
	theColourEditCarriesNoAlphaChannel();
	aRecolourTouchesNothingButItsOwnBackground();
	aRecolourIsRefusedForAnythingWhichIsNotABackground();
	thePanelDeleteNamesEveryDependentWindow();
	aDeleteWithNoDependentWindowsNeedsNoConfirmation();
}
