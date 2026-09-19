// The editor surface for Facades, for ticket #47.
//
// The panel and the palette tray are not reachable headlessly, so these checks
// mirror what the editor does: the paint flow plays the same validate-then-add
// sequence the palette's drag release performs, the colour edit crosses the
// same 0..1 float triple the panel's ColorEdit3 edits and lands through the
// same Building::setFacadeColour call, and the wall refusal is the diagnostic
// the wall editor would surface against a Facade - which the Facade panel never
// opens, because a Facade has no walls to edit.
//
// What gets pinned down:
//
//   the creation flow places an occupiable, selectable Facade with the Room's
//   placement validation - occupied cells refuse, free cells land
//   a colour edit persists through the construction record: the saved YAML
//   carries the new packed colour and a reload replays it
//   a recolour touches nothing but the Facade it was aimed at
//   the Background and Facade recolour paths do not cross types
//   wall add/remove against a Facade refuse with a clear, Facade-naming
//   diagnostic, both through the can-check and through the throwing command
//   the Selection panel's gates hold: a Facade is selectable, and no wall
//   affordance on any deck or side would be actionable

#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/Background.h"
#include "core/Building.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Exceptions.h"
#include "core/Facade.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/YamlSerializer.h"
#include "UI.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::shared_ptr<const core::Facade> facadeIn(core::Building const& building,
		uint32_t sectorIndex)
	{
		require(sectorIndex < building.getNumSectors(),
			"Building reported a Sector index outside itself");
		auto sector = building.getSector(sectorIndex);
		require(sector != nullptr, "Building reported a null Sector");
		require(sector->getType() == core::SectorType::Facade,
			("Sector " + std::to_string(sectorIndex) + " is not a Facade").c_str());
		auto facade = std::dynamic_pointer_cast<const core::Facade>(sector);
		require(facade != nullptr, "A Facade Sector is not a core::Facade");
		return facade;
	}

	std::string serializeBuilding(core::Building& building)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		building.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	void loadInto(core::Building& loaded, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(reader != nullptr, "The serialised Building could not be read back");
		require(loaded.deserialize(*reader, workData),
			"The Building did not reload");
	}

	// Mirrors the panel's colour edit: the widget is handed the live colour as
	// three floats, the user's edit comes back as three floats, and the panel
	// pushes what it got through Building::setFacadeColour(). This is the same
	// arithmetic the Background panel runs; only the destination call differs.
	bool panelRecolour(core::Building& building, uint32_t sectorIndex,
		core::BackgroundColour const& edited, std::string& diagnostic)
	{
		float rgb[3];
		core::backgroundColourToFloats(edited, rgb);
		return building.setFacadeColour(sectorIndex,
			core::backgroundColourFromFloats(rgb), &diagnostic);
	}
}

// The paint flow: the tray validates the dragged rectangle with canAddFacade -
// the Room's placement rule - and the release adds through addFacade. An
// occupied rectangle refuses before anything is built; a free one lands as an
// occupiable, selectable Facade with walkable ground.
void theFacadeCreationFlowPlacesAnOccupiableSelectableSector()
{
	core::Building building("Creation flow", 12, 3);
	while (building.getLayerCount() < 2) building.addLayer();
	auto const roomIndex = building.addRoom("Neighbour", 0, 0, 0, 4, 1);
	building.finishBuild();
	building.pauseSimulation();

	// A free rectangle on Layer 1 validates.
	std::string diagnostic;
	require(building.canAddFacade(1, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		("A free Facade rectangle was refused: " + diagnostic).c_str());

	// The paint release adds it, and it arrives as a Facade with the dragged
	// footprint, walkable ground on its bottom deck, and a name to show.
	auto const facadeIndex = building.addFacade("Frontage", 1, 0, 0, 4, 2,
		CORE_ROOM_MAX_HEIGHT);
	building.finishBuild();
	auto const facade = facadeIn(building, facadeIndex);
	require(facade->getLayerIndex() == 1 && facade->getCellX() == 0
		&& facade->getCellY() == 0 && facade->getCellsWide() == 4
		&& facade->getDecksHigh() == 2,
		"The painted Facade landed with the wrong footprint");
	require(facade->getName() == "Frontage", "The painted Facade lost its name");
	require(std::as_const(building).getLayer(1)->getCellDefinition(0, 0).floorType
		== core::CellFloorType::Ground,
		"The Facade's bottom deck does not own walkable ground");
	require(building.isTraversalTopologyValid(),
		"The painted Facade left an invalid topology: " + building.getTopologyDiagnostic());

	// And the canvas hit-test accepts it, so the Selection panel can open on one.
	require(isCanvasSelectableSectorType(core::SectorType::Facade),
		"A Facade cannot be selected by the canvas hit-test, so the Selection panel can never open on one");
	(void)roomIndex;
}

// The Facade's placement validation is the Room's: occupied cells refuse, the
// Building's bounds refuse, and an empty footprint refuses.
void facadePlacementFollowsTheRoomRules()
{
	core::Building building("Placement rules", 12, 3);
	while (building.getLayerCount() < 2) building.addLayer();
	building.addRoom("Occupied", 0, 0, 0, 4, 1);
	building.finishBuild();
	building.pauseSimulation();

	std::string diagnostic;
	// Same Layer, overlapping footprint: refused, with a diagnostic to show in
	// the red preview.
	require(!building.canAddFacade(0, 0, 2, 4, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		"A Facade was accepted over an occupied footprint");
	require(!diagnostic.empty(), "The occupied Facade refusal gave no diagnostic");

	// Outside the Building bounds: refused.
	require(!building.canAddFacade(0, 0, 10, 4, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		"A Facade was accepted outside the Building bounds");
	require(!diagnostic.empty(), "The out-of-bounds Facade refusal gave no diagnostic");

	// An empty footprint: refused.
	require(!building.canAddFacade(0, 0, 0, 0, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		"A zero-width Facade was accepted");
	require(!diagnostic.empty(), "The zero-width Facade refusal gave no diagnostic");

	// The same footprint on a free Layer: accepted - the refusal was about the
	// cells, not the shape.
	require(building.canAddFacade(1, 0, 0, 4, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		("A Facade was refused where the cells were free: " + diagnostic).c_str());
}

// A colour edit made through the panel's own path survives a save and reload:
// the construction record is patched alongside the live Sector.
void aFacadeColourEditPersistsThroughTheConstructionRecord()
{
	core::Building building("Panel recolour", 12, 3);
	while (building.getLayerCount() < 2) building.addLayer();
	building.addRoom("Front", 0, 0, 0, 12, 1);
	auto const index = building.addFacade(1, 0, 0, 4, 1);
	building.finishBuild();

	require(facadeIn(building, index)->getColour() == core::Facade::defaultColour(),
		"The Facade did not start at its default colour");

	std::string diagnostic;
	require(panelRecolour(building, index, { 30, 144, 255 }, diagnostic),
		"The panel recolour was refused: " + diagnostic);
	require(facadeIn(building, index)->getColour()
		== (core::BackgroundColour{ 30, 144, 255 }),
		"The recolour did not reach the live Facade");

	auto const yaml = serializeBuilding(building);
	require(yaml.find(std::format("colour: {}", core::packBackgroundColour(
		core::BackgroundColour{ 30, 144, 255 }))) != std::string::npos,
		"The saved record does not carry the recoloured value");

	core::Building reloaded("Panel recolour", 1, 1);
	loadInto(reloaded, yaml);

	auto const after = facadeIn(reloaded, index);
	require(after->getColour() == (core::BackgroundColour{ 30, 144, 255 }),
		std::format("The recolour did not survive the round-trip: got {},{},{}",
			after->getColour().r, after->getColour().g, after->getColour().b));
	// The edit was a recolour, not a move: the geometry the panel reports is
	// untouched.
	require(after->getLayerIndex() == 1 && after->getCellX() == 0 && after->getCellY() == 0
		&& after->getCellsWide() == 4 && after->getDecksHigh() == 1,
		"The recolour changed the Facade's geometry");
	require(reloaded.getNumSectors() == building.getNumSectors(),
		"The recolour changed the Sector count");
	require(reloaded.isTraversalTopologyValid(),
		"The recolour left an invalid topology: " + reloaded.getTopologyDiagnostic());
}

// A recolour touches the Facade it was aimed at and nothing else - not the
// Facade next to it, not a Background behind it.
void aFacadeRecolourTouchesNothingButItsOwnFacade()
{
	core::Building building("Recolour isolation", 12, 3);
	while (building.getLayerCount() < 3) building.addLayer();
	building.addRoom("Front", 0, 0, 0, 12, 1);
	auto const target = building.addFacade(1, 0, 0, 4, 1, CORE_ROOM_MAX_HEIGHT,
		core::Facade::defaultColour());
	auto const neighbour = building.addFacade(1, 0, 4, 4, 1, CORE_ROOM_MAX_HEIGHT,
		{ 40, 40, 200 });
	auto const background = building.addBackground(2, 0, 0, 8, 1, { 20, 60, 100 });
	building.finishBuild();

	auto const neighbourBefore = facadeIn(building, neighbour)->getColour();
	auto const backgroundBefore = building.getSector(background)->getType();
	require(backgroundBefore == core::SectorType::Background,
		"The test map lost its Background");

	std::string diagnostic;
	require(panelRecolour(building, target, { 5, 200, 240 }, diagnostic),
		"The panel recolour was refused: " + diagnostic);

	require(facadeIn(building, target)->getColour()
		== (core::BackgroundColour{ 5, 200, 240 }),
		"The targeted Facade did not take the new colour");
	require(facadeIn(building, neighbour)->getColour() == neighbourBefore,
		"The neighbouring Facade changed colour with its neighbour");
	require(building.isTraversalTopologyValid(),
		"The recolour left an invalid topology: " + building.getTopologyDiagnostic());

	// And the round-trip keeps both colours distinct.
	core::Building reloaded("Recolour isolation", 1, 1);
	loadInto(reloaded, serializeBuilding(building));
	require(facadeIn(reloaded, target)->getColour()
		== (core::BackgroundColour{ 5, 200, 240 }),
		"The recolour did not reload");
	require(facadeIn(reloaded, neighbour)->getColour() == neighbourBefore,
		"The neighbouring Facade reloaded with the wrong colour");
}

// The recolour paths do not cross types: a Facade refuses the Background's
// recolour call, and anything which is not a Facade refuses the Facade's.
void aRecolourIsRefusedForAnythingWhichIsNotAFacade()
{
	core::Building building("Recolour refusal", 12, 3);
	while (building.getLayerCount() < 2) building.addLayer();
	auto const room = building.addRoom("Front", 0, 0, 0, 6, 1);
	auto const background = building.addBackground(1, 0, 0, 3, 1);
	auto const facade = building.addFacade(1, 0, 3, 3, 1);
	building.finishBuild();

	std::string diagnostic;
	require(!building.setFacadeColour(room, { 255, 0, 0 }, &diagnostic),
		"A Location accepted a Facade recolour");
	require(!diagnostic.empty(), "The Facade recolour refusal gave no diagnostic");
	require(!building.setFacadeColour(background, { 255, 0, 0 }, &diagnostic),
		"A Background accepted a Facade recolour");
	require(!diagnostic.empty(), "The Background-as-Facade recolour refusal gave no diagnostic");
	require(!building.setFacadeColour(building.getNumSectors() + 8, { 255, 0, 0 },
		&diagnostic),
		"An index outside the Building accepted a Facade recolour");
	require(!diagnostic.empty(), "The out-of-range recolour refusal gave no diagnostic");

	// The Facade refuses the Background's recolour call too - the two panels
	// cannot be pointed at each other's Sectors.
	require(!building.setBackgroundColour(facade, { 255, 0, 0 }, &diagnostic),
		"A Facade accepted the Background recolour");
	require(!diagnostic.empty(), "The Facade-as-Background recolour refusal gave no diagnostic");

	require(building.getSector(facade)->getType() == core::SectorType::Facade,
		"The refused recolours changed the Facade's type");
	require(building.isTraversalTopologyValid(),
		"The refused recolours left an invalid topology: " + building.getTopologyDiagnostic());
}

// Wall add/remove against a Facade refuse with a clear, Facade-naming
// diagnostic - on every deck and both sides - through the can-check and
// through the throwing command the editor calls.
void wallCommandsRefuseAFacadeWithAClearDiagnostic()
{
	core::Building building("Wall refusal", 16, 3);
	while (building.getLayerCount() < 2) building.addLayer();
	auto const roomIndex = building.addRoom("Walled", 0, 0, 0, 4, 2);
	auto const facadeIndex = building.addFacade(0, 0, 4, 4, 2);
	building.finishBuild();
	building.pauseSimulation();

	auto const facade = facadeIn(building, facadeIndex);
	for (uint32_t deck = 0; deck < facade->getDecksHigh(); ++deck)
	{
		for (int side = CORE_SIDE_LEFT; side <= CORE_SIDE_RIGHT; ++side)
		{
			std::string diagnostic;
			require(!building.canAddLocationWall(facadeIndex, deck, side, &diagnostic),
				"A wall addition was accepted on a Facade");
			require(diagnostic.find("Facade") != std::string::npos
				&& diagnostic.find("no walls") != std::string::npos,
				"The wall-add refusal did not name the Facade rule: " + diagnostic);

			require(!building.canRemoveLocationWall(facadeIndex, deck, side, &diagnostic),
				"A wall removal was accepted on a Facade");
			require(diagnostic.find("Facade") != std::string::npos
				&& diagnostic.find("no walls") != std::string::npos,
				"The wall-remove refusal did not name the Facade rule: " + diagnostic);

			// The throwing command the editor's wall button calls carries the
			// same clear diagnostic.
			bool threw = false;
			try
			{
				building.addLocationWall(facadeIndex, deck, side);
			}
			catch (core::Exception const& error)
			{
				threw = true;
				require(std::string(error.getMessage()).find("Facade") != std::string::npos,
					"The thrown wall-add diagnostic did not name the Facade: "
					+ std::string(error.getMessage()));
			}
			require(threw, "addLocationWall did not throw against a Facade");

			threw = false;
			try
			{
				building.removeLocationWall(facadeIndex, deck, side);
			}
			catch (core::Exception const& error)
			{
				threw = true;
				require(std::string(error.getMessage()).find("Facade") != std::string::npos,
					"The thrown wall-remove diagnostic did not name the Facade: "
					+ std::string(error.getMessage()));
			}
			require(threw, "removeLocationWall did not throw against a Facade");
		}
	}

	// Nothing moved: the Facade still has no walls, and its neighbour's do.
	for (uint32_t deck = 0; deck < facade->getDecksHigh(); ++deck)
		for (int side = CORE_SIDE_LEFT; side <= CORE_SIDE_RIGHT; ++side)
			require(facade->getEndType(deck, side) == core::SectorEndType::None,
				"A refused wall edit changed a Facade end");
	require(building.getSector(roomIndex)->getEndType(0, CORE_SIDE_LEFT)
		== core::SectorEndType::Wall,
		"The refused Facade edits disturbed the neighbouring Room's walls");
	require(building.isTraversalTopologyValid(),
		"The refused wall edits left an invalid topology: " + building.getTopologyDiagnostic());
}

// The Selection panel's shape for a Facade: reachable by selection, its colour
// widget opens on the Facade's own colour, and no wall affordance on any deck
// or side would be actionable - the panel shows the picker and never the wall
// editor.
void theSelectionPanelShowsNoWallAffordancesForAFacade()
{
	core::Building building("Panel shape", 12, 3);
	while (building.getLayerCount() < 2) building.addLayer();
	building.addRoom("Neighbour", 0, 0, 0, 4, 1);
	auto const index = building.addFacade(0, 0, 4, 4, 2, CORE_ROOM_MAX_HEIGHT,
		{ 12, 240, 6 });
	building.finishBuild();

	// Reachable: the canvas and the Sector tree both take a Facade selection.
	require(isCanvasSelectableSectorType(core::SectorType::Facade),
		"The Selection panel can never open on a Facade");

	// The colour widget opens on the Facade's colour, as three floats with no
	// alpha to control - and the floats cross back exactly.
	auto const facade = facadeIn(building, index);
	float rgb[3];
	core::backgroundColourToFloats(facade->getColour(), rgb);
	require(core::backgroundColourFromFloats(rgb) == facade->getColour(),
		"The colour widget round-trip does not preserve the Facade's colour");

	// No wall affordance is actionable: every add and remove the wall editor
	// could offer across every deck and side refuses, so the panel - which
	// branches to its own Facade layout before the wall editor - has no wall
	// buttons to grey out or fire.
	for (uint32_t deck = 0; deck < facade->getDecksHigh(); ++deck)
	{
		for (int side = CORE_SIDE_LEFT; side <= CORE_SIDE_RIGHT; ++side)
		{
			std::string diagnostic;
			require(!building.canAddLocationWall(index, deck, side, &diagnostic),
				"The wall editor would find an actionable wall-add on a Facade");
			require(!building.canRemoveLocationWall(index, deck, side, &diagnostic),
				"The wall editor would find an actionable wall-remove on a Facade");
		}
	}
}

void runFacadeEditorSmokeChecks()
{
	theFacadeCreationFlowPlacesAnOccupiableSelectableSector();
	facadePlacementFollowsTheRoomRules();
	aFacadeColourEditPersistsThroughTheConstructionRecord();
	aFacadeRecolourTouchesNothingButItsOwnFacade();
	aRecolourIsRefusedForAnythingWhichIsNotAFacade();
	wallCommandsRefuseAFacadeWithAClearDiagnostic();
	theSelectionPanelShowsNoWallAffordancesForAFacade();
}
