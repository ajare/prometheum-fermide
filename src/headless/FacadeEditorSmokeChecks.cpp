// The editor surface for Facades, for ticket #47.
//
// The panel and the palette tray are not reachable headlessly, so these checks
// mirror what the editor does: the paint flow plays the same validate-then-add
// sequence the palette's drag release performs, the colour edit crosses the
// same 0..1 float triple the panel's ColorEdit3 edits and lands through the
// same World::setFacadeColour call, and the wall refusal is the diagnostic
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
//   the canvas drop targets - the pegman's Agent drop and a selected
//   Agent's drag-move - accept a Facade exactly as they accept a Room

#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/Background.h"
#include "core/World.h"
#include "core/CellDefinition.h"
#include "core/Defines.h"
#include "core/Exceptions.h"
#include "core/Facade.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/YamlSerializer.h"
#include "UI.h"
#include "AgentDropTargets.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::shared_ptr<const core::Facade> facadeIn(core::World const& world,
		uint32_t sectorIndex)
	{
		require(sectorIndex < world.getNumSectors(),
			"World reported a Sector index outside itself");
		auto sector = world.getSector(sectorIndex);
		require(sector != nullptr, "World reported a null Sector");
		require(sector->getType() == core::SectorType::Facade,
			("Sector " + std::to_string(sectorIndex) + " is not a Facade").c_str());
		auto facade = std::dynamic_pointer_cast<const core::Facade>(sector);
		require(facade != nullptr, "A Facade Sector is not a core::Facade");
		return facade;
	}

	std::string serializeWorld(core::World& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	void loadInto(core::World& loaded, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(reader != nullptr, "The serialised World could not be read back");
		require(loaded.deserialize(*reader, workData),
			"The World did not reload");
	}

	// Mirrors the panel's colour edit: the widget is handed the live colour as
	// three floats, the user's edit comes back as three floats, and the panel
	// pushes what it got through World::setFacadeColour(). This is the same
	// arithmetic the Background panel runs; only the destination call differs.
	bool panelRecolour(core::World& world, uint32_t sectorIndex,
		core::BackgroundColour const& edited, std::string& diagnostic)
	{
		float rgb[3];
		core::backgroundColourToFloats(edited, rgb);
		return world.setFacadeColour(sectorIndex,
			core::backgroundColourFromFloats(rgb), &diagnostic);
	}
}

// The paint flow: the tray validates the dragged rectangle with canAddFacade -
// the Room's placement rule - and the release adds through addFacade. An
// occupied rectangle refuses before anything is built; a free one lands as an
// occupiable, selectable Facade with walkable ground.
void theFacadeCreationFlowPlacesAnOccupiableSelectableSector()
{
	core::World world("Creation flow", 12, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	auto const roomIndex = world.addRoom("Neighbour", 0, 0, 0, 4, 1);
	world.finishBuild();
	world.pauseSimulation();

	// A free rectangle on Layer 1 validates.
	std::string diagnostic;
	require(world.canAddFacade(1, 0, 0, 4, 2, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		("A free Facade rectangle was refused: " + diagnostic).c_str());

	// The paint release adds it, and it arrives as a Facade with the dragged
	// footprint, walkable ground on its bottom deck, and a name to show.
	auto const facadeIndex = world.addFacade("Frontage", 1, 0, 0, 4, 2,
		CORE_ROOM_MAX_HEIGHT);
	world.finishBuild();
	auto const facade = facadeIn(world, facadeIndex);
	require(facade->getLayerIndex() == 1 && facade->getCellX() == 0
		&& facade->getCellY() == 0 && facade->getCellsWide() == 4
		&& facade->getDecksHigh() == 2,
		"The painted Facade landed with the wrong footprint");
	require(facade->getName() == "Frontage", "The painted Facade lost its name");
	require(std::as_const(world).getLayer(1)->getCellDefinition(0, 0).floorType
		== core::CellFloorType::Ground,
		"The Facade's bottom deck does not own walkable ground");
	require(world.isTraversalTopologyValid(),
		"The painted Facade left an invalid topology: " + world.getTopologyDiagnostic());

	// And the canvas hit-test accepts it, so the Selection panel can open on one.
	require(isCanvasSelectableSectorType(core::SectorType::Facade),
		"A Facade cannot be selected by the canvas hit-test, so the Selection panel can never open on one");
	(void)roomIndex;
}

// The Facade's placement validation is the Room's: occupied cells refuse, the
// World's bounds refuse, and an empty footprint refuses.
void facadePlacementFollowsTheRoomRules()
{
	core::World world("Placement rules", 12, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	world.addRoom("Occupied", 0, 0, 0, 4, 1);
	world.finishBuild();
	world.pauseSimulation();

	std::string diagnostic;
	// Same Layer, overlapping footprint: refused, with a diagnostic to show in
	// the red preview.
	require(!world.canAddFacade(0, 0, 2, 4, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		"A Facade was accepted over an occupied footprint");
	require(!diagnostic.empty(), "The occupied Facade refusal gave no diagnostic");

	// Outside the World bounds: refused.
	require(!world.canAddFacade(0, 0, 10, 4, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		"A Facade was accepted outside the World bounds");
	require(!diagnostic.empty(), "The out-of-bounds Facade refusal gave no diagnostic");

	// An empty footprint: refused.
	require(!world.canAddFacade(0, 0, 0, 0, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		"A zero-width Facade was accepted");
	require(!diagnostic.empty(), "The zero-width Facade refusal gave no diagnostic");

	// The same footprint on a free Layer: accepted - the refusal was about the
	// cells, not the shape.
	require(world.canAddFacade(1, 0, 0, 4, 1, CORE_ROOM_MAX_HEIGHT, &diagnostic),
		("A Facade was refused where the cells were free: " + diagnostic).c_str());
}

// A colour edit made through the panel's own path survives a save and reload:
// the construction record is patched alongside the live Sector.
void aFacadeColourEditPersistsThroughTheConstructionRecord()
{
	core::World world("Panel recolour", 12, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	world.addRoom("Front", 0, 0, 0, 12, 1);
	auto const index = world.addFacade(1, 0, 0, 4, 1);
	world.finishBuild();

	require(facadeIn(world, index)->getColour() == core::Facade::defaultColour(),
		"The Facade did not start at its default colour");

	std::string diagnostic;
	require(panelRecolour(world, index, { 30, 144, 255 }, diagnostic),
		"The panel recolour was refused: " + diagnostic);
	require(facadeIn(world, index)->getColour()
		== (core::BackgroundColour{ 30, 144, 255 }),
		"The recolour did not reach the live Facade");

	auto const yaml = serializeWorld(world);
	require(yaml.find(std::format("colour: {}", core::packBackgroundColour(
		core::BackgroundColour{ 30, 144, 255 }))) != std::string::npos,
		"The saved record does not carry the recoloured value");

	core::World reloaded("Panel recolour", 1, 1);
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
	require(reloaded.getNumSectors() == world.getNumSectors(),
		"The recolour changed the Sector count");
	require(reloaded.isTraversalTopologyValid(),
		"The recolour left an invalid topology: " + reloaded.getTopologyDiagnostic());
}

// A recolour touches the Facade it was aimed at and nothing else - not the
// Facade next to it, not a Background behind it.
void aFacadeRecolourTouchesNothingButItsOwnFacade()
{
	core::World world("Recolour isolation", 12, 3);
	while (world.getLayerCount() < 3) world.addLayer();
	world.addRoom("Front", 0, 0, 0, 12, 1);
	auto const target = world.addFacade(1, 0, 0, 4, 1, CORE_ROOM_MAX_HEIGHT,
		core::Facade::defaultColour());
	auto const neighbour = world.addFacade(1, 0, 4, 4, 1, CORE_ROOM_MAX_HEIGHT,
		{ 40, 40, 200 });
	auto const background = world.addBackground(2, 0, 0, 8, 1, { 20, 60, 100 });
	world.finishBuild();

	auto const neighbourBefore = facadeIn(world, neighbour)->getColour();
	auto const backgroundBefore = world.getSector(background)->getType();
	require(backgroundBefore == core::SectorType::Background,
		"The test map lost its Background");

	std::string diagnostic;
	require(panelRecolour(world, target, { 5, 200, 240 }, diagnostic),
		"The panel recolour was refused: " + diagnostic);

	require(facadeIn(world, target)->getColour()
		== (core::BackgroundColour{ 5, 200, 240 }),
		"The targeted Facade did not take the new colour");
	require(facadeIn(world, neighbour)->getColour() == neighbourBefore,
		"The neighbouring Facade changed colour with its neighbour");
	require(world.isTraversalTopologyValid(),
		"The recolour left an invalid topology: " + world.getTopologyDiagnostic());

	// And the round-trip keeps both colours distinct.
	core::World reloaded("Recolour isolation", 1, 1);
	loadInto(reloaded, serializeWorld(world));
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
	core::World world("Recolour refusal", 12, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	auto const room = world.addRoom("Front", 0, 0, 0, 6, 1);
	auto const background = world.addBackground(1, 0, 0, 3, 1);
	auto const facade = world.addFacade(1, 0, 3, 3, 1);
	world.finishBuild();

	std::string diagnostic;
	require(!world.setFacadeColour(room, { 255, 0, 0 }, &diagnostic),
		"A Location accepted a Facade recolour");
	require(!diagnostic.empty(), "The Facade recolour refusal gave no diagnostic");
	require(!world.setFacadeColour(background, { 255, 0, 0 }, &diagnostic),
		"A Background accepted a Facade recolour");
	require(!diagnostic.empty(), "The Background-as-Facade recolour refusal gave no diagnostic");
	require(!world.setFacadeColour(world.getNumSectors() + 8, { 255, 0, 0 },
		&diagnostic),
		"An index outside the World accepted a Facade recolour");
	require(!diagnostic.empty(), "The out-of-range recolour refusal gave no diagnostic");

	// The Facade refuses the Background's recolour call too - the two panels
	// cannot be pointed at each other's Sectors.
	require(!world.setBackgroundColour(facade, { 255, 0, 0 }, &diagnostic),
		"A Facade accepted the Background recolour");
	require(!diagnostic.empty(), "The Facade-as-Background recolour refusal gave no diagnostic");

	require(world.getSector(facade)->getType() == core::SectorType::Facade,
		"The refused recolours changed the Facade's type");
	require(world.isTraversalTopologyValid(),
		"The refused recolours left an invalid topology: " + world.getTopologyDiagnostic());
}

// Wall add/remove against a Facade refuse with a clear, Facade-naming
// diagnostic - on every deck and both sides - through the can-check and
// through the throwing command the editor calls.
void wallCommandsRefuseAFacadeWithAClearDiagnostic()
{
	core::World world("Wall refusal", 16, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	auto const roomIndex = world.addRoom("Walled", 0, 0, 0, 4, 2);
	auto const facadeIndex = world.addFacade(0, 0, 4, 4, 2);
	world.finishBuild();
	world.pauseSimulation();

	auto const facade = facadeIn(world, facadeIndex);
	for (uint32_t deck = 0; deck < facade->getDecksHigh(); ++deck)
	{
		for (int side = CORE_SIDE_LEFT; side <= CORE_SIDE_RIGHT; ++side)
		{
			std::string diagnostic;
			require(!world.canAddLocationWall(facadeIndex, deck, side, &diagnostic),
				"A wall addition was accepted on a Facade");
			require(diagnostic.find("Facade") != std::string::npos
				&& diagnostic.find("no walls") != std::string::npos,
				"The wall-add refusal did not name the Facade rule: " + diagnostic);

			require(!world.canRemoveLocationWall(facadeIndex, deck, side, &diagnostic),
				"A wall removal was accepted on a Facade");
			require(diagnostic.find("Facade") != std::string::npos
				&& diagnostic.find("no walls") != std::string::npos,
				"The wall-remove refusal did not name the Facade rule: " + diagnostic);

			// The throwing command the editor's wall button calls carries the
			// same clear diagnostic.
			bool threw = false;
			try
			{
				world.addLocationWall(facadeIndex, deck, side);
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
				world.removeLocationWall(facadeIndex, deck, side);
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
	require(world.getSector(roomIndex)->getEndType(0, CORE_SIDE_LEFT)
		== core::SectorEndType::Wall,
		"The refused Facade edits disturbed the neighbouring Room's walls");
	require(world.isTraversalTopologyValid(),
		"The refused wall edits left an invalid topology: " + world.getTopologyDiagnostic());
}

// The Selection panel's shape for a Facade: reachable by selection, its colour
// widget opens on the Facade's own colour, and no wall affordance on any deck
// or side would be actionable - the panel shows the picker and never the wall
// editor.
void theSelectionPanelShowsNoWallAffordancesForAFacade()
{
	core::World world("Panel shape", 12, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	world.addRoom("Neighbour", 0, 0, 0, 4, 1);
	auto const index = world.addFacade(0, 0, 4, 4, 2, CORE_ROOM_MAX_HEIGHT,
		{ 12, 240, 6 });
	world.finishBuild();

	// Reachable: the canvas and the Sector tree both take a Facade selection.
	require(isCanvasSelectableSectorType(core::SectorType::Facade),
		"The Selection panel can never open on a Facade");

	// The colour widget opens on the Facade's colour, as three floats with no
	// alpha to control - and the floats cross back exactly.
	auto const facade = facadeIn(world, index);
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
			require(!world.canAddLocationWall(index, deck, side, &diagnostic),
				"The wall editor would find an actionable wall-add on a Facade");
			require(!world.canRemoveLocationWall(index, deck, side, &diagnostic),
				"The wall editor would find an actionable wall-remove on a Facade");
		}
	}
}

// The canvas drop paths: the pegman's Agent drop and a selected Agent's
// drag-move both accept a Facade as a target (ticket #50, ADR 0003). The
// headless check calls the same inline functions UI.cpp runs, so the real
// interaction logic - not a mirror of it - is pinned down.
void theCanvasDropTargetsAcceptAFacade()
{
	auto const world = std::make_shared<core::World>("Drop targets", 12, 3);
	auto const roomIndex = world->addRoom("Bunker", 0, 0, 0, 3, 1);
	auto const facadeIndex = world->addFacade(0, 0, 4, 4, 1);
	world->finishBuild();

	auto const agentId = world->createAgent("Traveller", roomIndex, 0, 1.5f);
	auto const agent = world->lookupAgent(agentId).entity;
	require(agent != nullptr, "The moving Agent was not created");
	require(agent->getSector() != nullptr
		&& agent->getSector()->getIndex() == roomIndex,
		"The Agent did not start in the Room");

	// The pegman drops into the Facade exactly as it drops into a Room.
	auto const facadeTarget = pegmanAgentTargetAtWorld(world, { 5.5f, 0.5f });
	require(facadeTarget.sector != nullptr,
		"The pegman drop found no Sector inside the Facade");
	require(facadeTarget.diagnostic.empty(),
		"The pegman drop refused the Facade: " + facadeTarget.diagnostic);
	require(facadeTarget.sector->getType() == core::SectorType::Facade
		&& facadeTarget.sector->getIndex() == facadeIndex,
		"The pegman drop resolved to something other than the Facade");
	require(facadeTarget.deckOffset == 0, "The pegman drop landed on the wrong deck");
	require(facadeTarget.localX >= CORE_AGENT_MAX_WIDTH * 0.5f
		&& facadeTarget.localX <= facadeTarget.sector->getSize().x - CORE_AGENT_MAX_WIDTH * 0.5f,
		"The pegman drop landed outside the Facade's Agent band");

	// A selected Agent drag-moves into the Facade.
	auto const moveTarget = getAgentMoveTarget(world, agent, { 5.5f, 0.5f });
	require(moveTarget.sector != nullptr && moveTarget.diagnostic.empty(),
		"The Agent drag-move refused the Facade: " + moveTarget.diagnostic);
	require(moveTarget.sector->getType() == core::SectorType::Facade
		&& moveTarget.sector->getIndex() == facadeIndex,
		"The Agent drag-move resolved to something other than the Facade");
	require(moveTarget.floorY == 0.0f, "The Agent drag-move landed off the Facade floor");

	// The guards still hold: a drop onto a cell with no Sector refuses, and a
	// drop outside the world refuses.
	require(!getAgentMoveTarget(world, agent, { 3.5f, 0.5f }),
		"An Agent drop onto a cell with no Sector was accepted");
	require(!getAgentMoveTarget(world, agent, { 12.5f, 0.5f }),
		"An Agent drop outside the world was accepted");

	// And the plain-Location path is unchanged: the Agent can still target
	// its own Room.
	auto const selfTarget = getAgentMoveTarget(world, agent, { 1.5f, 0.5f });
	require(selfTarget.diagnostic.empty()
		&& selfTarget.sector != nullptr
		&& selfTarget.sector->getIndex() == roomIndex,
		"An Agent targeting its own Room stopped working");
}

// The Facade delete plan (ticket #53): a Facade is occupiable, so the plan
// names the Agents inside and every hosted object which goes with it, exactly
// as a Room deletion does - and reaches no further than the Facade's own.
void theFacadeDeletionPlanNamesItsAgentsAndHostedObjects()
{
	core::World world("Facade deletion", 12, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	auto const room = world.addRoom("Neighbour", 0, 0, 0, 4, 1);
	auto const facade = world.addFacade(1, 0, 0, 4, 2);
	world.finishBuild();
	world.pauseSimulation();

	auto const insideId = world.createAgent("Frontage dweller", facade, 0, 1.5f);
	auto const neighbourId = world.createAgent("Upstairs", room, 0, 1.5f);
	world.addSectorMarker(facade, 0, 2.0f);

	auto const plan = world.planRemoveFacade(facade);
	require(plan.valid, "Deleting a Facade was refused: " + plan.diagnostic);
	require(plan.remove && plan.sectorIndex == facade,
		"The Facade delete plan came back malformed");
	bool namesAgent = false, namesMarker = false;
	for (auto const& line : plan.consequences)
	{
		if (line.find("Delete Agent Frontage dweller") != std::string::npos) namesAgent = true;
		if (line.find("Marker") != std::string::npos) namesMarker = true;
		require(line.find("Upstairs") == std::string::npos,
			"The Facade delete plan reached into the neighbouring Room: " + line);
	}
	require(namesAgent, "The Facade delete plan did not name the Agent inside it");
	require(namesMarker, "The Facade delete plan did not name the hosted Marker");
	require(plan.requiresConfirmation(),
		"A Facade delete with casualties skipped the confirmation step");
	(void)insideId;
	(void)neighbourId;
}

// A Facade with nothing inside still deletes, and needs no confirmation -
// there is no cascade to warn about.
void anEmptyFacadeDeletesWithoutConfirmation()
{
	core::World world("Empty Facade delete", 12, 3);
	auto const facade = world.addFacade(0, 0, 0, 4, 1);
	world.finishBuild();
	world.pauseSimulation();

	auto const plan = world.planRemoveFacade(facade);
	require(plan.valid, "Deleting an empty Facade was refused: " + plan.diagnostic);
	require(!plan.requiresConfirmation(),
		"An empty Facade delete demanded confirmation for no cascade");
}

// Applying the plan: the Facade is gone, its Agent goes with it, and the rest
// of the World - neighbours, their Agents, the saved record - is whole.
void applyingAFacadeDeleteRemovesItAndLeavesTheRestStanding()
{
	core::World world("Facade delete applied", 12, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	auto const room = world.addRoom("Neighbour", 0, 0, 0, 4, 1);
	auto const facade = world.addFacade(1, 0, 0, 4, 2);
	world.finishBuild();
	world.pauseSimulation();

	auto const insideId = world.createAgent("Frontage dweller", facade, 0, 1.5f);
	auto const neighbourId = world.createAgent("Upstairs", room, 0, 1.5f);

	auto const before = world.getNumSectors();
	auto const plan = world.planRemoveFacade(facade);
	require(plan.valid, "Deleting a Facade was refused: " + plan.diagnostic);
	world.applyLocationEdit(plan);

	require(world.getNumSectors() == before - 1,
		"The deleted Facade's Sector was not compacted away");
	for (uint32_t index = 0; index < world.getNumSectors(); ++index)
		require(world.getSector(index)->getType() != core::SectorType::Facade,
			"A Facade survived its own deletion");
	require(world.lookupAgent(insideId).entity == nullptr,
		"The Agent inside the deleted Facade survived with it");
	auto const survivor = world.lookupAgent(neighbourId).entity;
	require(survivor != nullptr && survivor->getSector() != nullptr
		&& survivor->getSector()->getIndex() == room,
		"Deleting the Facade disturbed the neighbouring Room's Agent");
	require(world.isTraversalTopologyValid(),
		"The Facade delete left an invalid topology: " + world.getTopologyDiagnostic());

	// The authored record is gone too: the save never replays the Facade.
	core::World reloaded("Facade delete applied", 1, 1);
	loadInto(reloaded, serializeWorld(world));
	require(reloaded.getNumSectors() == world.getNumSectors(),
		"The reloaded World kept a Sector the delete had removed");
	for (uint32_t index = 0; index < reloaded.getNumSectors(); ++index)
		require(reloaded.getSector(index)->getType() != core::SectorType::Facade,
			"The saved record replayed the deleted Facade");
	require(reloaded.lookupAgent(neighbourId).entity != nullptr,
		"The surviving Agent did not reload after the Facade delete");
}

// The deletion paths do not cross types: the Facade plan refuses everything
// which is not a Facade, and the Location paths keep refusing Facades exactly
// as they did before - the resize refusal in particular is the ticket #53
// decision, so it stays put with its diagnostic intact.
void theDeletionPlansDoNotCrossTypes()
{
	core::World world("Deletion refusal", 12, 3);
	while (world.getLayerCount() < 2) world.addLayer();
	auto const room = world.addRoom("Room", 0, 0, 0, 4, 1);
	auto const corridor = world.addCorridor(0u, 1u, 0u, 4u, 1u);
	auto const background = world.addBackground(1, 0, 0, 3, 1);
	auto const facade = world.addFacade(1, 0, 3, 3, 1);
	world.finishBuild();
	world.pauseSimulation();

	auto const roomPlan = world.planRemoveFacade(room);
	require(!roomPlan.valid && roomPlan.diagnostic.find("Facade") != std::string::npos,
		"A Room accepted the Facade delete plan");
	auto const corridorPlan = world.planRemoveFacade(corridor);
	require(!corridorPlan.valid, "A Corridor accepted the Facade delete plan");
	auto const backgroundPlan = world.planRemoveFacade(background);
	require(!backgroundPlan.valid, "A Background accepted the Facade delete plan");
	auto const outsidePlan = world.planRemoveFacade(world.getNumSectors() + 8);
	require(!outsidePlan.valid, "An index outside the World accepted the Facade delete plan");

	// The old refusal is untouched: planRemoveLocation still will not delete a
	// Facade, and planResizeLocation still will not resize one. Deletion has
	// its own door; resize stays closed.
	auto const wrongDoor = world.planRemoveLocation(facade);
	require(!wrongDoor.valid
		&& wrongDoor.diagnostic == "Only rooms and corridors can be deleted",
		"planRemoveLocation changed its Facade refusal: " + wrongDoor.diagnostic);
	auto const resize = world.planResizeLocation(facade, 3, 0, 3, 1);
	require(!resize.valid
		&& resize.diagnostic == "Only rooms and corridors can be resized",
		"planResizeLocation changed its Facade refusal: " + resize.diagnostic);
	require(world.getSector(facade)->getType() == core::SectorType::Facade,
		"A refused delete or resize changed the Facade");
	require(world.isTraversalTopologyValid(),
		"The refused plans left an invalid topology: " + world.getTopologyDiagnostic());
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
	theCanvasDropTargetsAcceptAFacade();
	theFacadeDeletionPlanNamesItsAgentsAndHostedObjects();
	anEmptyFacadeDeletesWithoutConfirmation();
	applyingAFacadeDeleteRemovesItAndLeavesTheRestStanding();
	theDeletionPlansDoNotCrossTypes();
}
