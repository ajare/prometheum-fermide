#define NOMINMAX
#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#include <fstream>
#else
#error "Unsupported platform"
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "core/Agent.h"
#include "core/World.h"
#include "core/Button.h"
#include "core/GapEdge.h"
#include "core/Graph.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LiftTransit.h"
#include "core/LiftSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/DoorVertex.h"
#include "core/LadderSectorObject.h"
#include "core/Path.h"
#include "core/SectorEdge.h"
#include "core/Simulation.h"
#include "core/Staircase.h"
#include "core/Transit.h"
#include "core/Vector2.h"

#ifdef _MSC_VER
#pragma comment(lib, "Psapi.lib")
#endif

void runSerializationSmokeChecks();
void runAgentGroupSmokeChecks();
void runAgentGroupAssignmentSmokeChecks();
void runAgentGroupCountSmokeChecks();
void runAgentGroupDeleteSmokeChecks();
void runAgentGroupIdAllocationSmokeChecks();
void runAgentGroupClipboardSmokeChecks();
void runAgentGroupTopologySmokeChecks();
void runAgentActivationSmokeChecks();
void runMarkerIdentitySmokeChecks();
void runMovementCommandSmokeChecks();
void runShuttleDoorQuerySmokeChecks();
void runShuttleDoorRenderSmokeChecks();
void runRenderOrderSmokeChecks();
void runWallRenderSmokeChecks();
void runDoorOpenApartRenderSmokeChecks();
void runDoorOpenLeftRenderSmokeChecks();
void runDoorOpenRightRenderSmokeChecks();
void runEditorLayerSmokeChecks();
void runWindowLayerSmokeChecks();
void runBackgroundSectorSmokeChecks();
void runBackgroundPaintSmokeChecks();
void runBackgroundPlacementSmokeChecks();
void runBackgroundCascadeDeleteSmokeChecks();
void runBackgroundSelectionPanelSmokeChecks();
void runDoorPanelScopeSmokeChecks();
void runDoorTwoSidedButtonSmokeChecks();
void runDocumentHistorySmokeChecks();
void runAgentTagRegistrySmokeChecks();
void runAgentBehaviourRegistrySmokeChecks();
void runAgentBehaviourAssignmentSmokeChecks();
void runAgentBehaviourPortabilitySmokeChecks();
void runAgentBehaviourDeleteSmokeChecks();
void runAgentBehaviourSchemaReconciliationSmokeChecks();
void runAgentBehaviourRuntimeSmokeChecks();
void runAgentBehaviourWorkflowSmokeChecks();
void runAgentTagRegistryChangeSmokeChecks();
void runAgentTagDocumentSaveSmokeChecks();
void runAgentTagReloadSmokeChecks();
void runAgentTagAssignmentSmokeChecks();
void runAgentTagDeleteSmokeChecks();
void runAgentTagCoordinationSmokeChecks();
void runAgentTagReconciliationSmokeChecks();
void runAgentTagClipboardSmokeChecks();
void runAgentColourSmokeChecks();
void runAgentWalkSpeedSmokeChecks();
void runAgentHeightSmokeChecks();
void runThresholdRefusalSmokeChecks();
void runThresholdLayerOverlapSmokeChecks();
void runWindowIntoBackgroundSmokeChecks();
void runWindowMultiBackgroundSmokeChecks();
void runFacadeSmokeChecks();
void runFacadeRenderSmokeChecks();
void runFacadeDrawOrderSmokeChecks();
void runFacadeEditorSmokeChecks();
void runPaletteTraySmokeChecks();
void runOnboardAgentDeletionSmokeChecks();
void runViewportCullingSmokeChecks();
void runZeroSizeLocationSmokeChecks();
void runIsolatedSectorPathingSmokeChecks();
void runGraphicsStartupSmokeChecks();

static_assert(!std::is_convertible_v<core::AgentId, core::InteractionPointId>);
static_assert(!std::is_convertible_v<core::DeviceOperationId, core::TraversalResourceId>);

namespace
{
	constexpr uint64_t MaximumSimulationTicks = 1000;

	struct ScenarioResult
	{
		bool reachedDestination{ false };
		core::SimulationSnapshot snapshot;
		std::vector<core::SimulationEvent> events;
	};

	void appendAgent(std::ostringstream& output, core::AgentSnapshot const& agent)
	{
		output << agent.id.value << ':' << agent.name << ':' << agent.sectorId.value << ':'
			<< std::bit_cast<uint32_t>(agent.localPosition.x) << ':'
			<< std::bit_cast<uint32_t>(agent.localPosition.y) << ':'
			<< std::bit_cast<uint32_t>(agent.globalPosition.x) << ':'
			<< std::bit_cast<uint32_t>(agent.globalPosition.y) << ':'
			<< (int)agent.state << ':' << agent.hasPath << ':'
			<< agent.targetPathNode << ':' << agent.pathNodeCount;
	}

	void appendTraversalRequest(std::ostringstream& output, core::TraversalRequestSnapshot const& request)
	{
		output << request.id.value << ':' << request.owner.value << ':' << (int)request.edgeType << ':'
			<< request.sourceSector.value << ':' << request.destinationSector.value << ':'
			<< std::bit_cast<uint32_t>(request.sourceEndpoint.x) << ':'
			<< std::bit_cast<uint32_t>(request.sourceEndpoint.y) << ':'
			<< std::bit_cast<uint32_t>(request.destinationEndpoint.x) << ':'
			<< std::bit_cast<uint32_t>(request.destinationEndpoint.y) << ':'
			<< (int)request.state << ':' << request.permit.value << ':' << request.diagnostic;
	}

	std::string canonicalResult(ScenarioResult const& result)
	{
		std::ostringstream output;
		output << result.snapshot.tick << '|';
		for (auto const& agent : result.snapshot.agents)
		{
			appendAgent(output, agent);
			output << '|';
		}

		for (auto const& event : result.events)
		{
			output << event.sequence << ':' << event.tick << ':' << (int)event.type << ':'
				<< (int)event.phase << ':' << event.hasPreviousAgent << ':';
			if (event.hasPreviousAgent)
			{
				appendAgent(output, event.previousAgent);
			}
			output << ':';
			switch (event.type)
			{
			case core::SimulationEventType::AgentAdded:
			case core::SimulationEventType::AgentChanged:
			case core::SimulationEventType::AgentActivated:
			case core::SimulationEventType::AgentDeactivated:
			case core::SimulationEventType::AgentRemoved:
				appendAgent(output, event.agent);
				break;
			case core::SimulationEventType::TraversalRequestAdded:
			case core::SimulationEventType::TraversalRequestChanged:
			case core::SimulationEventType::TraversalRequestRemoved:
				appendTraversalRequest(output, event.traversalRequest);
				break;
			case core::SimulationEventType::TraversalPermitAdded:
			case core::SimulationEventType::TraversalPermitChanged:
			case core::SimulationEventType::TraversalPermitRemoved:
				output << event.traversalPermit.id.value << ':' << event.traversalPermit.request.value
					<< ':' << event.traversalPermit.owner.value << ':' << (int)event.traversalPermit.state;
				break;
			default:
				break;
			}
			output << '|';
		}
		return output.str();
	}

	bool phasesAreOrdered(std::vector<core::SimulationEvent> const& events, uint64_t ticks)
	{
		constexpr core::SimulationPhase expected[] = {
			core::SimulationPhase::ResourceAdvancement,
			core::SimulationPhase::IntentCollection,
			core::SimulationPhase::Allocation,
			core::SimulationPhase::Movement,
			core::SimulationPhase::Commit,
			core::SimulationPhase::CleanupAndEventPublication
		};

		uint64_t phaseEventCount = 0;
		for (auto const& event : events)
		{
			if (event.type != core::SimulationEventType::PhaseCompleted)
			{
				continue;
			}
			if (event.phase != expected[phaseEventCount % std::size(expected)])
			{
				return false;
			}
			++phaseEventCount;
		}
		return phaseEventCount == ticks * std::size(expected);
	}

	bool accumulatedRenderTimeAdvancesWholeTicksOnly()
	{
		core::World world("Accumulator check", 1, 1);
		auto halfTick = core::World::getFixedTimestep() * 0.5f;
		world.update(halfTick);
		if (world.getSimulationTick() != 0)
		{
			return false;
		}
		world.update(halfTick);
		return world.getSimulationTick() == 1;
	}

	bool worldOwnsTypedEntitiesAndInvalidatesHandles()
	{
		core::World world("Ownership check", 3, 2);
		auto corridor = world.addCorridor(0, 0, 2);
		world.finishBuild();

		auto agentId = world.createAgent("Owned idle agent", corridor, 0, 0.5f);
		auto pointId = world.createInteractionPoint("Light switch");
		auto operationId = world.createDeviceOperation("Turn lights on", agentId);
		auto resourceId = world.createTraversalResource("Ordinary passage");

		auto snapshot = world.getSimulationSnapshot();
		if (snapshot.agents.size() != 1 || snapshot.agents.front().id != agentId
			|| snapshot.interactionPoints.size() != 1 || snapshot.interactionPoints.front().id != pointId
			|| snapshot.deviceOperations.size() != 1 || snapshot.deviceOperations.front().id != operationId
			|| snapshot.deviceOperations.front().requester != agentId
			|| snapshot.traversalResources.size() != 1 || snapshot.traversalResources.front().id != resourceId)
		{
			return false;
		}

		if (!world.removeAgent(agentId)
			|| world.lookupAgent(agentId)
			|| world.lookupAgent(agentId).diagnostic.empty()
			|| world.lookupDeviceOperation(operationId)
			|| world.lookupDeviceOperation(operationId).diagnostic.empty())
		{
			return false;
		}
		if (!world.removeInteractionPoint(pointId) || !world.removeTraversalResource(resourceId))
		{
			return false;
		}

		world.advanceTick();
		auto afterRemoval = world.getSimulationSnapshot();
		return afterRemoval.agents.empty()
			&& afterRemoval.interactionPoints.empty()
			&& afterRemoval.deviceOperations.empty()
			&& afterRemoval.traversalResources.empty();
	}

	std::shared_ptr<core::Path> twoNodePath(std::shared_ptr<const core::Vertex> source,
		std::shared_ptr<const core::Vertex> destination, std::shared_ptr<const core::Edge> edge)
	{
		auto path = std::make_shared<core::Path>();
		path->nodes.push_back({ nullptr, std::move(source), 0.0f });
		path->nodes.push_back({ std::move(edge), std::move(destination), 1.0f });
		return path;
	}

	bool inferredPathSourceDoesNotMakeAgentDoubleBack()
	{
		core::World world("Path source selection", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Path source traveller", corridor, 0, 2.5f);
		auto agent = world.lookupAgent(agentId).entity;

		auto inferredPath = world.getGraph()->calculatePath(agent, destination);
		auto explicitPath = world.getGraph()->calculatePath(agent, source, destination);
		return inferredPath && inferredPath->nodes.size() == 1
			&& inferredPath->nodes.front().targetVertex->sameAs(destination)
			&& !inferredPath->nodes.front().edge
			&& inferredPath->nodes.front().edgeWeight == 0.0f
			&& explicitPath && explicitPath->nodes.size() == 2
			&& explicitPath->nodes.front().targetVertex->sameAs(source);
	}

	bool markerPlacementEnforcesPaletteCoreRules()
	{
		core::World world("Marker placement rules", 7, 3);
		auto room = world.addRoom("Marker room", 0, 0, 0, 6, 2);
		world.finishBuild();

		std::string diagnostic;
		if (!world.canAddSectorMarker(room, 0, 2.5f, &diagnostic)
			|| world.canAddSectorMarker(room, 1, 2.5f, &diagnostic)) return false;

		bool runningRejected = false;
		try { world.addSectorMarker(room, 0, 2.5f); }
		catch (std::exception const&) { runningRejected = true; }
		if (!runningRejected || world.isTraversalTopologyDirty()) return false;

		world.pauseSimulation();
		auto created = world.addSectorMarker(room, 0, 2.5f);
		if (created.type != core::SectorObjectType::Marker || created.index == ~0u
			|| world.canAddSectorMarker(room, 0, 2.52f, &diagnostic)
			|| diagnostic.find("already exists") == std::string::npos) return false;

		bool duplicateRejected = false;
		try { world.addSectorMarker(room, 0, 2.52f); }
		catch (std::exception const&) { duplicateRejected = true; }
		return duplicateRejected && world.rebuildTraversalTopology()
			&& world.resumeSimulation() && !world.isSimulationPaused();
	}

	bool corridorDoorPlacementEnforcesPaletteRules()
	{
		// Every Location kind may host either side of a Door. Exercise all nine
		// front/back combinations so Room, Corridor, and Facade stay symmetric.
		for (int frontKind = 0; frontKind < 3; ++frontKind)
			for (int backKind = 0; backKind < 3; ++backKind)
			{
				core::World world("Location Door placement rules", 10, 4);
				auto addLocation = [&](int kind, uint32_t layer)
				{
					if (kind == 0) return world.addRoom("Room", layer, 0, 0, 6, 1);
					if (kind == 1) return world.addCorridor(layer, 0, 0, 6, 1);
					return world.addFacade(layer, 0, 0, 6, 1);
				};
				addLocation(frontKind, 0);
				addLocation(backKind, 1);

				std::string diagnostic;
				if (!world.canAddCorridorDoor(0, 0, 2, &diagnostic)) return false;
				auto door = world.addSectorDoor(0, 0, 2);
				world.finishBuild();
				if (door.door.type != core::SectorObjectType::Door
					|| !world.isTraversalTopologyValid()) return false;
			}

		core::World world("Door obstruction rules", 10, 4);
		auto frontRoom = world.addRoom("Front room", 0, 0, 0, 6, 2);
		auto backRoom = world.addRoom("Back room", 1, 0, 0, 6, 1);
		std::string diagnostic;
		world.addSectorMarker(frontRoom, 0, 4.5f);
		world.addSectorMarker(backRoom, 0, 0.5f);
		if (world.canAddCorridorDoor(0, 0, 4, &diagnostic)
			|| diagnostic.find("blocks") == std::string::npos
			|| world.canAddCorridorDoor(0, 1, 1, &diagnostic)
			|| diagnostic.find("behind") == std::string::npos) return false;
		return true;
	}

	bool objectMoveValidatesAndRebuildsOnceCommitted()
	{
		core::World world("Object movement", 10, 3);
		auto corridor = world.addCorridor(0, 0, 8);
		world.addRoom("Back room", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.controls[0] = true;
		doorOptions.controls[1] = true;
		doorOptions.activationMode = core::DoorActivationMode::RemoteControlled;
		auto created = world.addSectorDoor(0, 0, 1, doorOptions);
		world.addSectorMarker(corridor, 0, 5.5f);
		world.finishBuild();
		world.pauseSimulation();
		auto agentId = world.createAgent("Stationary", corridor, 0, 0.5f);

		auto outsideBothSectors = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 1, 1);
		auto blocked = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 5, 0);
		auto valid = world.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 3, 0);
		if (outsideBothSectors.valid || blocked.valid || !valid.valid) return false;

		auto moved = world.applyObjectMove(valid);
		if (!moved || moved->getObjectType() != core::SectorObjectType::Door
			|| moved->getCellX() != 3 || moved->getCellY() != 0
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.isSimulationPaused() || !world.isTraversalTopologyValid()) return false;
		auto doorOwner = moved->getSector();
		uint32_t movedDoorIndex = ~0u;
		for (uint32_t i = 0; i < doorOwner->getNumObjects(); ++i)
			if (doorOwner->getObject(i) == moved) { movedDoorIndex = i; break; }
		if (movedDoorIndex == ~0u
			|| !world.removeSectorDoor(doorOwner->getIndex(), movedDoorIndex)
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.getSimulationSnapshot().traversalResources.empty()) return false;
		for (auto const& sector : world.getSectors(0))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = sector->getObject(i))
					if (object->getObjectType() == core::SectorObjectType::Door) return false;

		core::World windowWorld("Window editing", 10, 3);
		auto fore = windowWorld.addRoom("Fore", 0, 0, 0, 8, 1);
		windowWorld.addRoom("Back", 1, 0, 0, 8, 1);
		auto createdWindow = windowWorld.addSectorWindow(0, 0, 1, 1, 1, {});
		windowWorld.finishBuild();
		windowWorld.pauseSimulation();
		auto windowAgent = windowWorld.createAgent("Stationary", fore, 0, 0.5f);
		auto windowMove = windowWorld.planMoveSectorObject(
			createdWindow.window.sector->getIndex(), createdWindow.window.index, 3, 0);
		if (!windowMove.valid) return false;
		auto movedWindow = windowWorld.applyObjectMove(windowMove);
		if (!movedWindow || movedWindow->getObjectType() != core::SectorObjectType::Window
			|| movedWindow->getCellX() != 3) return false;
		auto owner = movedWindow->getSector();
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == movedWindow) { movedIndex = i; break; }
		if (movedIndex == ~0u || !windowWorld.removeSectorWindow(owner->getIndex(), movedIndex))
			return false;
		for (auto const& sector : windowWorld.getSectors(0))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = sector->getObject(i))
					if (object->getObjectType() == core::SectorObjectType::Window) return false;
		if (windowWorld.lookupAgent(windowAgent).entity == nullptr
			|| !windowWorld.isSimulationPaused() || !windowWorld.isTraversalTopologyValid()) return false;

		core::World pasteMoveWorld("Paste-style movement", 10, 2);
		pasteMoveWorld.addCorridor(0, 0, 4);
		auto right = pasteMoveWorld.addCorridor(0, 6, 4);
		pasteMoveWorld.addRoom("Left back", 1, 0, 0, 4, 1);
		pasteMoveWorld.addRoom("Right back", 1, 0, 6, 4, 1);
		auto crossSectorDoor = pasteMoveWorld.addSectorDoor(0, 0, 1);
		pasteMoveWorld.finishBuild();
		pasteMoveWorld.pauseSimulation();
		auto doorPlan = pasteMoveWorld.planMoveSectorObject(
			crossSectorDoor.door.sector->getIndex(), crossSectorDoor.door.index, 7, 0);
		if (!doorPlan.valid) return false;
		auto movedAcrossSectors = pasteMoveWorld.applyObjectMove(doorPlan);
		if (!movedAcrossSectors || movedAcrossSectors->getSector()->getIndex() != right) return false;

		core::World markerMoveWorld("Marker movement", 10, 1);
		auto markerLeft = markerMoveWorld.addCorridor(0, 0, 4);
		auto markerRight = markerMoveWorld.addCorridor(0, 6, 4);
		auto marker = markerMoveWorld.addSectorMarker(markerLeft, 0, 2.5f);
		markerMoveWorld.finishBuild();
		markerMoveWorld.pauseSimulation();
		auto markerPlan = markerMoveWorld.planMoveSectorObject(markerLeft, marker.index, 8, 0);
		if (!markerPlan.valid) return false;
		auto movedMarker = markerMoveWorld.applyObjectMove(markerPlan);
		return movedMarker && movedMarker->getSector()->getIndex() == markerRight
			&& movedMarker->getCellX() == 8;
	}

	bool windowResizeUsesWindowPlacementRules()
	{
		core::World world("Window resizing", 12, 3);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 3);
		world.addRoom("Front neighbour", 0, 0, 8, 4, 3);
		world.addRoom("Behind", 1, 0, 0, 12, 3);
		core::World::CreateWindowOptions options;
		options.traversable = true;
		options.initialState = core::Window::State::Tinted;
		options.style = core::Window::Style::Tinted;
		// Palette placement creates a one-cell aperture; resizing does the rest.
		auto created = world.addSectorWindow(0, 0, 3, 1, 1, options);
		if (created.window.sector->getObject(created.window.index)->getSize()
			!= core::Vector2{ 1.0f, 1.0f }) return false;
		world.finishBuild();
		world.pauseSimulation();

		auto findWindowIndex = [](std::shared_ptr<const core::Sector> const& owner,
			std::shared_ptr<const core::SectorObject> const& object)
		{
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == object) return i;
			return ~0u;
		};
		auto resize = [&](std::shared_ptr<const core::SectorObject> const& object,
			uint32_t x, uint32_t y, uint32_t width, uint32_t height)
		{
			auto owner = object->getSector();
			auto index = findWindowIndex(owner, object);
			if (index == ~0u) return std::shared_ptr<const core::SectorObject>{};
			auto plan = world.planResizeSectorWindow(
				owner->getIndex(), index, x, y, width, height);
			return plan.valid ? world.applyObjectMove(plan)
				: std::shared_ptr<const core::SectorObject>{};
		};

		auto resized = resize(created.window.sector->getObject(created.window.index), 1, 0, 3, 1);
		if (!resized || resized->getCellX() != 1
			|| resized->getSize() != core::Vector2{ 3.0f, 1.0f }) return false;
		resized = resize(resized, 1, 0, 6, 1);
		if (!resized || resized->getSize() != core::Vector2{ 6.0f, 1.0f }) return false;

		auto owner = resized->getSector();
		auto index = findWindowIndex(owner, resized);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 0, 1).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 0).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 8, 1).valid
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 12, 1).valid)
			return false;
		world.addSectorMarker(front, 0, 7.5f);
		if (world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 7, 1).valid)
			return false;

		resized = resize(resized, 1, 0, 6, 3);
		if (!resized || resized->getSize() != core::Vector2{ 6.0f, 3.0f }) return false;
		std::string diagnostic;
		if (world.canAddSectorWindow(0, 2, 2, 1, 1, &diagnostic)) return false;
		owner = resized->getSector();
		index = findWindowIndex(owner, resized);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 4).valid)
			return false;
		resized = resize(resized, 1, 1, 6, 2);
		if (!resized || resized->getCellY() != 1
			|| resized->getSize() != core::Vector2{ 6.0f, 2.0f }) return false;

		owner = resized->getSector();
		index = findWindowIndex(owner, resized);
		world.addSectorMarker(front, 0, 2.5f);
		if (index == ~0u
			|| world.planResizeSectorWindow(owner->getIndex(), index, 1, 0, 6, 3).valid)
			return false;

		if (!world.rebuildTraversalTopology()) return false;
		core::World::CreateWindowOptions retained;
		return world.getSectorWindowOptions(0, 1, 1, 6, 2, retained)
			&& retained.traversable && retained.initialState == core::Window::State::Tinted
			&& retained.style == core::Window::Style::Tinted
			&& world.isSimulationPaused() && world.isTraversalTopologyValid();
	}

	bool doorResizeRespectsDoorPlacementRules()
	{
		core::World world("Door resizing", 12, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
		world.addRoom("Front neighbour", 0, 0, 8, 4, 1);
		world.addRoom("Behind", 1, 0, 0, 12, 1);
		core::World::CreateDoorOptions options;
		options.controls[0] = true;
		options.controls[1] = true;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.holdOpenSeconds = 4.5f;
		// Palette placement creates a one-cell Door; resizing does the rest.
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		world.pauseSimulation();
		auto agentId = world.createAgent("Stationary", front, 0, 0.5f);

		auto findDoorIndex = [](std::shared_ptr<const core::Sector> const& owner,
			std::shared_ptr<const core::SectorObject> const& object)
		{
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == object) return i;
			return ~0u;
		};
		auto resize = [&](std::shared_ptr<const core::SectorObject> const& object,
			uint32_t x, uint32_t y, uint32_t width, uint32_t height = 1)
		{
			auto owner = object->getSector();
			auto index = findDoorIndex(owner, object);
			if (index == ~0u) return std::shared_ptr<const core::SectorObject>{};
			auto plan = world.planResizeSectorDoor(owner->getIndex(), index, x, y, width, height);
			return plan.valid ? world.applyObjectMove(plan)
				: std::shared_ptr<const core::SectorObject>{};
		};

		auto resized = resize(created.door.sector->getObject(created.door.index), 2, 0, 2);
		if (!resized || resized->getCellX() != 2
			|| resized->getSize() != core::Vector2{ 2.0f, 1.0f }) return false;

		auto owner = resized->getSector();
		auto index = findDoorIndex(owner, resized);
		if (index == ~0u) return false;
		// A Door is one or two cells wide, never zero and never three.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 0, 1).valid
			|| world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 3, 1).valid)
			return false;
		// Door resizing remains horizontal; its grid footprint is always one level.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 0).valid
			|| world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 2).valid)
			return false;
		// The span may not cross the front Sector boundary into the neighbour.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 7, 0, 2, 1).valid)
			return false;
		// Nor may it grow into a cell another object occupies.
		world.addSectorMarker(front, 0, 4.5f);
		if (world.planResizeSectorDoor(owner->getIndex(), index, 3, 0, 2, 1).valid)
			return false;
		// These rooms are one level tall, so the Door cannot grow upward here.
		if (world.planResizeSectorDoor(owner->getIndex(), index, 2, 0, 2, 2).valid)
			return false;

		if (!world.rebuildTraversalTopology()) return false;
		core::World::CreateDoorOptions retained;
		if (!world.getSectorDoorOptions(0, 0, 2, 2, retained)
			|| retained.activationMode != core::DoorActivationMode::RemoteControlled
			|| retained.holdOpenSeconds != 4.5f
			|| !retained.controls[0] || !retained.controls[1]
			|| world.lookupAgent(agentId).entity == nullptr
			|| !world.isSimulationPaused() || !world.isTraversalTopologyValid())
			return false;

		// Authored crossing lanes outrank a narrower Door: shrinking below them
		// would silently drop capacity, so the plan refuses.
		core::World laneWorld("Lane door resizing", 8, 2);
		laneWorld.addRoom("Fore", 0, 0, 0, 8, 1);
		laneWorld.addRoom("Aft", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions lanes;
		lanes.width = 2;
		lanes.crossingLanes = 2;
		auto laneDoor = laneWorld.addSectorDoor(0, 0, 3, lanes);
		laneWorld.finishBuild();
		laneWorld.pauseSimulation();
		owner = laneDoor.door.sector;
		index = findDoorIndex(owner, laneDoor.door.sector->getObject(laneDoor.door.index));
		if (index == ~0u
			|| laneWorld.planResizeSectorDoor(owner->getIndex(), index, 3, 0, 1, 1).valid)
			return false;

		// Lift landing doors belong to the transport and refuse to resize.
		core::World liftWorld("Lift door resizing", 10, 8);
		liftWorld.addCorridor(1, 0, 8);
		liftWorld.addCorridor(4, 0, 8);
		auto lift = liftWorld.addLift(1, 0, 2, 2, 6);
		liftWorld.finishBuild();
		liftWorld.pauseSimulation();
		if (lift.doors.empty()) return false;
		auto landingDoor = lift.doors[0].door.sector->getObject(lift.doors[0].door.index);
		if (!liftWorld.isLiftOwnedDoor(landingDoor)) return false;
		owner = landingDoor->getSector();
		index = findDoorIndex(owner, landingDoor);
		if (index == ~0u
			|| liftWorld.planResizeSectorDoor(owner->getIndex(), index,
				landingDoor->getCellX(), landingDoor->getCellY(), 1, 1).valid)
			return false;
		return true;
	}

	bool staircasePathSpansOuterCellEdges()
	{
		core::Staircase risingRight(5, 0, 3, CORE_SIDE_RIGHT);
		auto const rightPath = risingRight.getPath();
		core::Staircase risingLeft(5, 0, 3, CORE_SIDE_LEFT);
		auto const leftPath = risingLeft.getPath();
		return rightPath[0] == core::Vector2{ 0.0f, 0.0f }
			&& rightPath[1] == core::Vector2{ 3.0f, 1.0f }
			&& leftPath[0] == core::Vector2{ 3.0f, 0.0f }
			&& leftPath[1] == core::Vector2{ 0.0f, 1.0f };
	}

	bool staircaseCanUseForeRoomEndpoints()
	{
		core::World world("Room staircase landing", 10, 3);
		world.addCorridor(0, 0, 6);
		auto upperCorridor = world.addCorridor(1, 0, 7);
		auto room = world.addRoom("Upper room", 0, 1, 7, 3, 1);

		std::string diagnostic;
		if (world.canAddStaircase(1, 0, 5, 3, CORE_SIDE_RIGHT, &diagnostic)) return false;
		world.removeLocationWall(room, 0, CORE_SIDE_LEFT);
		if (!world.canAddStaircase(1, 0, 5, 3, CORE_SIDE_RIGHT, &diagnostic)) return false;
		auto staircase = world.addStaircase(1, 0, 5,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		world.finishBuild();
		if (staircase == ~0u || !world.isTraversalTopologyValid()) return false;
		world.pauseSimulation();
		auto edit = world.planResizeStaircase(staircase, 5, 0,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		if (!edit.valid
			|| world.getSector(room)->getEndType(0, CORE_SIDE_LEFT) != core::SectorEndType::None
			|| world.getSector(upperCorridor)->getEndType(0, CORE_SIDE_RIGHT) != core::SectorEndType::None)
			return false;

		core::World lowerRoomWorld("Lower Room staircase endpoint", 8, 3);
		lowerRoomWorld.addRoom("Lower room", 0, 0, 0, 3, 1);
		lowerRoomWorld.addCorridor(1, 4, 4);
		if (!lowerRoomWorld.canAddStaircase(1, 0, 2, 3, CORE_SIDE_RIGHT, &diagnostic))
			return false;
		lowerRoomWorld.addStaircase(1, 0, 2,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		lowerRoomWorld.finishBuild();
		if (!lowerRoomWorld.isTraversalTopologyValid()) return false;

		// escalator-test-1.world.yaml: the flight starts on the Room's ground-level floor and
		// reaches its upper-right edge, where the wall into the upper Corridor is open.
		core::World mapWorld("Escalator map Room landing", 16, 3);
		mapWorld.addRoom("Room 1", 0, 1, 10, 4, 2);
		mapWorld.addCorridor(2, 14, 2);
		mapWorld.removeLocationWall(0, 1, CORE_SIDE_RIGHT);
		if (!mapWorld.canAddStaircase(1, 1, 11, 3, CORE_SIDE_RIGHT, &diagnostic))
			return false;
		mapWorld.addStaircase(1, 1, 11,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.4f });
		mapWorld.finishBuild();
		return mapWorld.isTraversalTopologyValid();
	}

	bool sharedLocationWallsCanBeOpenedAndRestored()
	{
		core::World world("Shared Location walls", 8, 4);
		auto left = world.addRoom("Left", 0, 1, 0, 3, 2);
		auto right = world.addRoom("Right", 0, 0, 3, 3, 3);
		world.finishBuild();

		std::string diagnostic;
		if (!world.canRemoveLocationWall(left, 0, CORE_SIDE_RIGHT, &diagnostic)
			|| world.canRemoveLocationWall(left, 0, CORE_SIDE_LEFT, &diagnostic)) return false;
		bool activeEditRejected = false;
		try { world.removeLocationWall(left, 0, CORE_SIDE_RIGHT); }
		catch (std::exception const&) { activeEditRejected = true; }
		if (!activeEditRejected) return false;

		world.pauseSimulation();
		world.removeLocationWall(left, 0, CORE_SIDE_RIGHT);
		if (world.getSector(left)->getEndType(0, CORE_SIDE_RIGHT) != core::SectorEndType::None
			|| world.getSector(right)->getEndType(1, CORE_SIDE_LEFT) != core::SectorEndType::None
			|| !world.canAddLocationWall(right, 1, CORE_SIDE_LEFT, &diagnostic)) return false;
		world.finishBuild();
		if (!world.isTraversalTopologyValid()) return false;

		world.addLocationWall(right, 1, CORE_SIDE_LEFT);
		if (world.getSector(left)->getEndType(0, CORE_SIDE_RIGHT) != core::SectorEndType::Wall
			|| world.getSector(right)->getEndType(1, CORE_SIDE_LEFT) != core::SectorEndType::Wall)
			return false;
		world.finishBuild();
		return world.isTraversalTopologyValid()
			&& world.canRemoveLocationWall(right, 1, CORE_SIDE_LEFT, &diagnostic);
	}

	bool walkwayEditingEnforcesPlacementMovementAndOccupancyRules()
	{
		core::World world("Walkway editing", 10, 4);
		auto room = world.addRoom("Walkway room", 0, 0, 0, 4, 3);
		auto otherRoom = world.addRoom("Other room", 0, 0, 6, 3, 3);
		std::string diagnostic;
		if (world.canAddSectorWalkway(room, 0, 1, &diagnostic)
			|| !world.canAddSectorWalkway(room, 1, 1, &diagnostic)) return false;
		auto created = world.addSectorWalkway(room, 1, 1);
		world.finishBuild();
		world.pauseSimulation();

		auto acrossRooms = world.planMoveSectorObject(room, created.index, 6, 1);
		auto withinRoom = world.planMoveSectorObject(room, created.index, 2, 1);
		if (acrossRooms.valid || !withinRoom.valid) return false;

		auto agentId = world.createAgent("Walkway occupant", room, 1, 1.5f);
		if (world.planMoveSectorObject(room, created.index, 2, 1).valid) return false;
		bool occupiedDeleteRejected = false;
		try { world.removeSectorWalkway(room, created.index); }
		catch (std::exception const&) { occupiedDeleteRejected = true; }
		if (!occupiedDeleteRejected) return false;
		auto cropped = world.planResizeLocation(room, 0, 0, 1, 3);
		if (cropped.valid) return false;

		if (!world.removeAgent(agentId)) return false;
		withinRoom = world.planMoveSectorObject(room, created.index, 2, 1);
		if (!withinRoom.valid) return false;
		auto moved = world.applyObjectMove(withinRoom);
		if (!moved || moved->getCellX() != 2 || moved->getCellY() != 1
			|| moved->getSector()->getIndex() != room) return false;
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < moved->getSector()->getNumObjects(); ++i)
			if (moved->getSector()->getObject(i) == moved) { movedIndex = i; break; }
		if (movedIndex == ~0u || !world.removeSectorWalkway(room, movedIndex)) return false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::Walkway) return false;

		world.addSectorWalkway(room, 1, 3);
		world.finishBuild();
		auto cropUnoccupied = world.planResizeLocation(room, 0, 0, 3, 3);
		if (!cropUnoccupied.valid) return false;
		auto resizedRoom = world.applyLocationEdit(cropUnoccupied);
		for (uint32_t i = 0; i < world.getSector(resizedRoom)->getNumObjects(); ++i)
			if (auto object = world.getSector(resizedRoom)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::Walkway) return false;
		return world.getSector(otherRoom) != nullptr && world.isTraversalTopologyValid();
	}

	bool forceBridgeObjectEditingIsAtomic()
	{
		core::World world("Force Bridge editing", 10, 4);
		auto room = world.addRoom("Bridge room", 0, 0, 0, 8, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 6);
		core::World::CreateForceBridgeOptions options{ 2, CORE_SIDE_LEFT, true, true, 1 };
		std::string diagnostic;
		if (!world.canAddSectorForceBridge(room, 1, 1, options, &diagnostic)
			|| world.canAddSectorForceBridge(room, 1, 2, options, &diagnostic)) return false;
		auto created = world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();
		world.pauseSimulation();

		core::World::CreateForceBridgeOptions authored;
		if (!world.getSectorForceBridgeOptions(room, created.forceBridge.index, authored)
			|| authored.width != 2 || authored.controlCount != 1) return false;
		auto move = world.planMoveSectorObject(room, created.forceBridge.index, 4, 1);
		if (!move.valid || move.previewWidth != 2) return false;
		auto moved = world.applyObjectMove(move);
		if (!moved || moved->getCellX() != 4) return false;
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < moved->getSector()->getNumObjects(); ++i)
			if (moved->getSector()->getObject(i) == moved) { movedIndex = i; break; }
		if (movedIndex == ~0u) return false;
		options.fromSide = CORE_SIDE_RIGHT;
		options.controlCount = 2;
		auto edited = world.applySectorForceBridgeOptions(room, movedIndex, options);
		if (!edited || edited->getCellX() != 4) return false;
		uint32_t editedIndex = ~0u;
		for (uint32_t i = 0; i < edited->getSector()->getNumObjects(); ++i)
			if (edited->getSector()->getObject(i) == edited) { editedIndex = i; break; }
		if (editedIndex == ~0u) return false;
		auto occupant = world.createAgent("Bridge occupant", room, 1, 4.5f);
		bool occupiedDeleteRejected = false;
		try { world.removeSectorForceBridge(room, editedIndex); }
		catch (std::exception const&) { occupiedDeleteRejected = true; }
		if (!occupiedDeleteRejected || !world.removeAgent(occupant)
			|| !world.removeSectorForceBridge(room, editedIndex)) return false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::ForceBridge) return false;
		return world.isTraversalTopologyValid();
	}

	bool forceBridgeWalkwayDeletionUpdatesItsDestination()
	{
		{
			core::World placement("Force Bridge inferred width", 8, 4);
			auto placementRoom = placement.addRoom("Bridge room", 0, 0, 0, 6, 3);
			placement.addSectorWalkway(placementRoom, 1, 0);
			placement.addSectorWalkway(placementRoom, 1, 3);
			uint32_t inferredWidth = 0;
			std::string diagnostic;
			core::World::CreateForceBridgeOptions inferred;
			if (!placement.calculateSectorForceBridgeWidthToRight(placementRoom, 1, 1,
				inferredWidth, &diagnostic) || inferredWidth != 2) return false;
			inferred.width = inferredWidth;
			if (!placement.canAddSectorForceBridge(placementRoom, 1, 1, inferred, &diagnostic))
				return false;
		}

		{
			core::World right("Right-origin Force Bridge dependencies", 9, 4);
			auto rightRoom = right.addRoom("Bridge room", 0, 0, 0, 7, 3);
			right.addSectorWalkway(rightRoom, 1, 2);
			auto rightDestination = right.addSectorWalkway(rightRoom, 1, 3);
			auto rightOrigin = right.addSectorWalkway(rightRoom, 1, 5);
			core::World::CreateForceBridgeOptions rightOptions{
				1, CORE_SIDE_RIGHT, true, true, 1 };
			right.addSectorForceBridge(rightRoom, 1, 4, rightOptions);
			right.finishBuild();
			right.pauseSimulation();
			bool rightOriginRejected = false;
			try { right.removeSectorWalkway(rightRoom, rightOrigin.index); }
			catch (std::exception const&) { rightOriginRejected = true; }
			if (!rightOriginRejected
				|| !right.removeSectorWalkway(rightRoom, rightDestination.index)) return false;
			bool resized = false;
			auto sector = right.getSector(rightRoom);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto candidate = sector->getObject(i);
				if (!candidate || candidate->getObjectType() != core::SectorObjectType::ForceBridge) continue;
				core::World::CreateForceBridgeOptions updated;
				resized = right.getSectorForceBridgeOptions(rightRoom, i, updated)
					&& candidate->getCellX() == 3 && updated.width == 2
					&& updated.fromSide == CORE_SIDE_RIGHT;
			}
			if (!resized) return false;
		}

		core::World world("Force Bridge walkway dependencies", 10, 4);
		auto room = world.addRoom("Bridge room", 0, 0, 0, 7, 3);
		auto origin = world.addSectorWalkway(room, 1, 0);
		auto destination = world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		core::World::CreateForceBridgeOptions options{ 1, CORE_SIDE_LEFT, true, true, 1 };
		world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();
		world.pauseSimulation();

		bool originRejected = false;
		try { world.removeSectorWalkway(room, origin.index); }
		catch (std::exception const&) { originRejected = true; }
		if (!originRejected || !world.removeSectorWalkway(room, destination.index)) return false;
		auto sector = world.getSector(room);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::ForceBridge) continue;
			core::World::CreateForceBridgeOptions updated;
			return world.getSectorForceBridgeOptions(room, i, updated)
				&& object->getCellX() == 1 && updated.width == 2;
		}
		return false;
	}

	bool roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints()
	{
		core::World world("Room Ladder editing", 8, 6);
		auto room = world.addRoom("Ladder room", 0, 0, 0, 5, 5);
		world.addSectorWalkway(room, 2, 1);
		world.addSectorWalkway(room, 4, 1);
		world.addSectorWalkway(room, 3, 3);
		world.addSectorWalkway(room, 3, 4);
		uint32_t height = 0; std::string diagnostic;
		if (!world.canAddRoomLadder(room, 0, 1, &height, &diagnostic) || height != 3) return false;
		auto lower = world.addRoomLadder(room, 0, 1);
		auto upper = world.addRoomLadder(room, 2, 1);
		core::World::CreateLadderOptions defaultOptions{};
		if (!world.getRoomLadderOptions(room, lower.ladder.index, defaultOptions))
			return false;
		if (std::static_pointer_cast<const core::LadderSectorObject>(
			lower.ladder.sector->getObject(lower.ladder.index))->getLadder()->getLevelsHigh() != 3) return false;
		if (std::static_pointer_cast<const core::LadderSectorObject>(
			upper.ladder.sector->getObject(upper.ladder.index))->getLadder()->getLevelsHigh() != 3) return false;
		if (world.canAddRoomLadder(room, 0, 2, &height, &diagnostic)
			|| diagnostic.find("No Walkway") == std::string::npos) return false;
		auto corridor = world.addCorridor(5, 0, 3);
		if (world.canAddRoomLadder(corridor, 0, 0, &height, &diagnostic)) return false;

		world.finishBuild();
		world.pauseSimulation();
		auto move = world.planMoveSectorObject(room, lower.ladder.index, 3, 0);
		if (!move.valid || move.previewHeight != 4) return false;
		auto moved = world.applyObjectMove(move);
		if (!moved || std::static_pointer_cast<const core::LadderSectorObject>(moved)
			->getLadder()->getLevelsHigh() != 4) return false;

		auto nearer = world.addSectorWalkway(room, 1, 3);
		auto rebuiltRoom = world.getSector(room);
		std::shared_ptr<const core::LadderSectorObject> recalculated;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0) recalculated = ladder;
		}
		if (!recalculated || recalculated->getLadder()->getLevelsHigh() != 2) return false;
		if (!world.removeSectorWalkway(room, nearer.index)) return false;
		rebuiltRoom = world.getSector(room);
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0)
			{
				if (ladder->getLadder()->getLevelsHigh() != 4) return false;
				movedIndex = i;
			}
		}
		if (movedIndex == ~0u) return false;
		auto edited = world.applyRoomLadderOptions(room, movedIndex, { 0, true, false, 2 });
		if (!edited) return false;
		core::World::CreateLadderOptions options{};
		rebuiltRoom = world.getSector(room);
		movedIndex = ~0u;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
			if (rebuiltRoom->getObject(i) == edited) { movedIndex = i; break; }
		if (movedIndex == ~0u || !world.getRoomLadderOptions(room, movedIndex, options)
			|| !options.extensible || options.startExtended
			|| options.directionalBatchLimit != 2) return false;
		uint32_t insetControls = 0;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto control = rebuiltRoom->getObject(i);
			if (!control || control->getObjectType() != core::SectorObjectType::InteractionPoint
				|| control->getCellX() != 3 || (control->getCellY() != 0 && control->getCellY() != 3)) continue;
			auto button = control->_getObject();
			float centerX = button->getPosition().x + button->getSize().x * 0.5f;
			if (std::abs(centerX - 3.8f) < 0.0001f) ++insetControls;
		}
		if (insetControls != 2) return false;
		if (!world.removeRoomLadder(room, movedIndex)) return false;
		rebuiltRoom = world.getSector(room);
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0) return false;
		}
		auto edge = world.addRoomLadder(room, 0, 4, { 0, true, true });
		for (auto const& control : edge.controls)
		{
			auto button = control.sector->getObject(control.index)->_getObject();
			float centerX = button->getPosition().x + button->getSize().x * 0.5f;
			if (std::abs(centerX - 4.2f) >= 0.0001f) return false;
		}
		return true;
	}

	bool deletingWalkwayPreservesUnrelatedRoomDoor()
	{
		core::World world("Walkway deletion isolation", 16, 6);
		world.addCorridor(4, 9, 4);
		auto room = world.addRoom("Walkway room", 1, 3, 9, 4, 2);
		core::World::CreateObjectResult walkways[4];
		for (uint32_t x = 0; x < 4; ++x)
			walkways[x] = world.addSectorWalkway(room, 1, x);
		world.addSectorDoor(0, 4, 12);
		world.finishBuild();
		world.pauseSimulation();

		try
		{
			// The Walkway at 11,4 is not beneath the Door at 12,4. Removing it
			// must shrink the physical queue rather than invalidate the Door.
			if (!world.removeSectorWalkway(room, walkways[2].index)) return false;
		}
		catch (std::exception const&)
		{
			return false;
		}
		uint32_t doorsInRoom = 0, remainingWalkways = 0;
		bool walkwayAt11 = false, walkwayAt12 = false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
			{
				doorsInRoom += object->getObjectType() == core::SectorObjectType::Door;
				if (object->getObjectType() != core::SectorObjectType::Walkway) continue;
				++remainingWalkways;
				walkwayAt11 = walkwayAt11 || object->getCellX() == 11;
				walkwayAt12 = walkwayAt12 || object->getCellX() == 12;
			}
		return doorsInRoom == 1 && remainingWalkways == 3
			&& !walkwayAt11 && walkwayAt12 && world.isTraversalTopologyValid()
			&& world.getSimulationSnapshot().traversalResources.size() == 1;
	}

	bool ordinaryTraversalCommitsOnlyAtDestination()
	{
		core::World world("Ordinary transition", 10, 2);
		auto sourceSector = world.addCorridor(0, 0, 3);
		auto destinationSector = world.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Ordinary traveller", sourceSector, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		bool observedPermit = false;
		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (!snapshot.traversalPermits.empty())
			{
				observedPermit = snapshot.traversalPermits.size() == 1
					&& snapshot.traversalRequests.size() == 1
					&& snapshot.traversalRequests.front().diagnostic.starts_with("Active:")
					&& snapshot.agents.front().hasLocomotionTask;
			}

			if (agent->getState() != core::Agent::State::Idle
				&& agent->getSector() != world.getSector(sourceSector).get())
			{
				return false;
			}
		}

		auto snapshot = world.getSimulationSnapshot();
		return observedPermit
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(destinationSector).get()
			&& agent->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool deniedTraversalCannotBeCrossed()
	{
		core::World world("Denied transition", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Blocked traveller", corridor, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::GapEdge>()), true);
		world.advanceTicks(30);

		auto snapshot = world.getSimulationSnapshot();
		if (agent->getGlobalPosition().distanceTo(source->getPosition()) >= 0.001f
			|| agent->getSector() != world.getSector(corridor).get()
			|| agent->getState() != core::Agent::State::WaitingForTraversal
			|| snapshot.traversalRequests.size() != 1
			|| snapshot.traversalRequests.front().state != core::TraversalRequestState::Denied
			|| !snapshot.traversalRequests.front().diagnostic.starts_with("Denied:")
			|| !snapshot.traversalPermits.empty())
		{
			return false;
		}

		agent->clearPath();
		snapshot = world.getSimulationSnapshot();
		return snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty()
			&& agent->getSector() == world.getSector(corridor).get();
	}

	bool cancellationReleasesPermitWithoutCommitting()
	{
		core::World world("Cancelled transition", 10, 2);
		auto sourceSector = world.addCorridor(0, 0, 3);
		auto destinationSector = world.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Cancelling traveller", sourceSector, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		for (uint32_t i = 0; i < 10 && !agent->getTraversalPermitId(); ++i)
		{
			world.advanceTick();
		}
		if (!agent->getTraversalPermitId() || agent->getSector() != world.getSector(sourceSector).get())
		{
			return false;
		}

		agent->clearPath();
		world.advanceTicks(10);
		auto snapshot = world.getSimulationSnapshot();
		return agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(sourceSector).get()
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool typedLightingInteractionCoalescesAndCancelsByRequester()
	{
		core::World world("Typed lighting interaction", 6, 2);
		auto corridorIndex = world.addCorridor(0, 0, 5);
		world.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto firstAgent = world.createAgent("First operator", corridorIndex, 0, 0.5f);
		auto secondAgent = world.createAgent("Dependent operator", corridorIndex, 0, 0.7f);

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		binding.requirement = core::InteractionBindingRequirement::Required;
		auto point = world.createInteractionPoint("Typed light control", sectorId,
			{ 3.5f, 0.5f }, 0.1f, core::World::getFixedTimestep() * 3.0f, { binding });
		auto firstRequest = world.requestInteraction(point, firstAgent);
		auto secondRequest = world.requestInteraction(point, secondAgent);
		if (!firstRequest || !secondRequest)
		{
			return false;
		}

		auto first = world.lookupInteractionRequest(firstRequest);
		auto second = world.lookupInteractionRequest(secondRequest);
		if (!first || !second || first.entity->getOperations().size() != 1
			|| second.entity->getOperations().size() != 1
			|| first.entity->getOperations().front().first != second.entity->getOperations().front().first)
		{
			return false;
		}
		auto operationId = first.entity->getOperations().front().first;

		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			world.advanceTick();
			auto operation = world.lookupDeviceOperation(operationId);
			if (operation && operation.entity->getState() == core::DeviceOperationState::Running)
			{
				break;
			}
		}
		auto firstPosition = world.lookupAgent(firstAgent).entity->getGlobalPosition();
		if (firstPosition.distanceTo({ 3.5f, 0.5f }) > 0.101f || !world.cancelInteraction(firstRequest))
		{
			return false;
		}
		auto operation = world.lookupDeviceOperation(operationId);
		if (!operation || operation.entity->getState() == core::DeviceOperationState::Cancelled
			|| operation.entity->getRequesters().size() != 1
			|| !operation.entity->getRequesters().contains(secondAgent))
		{
			return false;
		}

		world.advanceTicks(3);
		first = world.lookupInteractionRequest(firstRequest);
		second = world.lookupInteractionRequest(secondRequest);
		auto snapshot = world.getSimulationSnapshot();
		return first && first.entity->getResult() == core::InteractionResult::Cancelled
			&& second && second.entity->getResult() == core::InteractionResult::Succeeded
			&& !world.getSector(corridorIndex)->areLightsOn()
			&& snapshot.deviceOperations.size() == 1
			&& snapshot.deviceOperations.front().hasCommand
			&& snapshot.deviceOperations.front().command.type == core::DeviceCommandType::SetSectorLights
			&& snapshot.deviceOperations.front().state == core::DeviceOperationState::Succeeded
			&& snapshot.interactionPoints.front().activeRequest == core::InteractionRequestId{};
	}

	bool singleAgentDoorJourney(core::DoorActivationMode mode)
	{
		core::World world("Single-agent door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = mode;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 8.0f;
		auto created = world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& edge) { return edge->getType() == core::EdgeType::Door; });
		if (edgeIt == world.getGraph()->getEdges().end() || !created.traversalResource)
		{
			return false;
		}
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Door traveller", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);

		bool observedWaitingForFullOpen = false;
		bool observedVisibleCrossingLease = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks && agent->getState() != core::Agent::State::Idle; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto const& resource = snapshot.traversalResources.front();
			if (resource.doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()
				&& agent->getSector() == world.getSector(fore).get())
			{
				observedWaitingForFullOpen = true;
			}
			if (agent->getState() == core::Agent::State::TraversingEdge
				&& resource.doorState == core::DoorSnapshotState::Open
				&& resource.openLeaseCount == 1
				&& agent->getSector() == world.getSector(fore).get())
			{
				observedVisibleCrossingLease = true;
			}
		}

		auto completed = world.getSimulationSnapshot();
		if (!observedWaitingForFullOpen || !observedVisibleCrossingLease
			|| agent->getState() != core::Agent::State::Idle
			|| agent->getSector() != world.getSector(back).get()
			|| completed.deviceOperations.size() != 1
			|| completed.deviceOperations.front().command.type != core::DeviceCommandType::OpenDoor
			|| completed.deviceOperations.front().state != core::DeviceOperationState::Succeeded
			|| completed.traversalResources.front().doorActivationMode != mode
			|| completed.traversalResources.front().openLeaseCount != 0)
		{
			return false;
		}

		for (uint32_t i = 0; i < 120
			&& world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closed; ++i)
		{
			world.advanceTick();
		}
		return world.getSimulationSnapshot().traversalResources.front().doorState == core::DoorSnapshotState::Closed;
	}

	bool bulkheadAndWindowThresholdsUseTraversalResources()
	{
		// A bulkhead is horizontal and same-layer, but still queues and waits for
		// its fully-open resource permit.
		core::World bulkheadWorld("Bulkhead threshold", 8, 2);
		auto left = bulkheadWorld.addRoom("Left", 0, 0, 0, 3, 1);
		auto right = bulkheadWorld.addRoom("Right", 0, 0, 3, 3, 1);
		core::World::CreateBulkheadDoorOptions bulkheadOptions;
		bulkheadOptions.activationMode = core::DoorActivationMode::Manual;
		bulkheadOptions.controls[0] = bulkheadOptions.controls[1] = false;
		auto bulkhead = bulkheadWorld.addSectorBulkheadDoor(0, 0, 3,
			CORE_SIDE_LEFT, bulkheadOptions);
		bulkheadWorld.finishBuild();
		auto bulkheadEdge = std::find_if(bulkheadWorld.getGraph()->getEdges().begin(),
			bulkheadWorld.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::BulkheadDoor; });
		if (bulkheadEdge == bulkheadWorld.getGraph()->getEdges().end()
			|| (*bulkheadEdge)->getTraversalResourceId() != bulkhead.traversalResource) return false;
		auto source = (*bulkheadEdge)->getVertex(0)->getSector()->getIndex() == left
			? (*bulkheadEdge)->getVertex(0) : (*bulkheadEdge)->getVertex(1);
		auto destination = (*bulkheadEdge)->getOtherVertex(source);
		auto agentId = bulkheadWorld.createAgent("Left bulkhead traveller", left, 0, 1.0f);
		auto opposingId = bulkheadWorld.createAgent("Right bulkhead traveller", right, 0, 1.0f);
		auto agent = bulkheadWorld.lookupAgent(agentId).entity;
		auto opposing = bulkheadWorld.lookupAgent(opposingId).entity;
		agent->setPath(twoNodePath(source, destination, *bulkheadEdge), true);
		opposing->setPath(twoNodePath(destination, source, *bulkheadEdge), true);
		bool waitedForOpen = false;
		bool serializedContention = true;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& (agent->getState() != core::Agent::State::Idle
				|| opposing->getState() != core::Agent::State::Idle); ++i)
		{
			bulkheadWorld.advanceTick();
			auto const& snapshot = bulkheadWorld.getSimulationSnapshot();
			if (!snapshot.traversalResources.empty()
				&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()) waitedForOpen = true;
			if (snapshot.traversalPermits.size() > 1) serializedContention = false;
		}
		if (!waitedForOpen || !serializedContention
			|| agent->getSector() != bulkheadWorld.getSector(right).get()
			|| opposing->getSector() != bulkheadWorld.getSector(left).get()
			|| !bulkheadWorld.getSimulationSnapshot().traversalRequests.empty()) return false;

		// Traversable windows contribute conditional topology, and only the clear,
		// fully-open state can receive a permit.
		core::World windowWorld("Window threshold", 6, 2);
		auto fore = windowWorld.addRoom("Fore", 0, 0, 0, 5, 1);
		auto back = windowWorld.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateWindowOptions windowOptions;
		windowOptions.traversable = true;
		windowOptions.initialState = core::Window::State::Open;
		auto window = windowWorld.addSectorWindow(0, 0, 2, 1, 1, windowOptions);
		windowWorld.finishBuild();
		auto windowEdge = std::find_if(windowWorld.getGraph()->getEdges().begin(),
			windowWorld.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::Window; });
		if (windowEdge == windowWorld.getGraph()->getEdges().end()
			|| (*windowEdge)->getTraversalResourceId() != window.traversalResource
			|| !window.object->isNormallyTraversable()) return false;
		constexpr core::Window::State blockedStates[] = {
			core::Window::State::Closed, core::Window::State::Opening,
			core::Window::State::Closing, core::Window::State::Broken,
			core::Window::State::Frosted, core::Window::State::Frosting,
			core::Window::State::Unfrosting, core::Window::State::Tinted,
			core::Window::State::Tinting, core::Window::State::Untinting
		};
		for (auto state : blockedStates)
		{
			window.object->setState(state);
			if ((*windowEdge)->isTraversable({}, {})) return false;
		}
		window.object->setState(core::Window::State::Open, core::Window::Style::Tinted);
		if ((*windowEdge)->isTraversable({}, {})) return false;
		window.object->setState(core::Window::State::Open, core::Window::Style::Frosted);
		if ((*windowEdge)->isTraversable({}, {})) return false;
		window.object->setState(core::Window::State::Open);
		auto windowSource = (*windowEdge)->getVertex(0)->getSector()->getIndex() == fore
			? (*windowEdge)->getVertex(0) : (*windowEdge)->getVertex(1);
		auto windowDestination = (*windowEdge)->getOtherVertex(windowSource);
		auto windowAgentId = windowWorld.createAgent("Window traveller", fore, 0, 0.5f);
		auto windowAgent = windowWorld.lookupAgent(windowAgentId).entity;
		windowAgent->setPath(twoNodePath(windowSource, windowDestination, *windowEdge), true);
		for (uint32_t i = 0; i < MaximumSimulationTicks && windowAgent->getState() != core::Agent::State::Idle; ++i)
			windowWorld.advanceTick();
		return windowAgent->getSector() == windowWorld.getSector(back).get()
			&& windowWorld.getSimulationSnapshot().traversalRequests.empty();
	}

	bool pausedTopologyRebuildIsAtomicAndCleansOwnership()
	{
		core::World world("Paused topology rebuild", 8, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 7, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 8.0f;
		auto door = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto generation = world.getTopologyGeneration();
		auto oldGraph = world.getGraph();
		auto edge = *std::find_if(oldGraph->getEdges().begin(), oldGraph->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Rebuild traveller", fore, 0,
			source->getPosition().x - world.getSector(fore)->getPosition().x);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(3);
		auto active = world.getSimulationSnapshot();
		if (active.traversalRequests.empty()) return false;

		// An active simulation cannot be structurally changed.
		bool rejected = false;
		try { world.addSectorMarker(fore, 0, 0.5f); }
		catch (std::exception const&) { rejected = true; }
		if (!rejected || world.isSimulationPaused()) return false;

		world.pauseSimulation();
		auto pausedTick = world.getSimulationTick();
		world.advanceTicks(10);
		auto paused = world.getSimulationSnapshot();
		if (!paused.paused || world.getSimulationTick() != pausedTick
			|| !paused.traversalRequests.empty() || !paused.traversalPermits.empty()) return false;
		for (auto const& resource : paused.traversalResources)
		{
			if (resource.id != door.traversalResource) continue;
			if (resource.openLeaseCount || resource.crossingOwner
				|| std::any_of(resource.queueLanes.begin(), resource.queueLanes.end(),
					[](auto const& lane) { return std::any_of(lane.positions.begin(), lane.positions.end(),
						[](auto const& position) { return (bool)position.owner; }); })) return false;
		}

		uint32_t marker;
		world.addSectorMarker(fore, 0, 0.5f, &marker);
		if (!world.isTraversalTopologyDirty() || !world.rebuildTraversalTopology()
			|| world.getGraph() == oldGraph || world.getTopologyGeneration() != generation + 1
			|| !world.getGraph()->getVertexByIdentifier(marker)
			|| !world.resumeSimulation()) return false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& agent->getState() != core::Agent::State::Idle; ++i) world.advanceTick();
		if (agent->getSector() != world.getSector(back).get()) return false;

		// Candidate failure leaves the previous graph installed, the simulation
		// paused, and removed handles permanently invalid.
		core::World invalid("Invalid paused rebuild", 6, 2);
		invalid.addRoom("Fore", 0, 0, 0, 5, 1);
		invalid.addRoom("Back", 1, 0, 0, 5, 1);
		auto invalidDoor = invalid.addSectorDoor(0, 0, 2);
		invalid.finishBuild();
		auto previousGraph = invalid.getGraph();
		invalid.pauseSimulation();
		if (!invalid.removeTraversalResource(invalidDoor.traversalResource)
			|| invalid.lookupTraversalResource(invalidDoor.traversalResource)) return false;
		auto replacement = invalid.createTraversalResource("Replacement handle proof");
		if (replacement.value <= invalidDoor.traversalResource.value
			|| invalid.rebuildTraversalTopology() || invalid.resumeSimulation()
			|| !invalid.isSimulationPaused() || invalid.getGraph() != previousGraph
			|| invalid.getTopologyDiagnostic().empty()) return false;
		return true;
	}

	bool agentsPressUpcomingDoorButtonsWhilePassing()
	{
		// Reproduce the Citadel route: the Button's Interactable vertex is part of
		// the in-sector path leading from the far Door to the controlled Door.
		core::World world("Opportunistic remote door", 8, 2);
		auto corridor = world.addCorridor(0, 1, 5);
		world.addRoom("Destination", 1, 0, 0, 3, 1);
		world.addRoom("Far room", 1, 0, 4, 3, 1);
		uint32_t markerId;
		world.addSectorMarker(1, 0, 0.5f, &markerId);
		core::World::CreateDoorOptions remote;
		remote.activationMode = core::DoorActivationMode::RemoteControlled;
		remote.controls[0] = true;
		auto created = world.addSectorDoor(0, 0, 1, remote);
		world.addSectorDoor(0, 0, 5);
		world.finishBuild();

		auto target = world.getGraph()->getVertexByIdentifier(markerId);
		std::vector<core::AgentId> ids = {
			world.createAgent("Early presser one", corridor, 0, 2.75f),
			world.createAgent("Early presser two", corridor, 0, 2.75f)
		};
		for (auto id : ids)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path || path->nodes.size() < 3) return false;
			bool reachesButtonBeforeDoor = false;
			for (auto const& node : path->nodes)
			{
				if (node.edge && node.edge->getType() == core::EdgeType::Door) break;
				reachesButtonBeforeDoor = reachesButtonBeforeDoor || (node.targetVertex
					&& node.targetVertex->getSubType() == core::VertexSubType::Interactable);
			}
			if (!reachesButtonBeforeDoor) return false;
			agent->setPath(std::move(path), true);
		}

		bool observedIndependentPressesBeforeDoorRequest = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			bool hasDoorRequest = std::any_of(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(), [](auto const& request)
					{ return request.edgeType == core::EdgeType::Door; });
			if (!hasDoorRequest && snapshot.interactionRequests.size() == 2
				&& snapshot.deviceOperations.size() == 1
				&& snapshot.deviceOperations.front().requesters.size() == 2)
			{
				observedIndependentPressesBeforeDoorRequest = true;
			}
			if (std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })) break;
		}

		return observedIndependentPressesBeforeDoorRequest
			&& std::all_of(ids.begin(), ids.end(), [&](auto id)
			{
				return world.lookupAgent(id).entity->getSector() == world.getSector(1).get();
			})
			&& world.lookupInteractionPoint(created.controls[0].interactionPoint).entity->getReach()
				== CORE_AGENT_MAX_HEIGHT * 0.4f;
	}

	bool remoteDoorUsesOnePhysicalOperatorAndSharedOperation()
	{
		core::World world("Shared remote door", 7, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 6, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = true;
		options.controls[1] = true;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();

		for (auto const& control : created.controls)
		{
			auto object = control.sector->getObject(control.index)->_getObject();
			auto button = std::dynamic_pointer_cast<core::Button>(object);
			if (!button || !control.interactionPoint
				|| button->getInteractionPointId() != control.interactionPoint
				|| !world.lookupInteractionPoint(control.interactionPoint)) return false;
		}

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = world.createAgent("First remote waiter", fore, 0, 0.4f);
		auto secondId = world.createAgent("Second remote waiter", fore, 0, 0.6f);
		auto first = world.lookupAgent(firstId).entity;
		auto second = world.lookupAgent(secondId).entity;
		first->setPath(twoNodePath(source, destination, edge), true);
		second->setPath(twoNodePath(source, destination, edge), true);

		bool observedSharedPreparation = false;
		bool cancelledFirst = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& second->getState() != core::Agent::State::Idle; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (!cancelledFirst && snapshot.traversalRequests.size() == 2
				&& snapshot.interactionRequests.size() == 1
				&& snapshot.deviceOperations.size() == 1
				&& snapshot.deviceOperations.front().requesters.size() == 2
				&& snapshot.traversalResources.front().preparationOperator
				&& snapshot.traversalResources.front().activePreparation)
			{
				observedSharedPreparation = true;
				first->clearPath();
				cancelledFirst = true;
			}
		}

		auto snapshot = world.getSimulationSnapshot();
		return observedSharedPreparation && cancelledFirst
			&& first->getSector() == world.getSector(fore).get()
			&& second->getState() == core::Agent::State::Idle
			&& second->getSector() == world.getSector(back).get()
			&& snapshot.deviceOperations.size() >= 1
			&& std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(), [](auto const& operation)
				{ return operation.command.type == core::DeviceCommandType::OpenDoor
					&& operation.state == core::DeviceOperationState::Succeeded; });
	}

	bool remoteDoorWithoutReachableControlIsUnavailable()
	{
		core::World world("Uncontrolled remote door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = false;
		options.controls[1] = false;
		auto created = world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Stranded remote waiter", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(400);
		auto snapshot = world.getSimulationSnapshot();
		return snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.traversalRequests.front().failureReason == core::TraversalFailureReason::NoReachableControl
			&& snapshot.deviceOperations.empty() && snapshot.interactionRequests.empty();
	}

	bool fairDoorQueuesServeBothSidesInStableOrder()
	{
		core::World world("Fair two-sided door", 8, 2);
		auto fore = world.addRoom("Fore queue", 0, 0, 0, 7, 1);
		auto back = world.addRoom("Back queue", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto foreVertex = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto backVertex = edge->getOtherVertex(foreVertex);

		std::vector<core::AgentId> ids = {
			world.createAgent("Fore first", fore, 0, 3.5f),
			world.createAgent("Back first", back, 0, 3.5f),
			world.createAgent("Fore second", fore, 0, 3.5f),
			world.createAgent("Back second", back, 0, 3.5f)
		};
		for (size_t i = 0; i < ids.size(); ++i)
		{
			auto source = i % 2 == 0 ? foreVertex : backVertex;
			auto destination = i % 2 == 0 ? backVertex : foreVertex;
			world.lookupAgent(ids[i]).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		bool observedSeparatedPositions = false;
		bool observedQueueDiagnostics = false;
		std::vector<core::AgentId> completionOrder;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2 && completionOrder.size() < ids.size(); ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 1)
			{
				return false;
			}
			auto const& resource = snapshot.traversalResources.front();
			observedQueueDiagnostics = observedQueueDiagnostics
				|| (resource.queueLanes.size() == 2 && resource.crossingOwner);
			for (auto const& lane : resource.queueLanes)
			{
				std::vector<core::Vector2> occupied;
				for (auto const& position : lane.positions)
				{
					if (!position.owner)
					{
						continue;
					}
					for (auto const& other : occupied)
					{
						if (position.position.distanceTo(other) < CORE_DOOR_QUEUE_STOP_WIDTH - 0.001f)
						{
							return false;
						}
					}
					occupied.push_back(position.position);
				}
				observedSeparatedPositions = observedSeparatedPositions || occupied.size() >= 2;
			}
			for (auto id : ids)
			{
				if (std::find(completionOrder.begin(), completionOrder.end(), id) == completionOrder.end()
					&& world.lookupAgent(id).entity->getState() == core::Agent::State::Idle)
				{
					completionOrder.push_back(id);
				}
			}
		}
		return completionOrder == ids && observedSeparatedPositions && observedQueueDiagnostics
			&& world.getSimulationSnapshot().traversalResources.front().crossingOwner == core::TraversalRequestId{};
	}

	bool queuePositionsPreferObjectProximityThenAgentProximity()
	{
		core::World world("Nearest queue position", 8, 2);
		auto fore = world.addRoom("Queue room", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Queue head", fore, 0, 3.5f),
			world.createAgent("Second waiter", fore, 0, 3.5f),
			world.createAgent("Third waiter", fore, 0, 3.5f)
		};
		for (auto id : ids)
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		world.advanceTicks(3);
		auto initial = world.getSimulationSnapshot();
		if (initial.traversalRequests.size() != 3) return false;
		auto initialThird = std::find_if(initial.traversalRequests.begin(), initial.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		if (initialThird == initial.traversalRequests.end() || !initialThird->hasQueuePosition) return false;
		auto const& initialLane = initial.traversalResources.front().queueLanes[initialThird->queueApproach];
		auto initialThirdTarget = initialLane.positions[initialThird->queuePosition].position;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto first = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[0]; });
			auto second = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[1]; });
			auto third = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[2]; });
			if (second == snapshot.traversalRequests.end() || third == snapshot.traversalRequests.end()) continue;
			if ((first == snapshot.traversalRequests.end() || !first->hasQueuePosition)
				&& second->hasQueuePosition && third->hasQueuePosition)
			{
				auto const& lane = snapshot.traversalResources.front().queueLanes[third->queueApproach];
				auto secondTarget = lane.positions[second->queuePosition].position;
				auto thirdTarget = lane.positions[third->queuePosition].position;
				return secondTarget.distanceTo(source->getPosition()) <= 0.001f
					&& thirdTarget.distanceTo(initialThirdTarget) <= 0.001f;
			}
		}
		return false;
	}

	bool doorQueueRequestsBeforeOccupiedTail()
	{
		core::World world("Early Door queue", 8, 2);
		auto fore = world.addRoom("Approach", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		uint32_t approachId;
		world.addSectorMarker(fore, 0, 2.75f, &approachId);
		world.finishBuild();

		auto edge = *find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto blocker = world.createAgent("Door queue head", fore, 0, source->getSectorOffset().x);
		world.lookupAgent(blocker).entity->setPath(twoNodePath(source, destination, edge), true);
		for (uint32_t tick = 0; tick < 20; ++tick) world.advanceTick();

		auto approach = world.getGraph()->getVertexByIdentifier(approachId);
		auto waiter = world.createAgent("Door waiter", fore, 0, 2.75f);
		auto waiterEntity = world.lookupAgent(waiter).entity;
		auto path = world.getGraph()->calculatePath(waiterEntity, approach, destination);
		if (!path) return false;
		waiterEntity->setPath(std::move(path), true);
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto request = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == waiter; });
			if (request == snapshot.traversalRequests.end() || !request->hasQueuePosition) continue;
			auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			auto agent = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == waiter; });
			if (resource == snapshot.traversalResources.end() || agent == snapshot.agents.end()) return false;
			auto const& lane = resource->queueLanes[request->queueApproach];
			auto const target = lane.positions[request->queuePosition].position;
			return agent->globalPosition.x < lane.origin.x
				&& target.x >= agent->globalPosition.x - 0.001f
				&& target.x <= lane.origin.x + 0.001f;
		}
		return false;
	}

	bool queuedCancellationReleasesAndAdvancesPositions()
	{
		core::World world("Queue cancellation", 8, 2);
		auto fore = world.addRoom("Queue room", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		auto created = world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = world.createAgent("First", fore, 0, 3.5f);
		auto cancelledId = world.createAgent("Cancelled", fore, 0, 3.5f);
		auto lastId = world.createAgent("Last", fore, 0, 3.5f);
		for (auto id : { firstId, cancelledId, lastId })
		{
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}
		world.advanceTicks(3);
		auto before = world.getSimulationSnapshot();
		if (before.traversalRequests.size() != 3
			|| std::count_if(before.traversalRequests.begin(), before.traversalRequests.end(),
				[](auto const& request) { return request.queueTicket && request.hasQueuePosition; }) != 3)
		{
			return false;
		}
		world.lookupAgent(cancelledId).entity->clearPath();
		auto after = world.getSimulationSnapshot();
		if (after.traversalRequests.size() != 2
			|| after.traversalResources.front().queueLanes.front().queue.size() != 2)
		{
			return false;
		}
		for (auto const& position : after.traversalResources.front().queueLanes.front().positions)
		{
			if (position.owner && position.owner == before.traversalRequests[1].id)
			{
				return false;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		return world.lookupAgent(firstId).entity->getState() == core::Agent::State::Idle
			&& world.lookupAgent(lastId).entity->getState() == core::Agent::State::Idle
			&& world.lookupAgent(cancelledId).entity->getSector() == world.getSector(fore).get();
	}

	bool resilientWaitingRetainsPriorityAndExpiresPermits()
	{
		core::World world("Resilient door waiting", 8, 2);
		auto fore = world.addRoom("Waiting side", 0, 0, 0, 7, 1);
		world.addRoom("Destination side", 1, 0, 0, 7, 1);
		auto created = world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto initialEdge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto initialSource = initialEdge->getVertex(0)->getSector()->getIndex() == fore
			? initialEdge->getVertex(0) : initialEdge->getVertex(1);

		// One physical position deliberately forces logical overflow. Runtime queue
		// geometry is a structural edit and therefore uses the paused rebuild seam.
		world.pauseSimulation();
		auto sourceSectorId = core::SectorId{ (uint64_t)fore + 1 };
		if (!world.configureDoorQueueLane(created.traversalResource, sourceSectorId,
			initialSource->getPosition(), { -1.0f, 0.0f }, 0.0f)
			|| !world.rebuildTraversalTopology() || !world.resumeSimulation()) return false;
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Queue head", fore, 0, 3.5f),
			world.createAgent("Overflow one", fore, 0, 3.5f),
			world.createAgent("Overflow two", fore, 0, 3.5f)
		};
		for (auto id : ids) world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(2);
		auto queued = world.getSimulationSnapshot();
		if (queued.traversalRequests.size() != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return (bool)request.queueTicket; }) != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return request.hasQueuePosition; }) != 1)
		{
			return false;
		}

		auto firstRequest = queued.traversalRequests.front();
		world.lookupAgent(ids.front()).entity->setPath(twoNodePath(source, destination, edge), true);
		auto compatible = world.getSimulationSnapshot();
		auto retained = std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == compatible.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		// Changing the immediate authority is incompatible and must release the old
		// logical ticket before a later compatible route can queue afresh.
		auto oldOverflow = *std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		world.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination,
			std::make_shared<core::SectorEdge>()), true);
		auto incompatible = world.getSimulationSnapshot();
		if (std::any_of(incompatible.traversalRequests.begin(), incompatible.traversalRequests.end(),
			[&](auto const& request) { return request.id == oldOverflow.id; })) return false;
		world.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(2);
		auto fresh = world.getSimulationSnapshot();
		auto freshOverflow = std::find_if(fresh.traversalRequests.begin(), fresh.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		if (freshOverflow == fresh.traversalRequests.end() || freshOverflow->queueTicket == oldOverflow.queueTicket)
			return false;

		// Route estimation observes queue demand but creates no coordination state.
		auto requestCount = fresh.traversalRequests.size();
		auto permitCount = fresh.traversalPermits.size();
		if (edge->getWeight(destination, world.lookupAgent(ids.front()).entity, true) <= 0.0f
			|| world.getSimulationSnapshot().traversalRequests.size() != requestCount
			|| world.getSimulationSnapshot().traversalPermits.size() != permitCount) return false;

		// A deliberately short no-progress deadline expires the coincident threshold
		// crossing. The request and ticket survive and a fresh permit is assigned.
		auto policy = world.getTraversalWaitingPolicy();
		policy.permitProgressTimeoutTicks = 1;
		world.setTraversalWaitingPolicy(policy);
		core::TraversalPermitId firstPermit;
		core::TraversalPermitId replacementPermit;
		for (uint32_t i = 0; i < MaximumSimulationTicks && !replacementPermit; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			for (auto const& permit : snapshot.traversalPermits)
			{
				if (permit.owner != ids.front()) continue;
				if (!firstPermit) firstPermit = permit.id;
				else if (permit.id != firstPermit) replacementPermit = permit.id;
			}
		}
		if (!firstPermit || !replacementPermit) return false;
		auto afterExpiry = world.getSimulationSnapshot();
		retained = std::find_if(afterExpiry.traversalRequests.begin(), afterExpiry.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == afterExpiry.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		policy.permitProgressTimeoutTicks = 120;
		world.setTraversalWaitingPolicy(policy);
		world.advanceTicks(MaximumSimulationTicks * 2);
		return std::all_of(ids.begin(), ids.end(), [&](auto id)
		{
			auto agent = world.lookupAgent(id).entity;
			return agent->getState() == core::Agent::State::Idle
				&& agent->getSector() == destination->getSector().get();
		}) && world.getSimulationSnapshot().traversalRequests.empty()
			&& world.getSimulationSnapshot().traversalPermits.empty();
	}

	bool wideDoorLanesAndGracefulDisableAreSafe()
	{
		core::World world("Wide safe door", 9, 2);
		auto fore = world.addRoom("Wide fore", 0, 0, 0, 8, 1);
		auto back = world.addRoom("Wide back", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions options;
		options.width = 2;
		options.crossingLanes = 2;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Wide first", fore, 0, 4.0f),
			world.createAgent("Wide second", fore, 0, 4.0f),
			world.createAgent("Disabled waiter", fore, 0, 4.0f)
		};
		for (auto id : ids) world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		bool disabledWithTwoCrossings = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 2 || snapshot.traversalResources.front().crossingLanes.size() != 2)
				return false;
			if (!disabledWithTwoCrossings && snapshot.traversalPermits.size() == 2)
			{
				auto const& lanes = snapshot.traversalResources.front().crossingLanes;
				if (!lanes[0].owner || !lanes[1].owner || lanes[0].owner == lanes[1].owner
					|| snapshot.traversalResources.front().crossingLeaseCount != 2)
					return false;
				disabledWithTwoCrossings = world.setTraversalResourceEnabled(created.traversalResource, false);
			}
			if (disabledWithTwoCrossings
				&& world.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}
		world.advanceTicks(3);
		auto snapshot = world.getSimulationSnapshot();
		auto denied = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		return disabledWithTwoCrossings
			&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(ids[1]).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(ids[2]).entity->getSector() == world.getSector(fore).get()
			&& denied != snapshot.traversalRequests.end()
			&& denied->state == core::TraversalRequestState::Denied
			&& denied->failureReason == core::TraversalFailureReason::ResourceDisabled
			&& snapshot.traversalResources.front().crossingLeaseCount == 0;
	}

	// Ticket #97: the band predicate itself - within crossingWidth in x of the
	// threshold, on the threshold row in y.
	bool doorCrossingBandPredicateShape()
	{
		auto const width = CORE_DOOR_CROSSING_HALF_WIDTH(3);
		if (std::abs(width - 1.2f) > 0.0001f) return false;
		if (std::abs(CORE_DOOR_CROSSING_HALF_WIDTH(1) - 0.2f) > 0.0001f) return false;

		auto const threshold = core::Vector2{ 4.5f, 0.0f };
		if (!core::isWithinDoorCrossingBand({ 4.5f, 0.0f }, threshold, width)) return false;
		if (!core::isWithinDoorCrossingBand({ 3.3f, 0.0f }, threshold, width)) return false;
		if (!core::isWithinDoorCrossingBand({ 5.7f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 3.2f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 5.8f, 0.0f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 4.5f, 0.1f }, threshold, width)) return false;
		if (core::isWithinDoorCrossingBand({ 4.5f, -0.01f }, threshold, width)) return false;
		return true;
	}

	// Ticket #97: Door vertices carry the crossing width derived from the
	// physical doorway (cell width minus the x insets) minus the agent width.
	bool doorVertexCarriesCrossingWidth()
	{
		core::World world("Crossing width vertices", 8, 2);
		world.addRoom("Width fore", 0, 0, 0, 7, 1);
		world.addRoom("Width back", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions wide;
		wide.width = 3;
		world.addSectorDoor(0, 0, 1);
		world.addSectorDoor(0, 0, 3, wide);
		world.finishBuild();

		bool foundNarrow = false;
		bool foundWide = false;
		for (auto const& vertex : world.getGraph()->getVertices())
		{
			auto doorVertex = std::dynamic_pointer_cast<const core::DoorVertex>(vertex);
			if (!doorVertex || !doorVertex->getDoor()) continue;
			if (doorVertex->getDoor()->getCellsWide() == 1)
			{
				foundNarrow = std::abs(doorVertex->getCrossingWidth() - 0.2f) <= 0.0001f;
				if (!foundNarrow) return false;
			}
			else if (doorVertex->getDoor()->getCellsWide() == 3)
			{
				foundWide = std::abs(doorVertex->getCrossingWidth() - 1.2f) <= 0.0001f;
				if (!foundWide) return false;
			}
		}
		return foundNarrow && foundWide;
	}

	// Runs a contended manual door with a blocker and a waiter that enters the
	// flow at the band, and records the waiter's request and grant moments
	// relative to the band.
	struct CrossingBandTrace
	{
		std::string text;
		bool createdWithinBand{ false };
		bool grantedBeforeCentre{ false };
		bool grantedWithinBand{ false };
		bool crossedOver{ false };
	};

	CrossingBandTrace runCrossingWidthGrantScenario(uint32_t cellsWide)
	{
		CrossingBandTrace trace;
		core::World world("Crossing width grant", 10, 2);
		auto fore = world.addRoom("Band fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Band back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = cellsWide;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(cellsWide);

		auto blockerId = world.createAgent("Band blocker", fore, 0, 7.0f);
		auto waiterId = world.createAgent("Band waiter", fore, 0, 8.0f);
		world.lookupAgent(blockerId).entity->setPath(twoNodePath(source, destination, edge), true);
		world.lookupAgent(waiterId).entity->setPath(twoNodePath(source, destination, edge), true);

		bool firstObservation = true;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto waiter = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == waiterId; });
			auto request = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == waiterId; });
			if (waiter == snapshot.agents.end() || request == snapshot.traversalRequests.end())
			{
				continue;
			}
			trace.text += std::to_string(tick) + ':' + std::to_string((int)request->state) + ':'
				+ std::to_string(request->queuePosition) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(waiter->globalPosition.x)) + ':'
				+ std::to_string(snapshot.traversalPermits.size()) + ';';
			auto const hasPermit = std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == request->id; });
			auto const dx = std::abs(waiter->globalPosition.x - centre.x);
			// Ticket #98: the request-creation gate is the band itself, so the
			// waiter's first observed request sits inside the band.
			if (firstObservation)
			{
				firstObservation = false;
				trace.createdWithinBand = dx <= crossingWidth + 0.001f;
			}
			if (!hasPermit)
			{
				continue;
			}
			trace.grantedWithinBand = dx <= crossingWidth + 0.001f
				&& std::abs(waiter->globalPosition.y - centre.y) <= 0.001f;
			// The waiter approaches from the right; "before centre" means it is
			// still short of the door centre by a clear margin.
			trace.grantedBeforeCentre = waiter->globalPosition.x > centre.x + 0.25f;
			break;
		}
		world.advanceTicks(MaximumSimulationTicks);
		trace.crossedOver = world.lookupAgent(waiterId).entity->getSector()
			== world.getSector(back).get();
		return trace;
	}

	// Ticket #97/#98: at a wide door the head of queue enters the flow inside
	// the crossing band and is granted from there without reaching its
	// assigned centre position, and repeated runs produce identical traces.
	bool crossingWidthGrantsHeadOfQueueBeforeCentre()
	{
		auto const first = runCrossingWidthGrantScenario(3);
		auto const second = runCrossingWidthGrantScenario(3);
		return first.createdWithinBand && first.grantedWithinBand && first.grantedBeforeCentre
			&& first.crossedOver && !first.text.empty() && first.text == second.text;
	}

	// Ticket #97/#98: at a 1-cell door the band is only +/-0.2. With the
	// request-creation gate on the band the waiter enters the flow at the
	// band edge and is granted within the +/-0.2 tolerance of the centre -
	// never before it - and repeated runs are identical.
	bool narrowDoorBandArrivalGrantsAtCentreTolerance()
	{
		auto const first = runCrossingWidthGrantScenario(1);
		auto const second = runCrossingWidthGrantScenario(1);
		return first.createdWithinBand && first.grantedWithinBand
			&& !first.grantedBeforeCentre && first.crossedOver
			&& !first.text.empty() && first.text == second.text;
	}

	// Ticket #98: a lone Agent approaching an open wide Door enters the
	// traversal flow - request created, queue ticket taken - as soon as it is
	// within the crossing width at the threshold row, and crosses from where
	// it stands without ever converging on the door centre.
	struct BandEntryTrace
	{
		std::string text;
		bool createdWithinBand{ false };
		bool createdOnThresholdRow{ false };
		bool createdOffCentre{ false };
		bool grantedOffCentre{ false };
		bool neverNearedCentre{ false };
		bool crossedOver{ false };
	};

	BandEntryTrace runBandEntryScenario(uint32_t cellsWide)
	{
		BandEntryTrace trace;
		core::World world("Band entry crossing", 10, 2);
		auto fore = world.addRoom("Entry fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Entry back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = cellsWide;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		// Hold the door open so the grant lands as soon as the request exists.
		if (!world.acquireDoorOpenLease(created.traversalResource)) return trace;

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(cellsWide);

		auto agentId = world.createAgent("Band arriver", fore, 0, centre.x + 2.5f);
		world.lookupAgent(agentId).entity->setPath(twoNodePath(source, destination, edge), true);

		auto minDx = 1000.0f;
		bool requestObserved = false;
		bool granted = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			if (world.lookupAgent(agentId).entity->getSector() == world.getSector(back).get())
			{
				break;
			}
			auto snapshot = world.getSimulationSnapshot();
			auto agent = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == agentId; });
			if (agent == snapshot.agents.end()) return trace;
			auto const dx = std::abs(agent->globalPosition.x - centre.x);
			minDx = std::min(minDx, dx);
			auto request = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == agentId; });
			if (request == snapshot.traversalRequests.end()) continue;
			trace.text += std::to_string(tick) + ':' + std::to_string((int)request->state) + ':'
				+ std::to_string(request->queuePosition) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(agent->globalPosition.x)) + ';';
			if (!requestObserved)
			{
				requestObserved = true;
				trace.createdWithinBand = dx <= crossingWidth + 0.001f;
				trace.createdOnThresholdRow
					= std::abs(agent->globalPosition.y - centre.y) <= 0.001f;
				trace.createdOffCentre = dx > 0.5f;
			}
			if (!granted && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == request->id; }))
			{
				granted = true;
				trace.grantedOffCentre = dx > 0.5f;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		trace.neverNearedCentre = minDx > 0.5f;
		trace.crossedOver = world.lookupAgent(agentId).entity->getSector()
			== world.getSector(back).get();
		return trace;
	}

	bool bandArrivalCrossesWideDoorFromStandingPosition()
	{
		auto const first = runBandEntryScenario(3);
		auto const second = runBandEntryScenario(3);
		return first.createdWithinBand && first.createdOnThresholdRow && first.createdOffCentre
			&& first.grantedOffCentre && first.neverNearedCentre && first.crossedOver
			&& !first.text.empty() && first.text == second.text;
	}

	// Ticket #98: band arrival composes with the queue and the existing early
	// stop at a contended wide door. A second Agent joins while the first is
	// still waiting inside the band: both requests share the queue, the grant
	// follows ticket order on the single crossing lane, and neither Agent is
	// stranded between the gates.
	bool bandArrivalComposesWithEarlyStopForContendedDoor()
	{
		core::World world("Band contention", 10, 2);
		auto fore = world.addRoom("Contended fore", 0, 0, 0, 9, 1);
		auto back = world.addRoom("Contended back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = 3;
		options.crossingLanes = 1;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto const centre = source->getPosition();
		auto const crossingWidth = CORE_DOOR_CROSSING_HALF_WIDTH(3);

		auto firstId = world.createAgent("Contended first", fore, 0, 7.0f);
		world.lookupAgent(firstId).entity->setPath(twoNodePath(source, destination, edge), true);

		core::AgentId secondId{};
		bool overlappedPending{ false };
		bool secondCreatedOffCentre{ false };
		bool secondHadQueuePosition{ false };
		bool firstGrantedBeforeSecond{ false };
		int firstGrantTick = -1;
		int secondGrantTick = -1;
		std::string text;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto firstRequest = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == firstId; });
			// Spawn the second Agent just outside the band once the first is
			// queued inside it, so the two requests contend for one lane.
			if (!secondId && firstRequest != snapshot.traversalRequests.end()
				&& firstRequest->state == core::TraversalRequestState::Pending
				&& firstRequest->hasQueuePosition)
			{
				secondId = world.createAgent("Contended second", fore, 0,
					centre.x + crossingWidth + 0.25f);
				world.lookupAgent(secondId).entity->setPath(
					twoNodePath(source, destination, edge), true);
			}
			if (!secondId) continue;
			auto secondRequest = std::find_if(snapshot.traversalRequests.begin(),
				snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == secondId; });
			auto secondAgent = std::find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == secondId; });
			if (secondRequest == snapshot.traversalRequests.end()
				|| secondAgent == snapshot.agents.end())
			{
				continue;
			}
			text += std::to_string(tick) + ':' + std::to_string((int)firstRequest->state) + ':'
				+ std::to_string((int)secondRequest->state) + ':'
				+ std::to_string(std::bit_cast<uint32_t>(secondAgent->globalPosition.x)) + ';';
			if (!secondCreatedOffCentre)
			{
				secondCreatedOffCentre
					= std::abs(secondAgent->globalPosition.x - centre.x) > 0.25f;
			}
			secondHadQueuePosition = secondHadQueuePosition || secondRequest->hasQueuePosition;
			overlappedPending = overlappedPending
				|| (firstRequest->state == core::TraversalRequestState::Pending
					&& secondRequest->state == core::TraversalRequestState::Pending);
			if (firstGrantTick < 0 && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == firstRequest->id; }))
			{
				firstGrantTick = (int)tick;
			}
			if (secondGrantTick < 0 && std::any_of(snapshot.traversalPermits.begin(),
				snapshot.traversalPermits.end(),
				[&](auto const& permit) { return permit.request == secondRequest->id; }))
			{
				secondGrantTick = (int)tick;
				firstGrantedBeforeSecond = firstGrantTick >= 0 && firstGrantTick < secondGrantTick;
			}
			if (firstGrantedBeforeSecond && secondGrantTick >= 0
				&& world.lookupAgent(firstId).entity->getSector() == world.getSector(back).get()
				&& world.lookupAgent(secondId).entity->getSector() == world.getSector(back).get())
			{
				break;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		return overlappedPending && secondCreatedOffCentre && secondHadQueuePosition
			&& firstGrantedBeforeSecond
			&& world.lookupAgent(firstId).entity->getSector() == world.getSector(back).get()
			&& world.lookupAgent(secondId).entity->getSector() == world.getSector(back).get()
			&& !text.empty();
	}

	// Ticket #98: the band only arms an Agent whose next edge crosses the
	// Door. An Agent walking through the band's x range at the threshold row
	// with no intent to cross never creates a traversal request.
	bool bandArrivalLeavesNonCrossingAgentsUnaffected()
	{
		core::World world("Band passer by", 10, 2);
		auto fore = world.addRoom("Passer fore", 0, 0, 0, 9, 1);
		world.addRoom("Passer back", 1, 0, 0, 9, 1);
		core::World::CreateDoorOptions options;
		options.width = 3;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		uint32_t pastDoorId;
		world.addSectorMarker(fore, 0, 8.0f, &pastDoorId);
		world.finishBuild();

		auto walker = world.createAgent("Passer by", fore, 0, 1.0f);
		auto walkerEntity = world.lookupAgent(walker).entity;
		auto target = world.getGraph()->getVertexByIdentifier(pastDoorId);
		if (!target) return false;
		auto path = world.getGraph()->calculatePath(walkerEntity, target);
		if (!path) return false;
		walkerEntity->setPath(std::move(path), true);

		bool crossedBandRow = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			// Only a crossing intent arms the Door gate; the walker's ordinary
			// Location-edge requests must never target the door resource.
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == walker && request.resource == created.traversalResource)
					return false;
			auto const x = walkerEntity->getGlobalPosition().x;
			crossedBandRow = crossedBandRow || (x > 3.3f && x < 5.7f);
			if (walkerEntity->getState() == core::Agent::State::Idle) break;
		}
		return crossedBandRow && walkerEntity->getGlobalPosition().x > 7.0f;
	}

	bool doorLeasesAndSensorObservationsPreventUnsafeClosure()
	{
		core::World world("Door observation safety", 7, 2);
		auto fore = world.addRoom("Sensor fore", 0, 0, 0, 6, 1);
		world.addRoom("Sensor back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		options.holdOpenSeconds = core::World::getFixedTimestep() * 2.0f;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto sensor = core::DoorSensorId{ 1 };
		if (!world.setDoorSensorObservation(created.traversalResource, sensor,
			core::DoorSensorObservation::Presence)) return false;
		world.advanceTicks(90);
		auto lease = world.acquireDoorOpenLease(created.traversalResource);
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		world.advanceTicks(30);
		auto snapshot = world.getSimulationSnapshot();
		if (!lease || snapshot.traversalResources.front().doorState != core::DoorSnapshotState::Open
			|| snapshot.traversalResources.front().externalOpenLeaseCount != 1) return false;

		auto actor = world.createAgent("Close operator", fore, 0, 3.5f);
		core::DeviceCommand close;
		close.type = core::DeviceCommandType::OpenDoor;
		close.desiredState = false;
		close.traversalResource = created.traversalResource;
		auto point = world.createInteractionPoint("Close door", core::SectorId{ (uint64_t)fore + 1 },
			{ 3.5f, 0.0f }, 1.0f, 0.0f, { { close, core::InteractionBindingRequirement::Required } });
		auto closeRequest = world.requestInteraction(point, actor);
		world.advanceTicks(4);
		if (world.lookupInteractionRequest(closeRequest).entity->getResult() != core::InteractionResult::Rejected
			|| !world.releaseDoorOpenLease(created.traversalResource, lease)) return false;

		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		world.advanceTicks(20);
		if (world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Open) return false;
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		for (uint32_t i = 0; i < 10 && world.getSimulationSnapshot().traversalResources.front().doorState
			!= core::DoorSnapshotState::Closing; ++i) world.advanceTick();
		if (world.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closing) return false;
		world.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		world.advanceTick();
		snapshot = world.getSimulationSnapshot();
		return snapshot.traversalResources.front().obstructionObserved
			&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening;
	}

	bool finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed()
	{
		core::World world("Finite ladder", 4, 4);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(1, 0, 3);
		core::World::CreateLadderOptions options{ 2, false, true };
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();
		if (!created.traversalResource) return false;

		auto const& graph = world.getGraph();
		auto target = graph->getClosestVertexInSector(world.getSector(upper).get(), { 1.5f, 1.0f });
		if (!target) return false;
		std::vector<core::AgentId> ids = {
			world.createAgent("First climber", lower, 0, 1.5f),
			world.createAgent("Second climber", lower, 0, 1.5f),
			world.createAgent("Cancelled climber", lower, 0, 1.5f)
		};
		for (auto id : ids)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedFull = false;
		bool observedQueuePosition = false;
		bool cancelledWaiter = false;
		uint64_t climbStarted = 0;
		uint64_t climbFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isLadder
				|| std::abs(resource->agentSpacing
					- CORE_LADDER_AGENT_SPACING / CORE_CELL_YX_RENDER_RATIO) > 0.001f
				|| resource->capacity != 1 || resource->queueLanes.size() != 2
				|| resource->occupantCount + resource->admissionReservationCount > resource->capacity
				|| resource->capacityPositions.size() != resource->capacity)
				return false;
			for (auto const& lane : resource->queueLanes)
				if (any_of(lane.positions.begin(), lane.positions.end(), [&](auto const& position)
					{ return position.position.distanceTo(lane.origin) <= 0.001f; })) return false;
			observedQueuePosition = observedQueuePosition
				|| any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
					[&](auto const& request) { return request.resource == created.traversalResource
						&& request.hasQueuePosition && !request.hasCapacityPosition; });
			if (resource->occupantCount == 1)
			{
				observedFull = true;
				if (!climbStarted && world.lookupAgent(ids[0]).entity->getState()
					== core::Agent::State::TraversingEdge)
					climbStarted = world.getSimulationTick();
				if (!cancelledWaiter)
				{
					world.lookupAgent(ids[2]).entity->clearPath();
					cancelledWaiter = true;
				}
			}
			if (climbStarted && !climbFinished
				&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(upper).get())
				climbFinished = world.getSimulationTick();
			if (world.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}

		auto snapshot = world.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		// One vertical unit at 0.25 units/second requires about 240 fixed ticks;
		// this also detects accidentally using walking speed.
		return observedFull && observedQueuePosition && cancelledWaiter && climbStarted && climbFinished
			&& climbFinished - climbStarted >= 230
			&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(ids[1]).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(ids[2]).entity->getSector() == world.getSector(lower).get()
			&& resource != snapshot.traversalResources.end()
			&& resource->occupantCount == 0 && resource->admissionReservationCount == 0
			&& resource->admissionQueue.empty();
	}

	bool ladderQueuePositionsPreferAgentApproachSide()
	{
		core::World world("Ladder queue approach", 8, 4);
		auto lower = world.addCorridor(0, 0, 7);
		auto upper = world.addCorridor(1, 0, 7);
		core::World::CreateLadderOptions options{ 2, false, true };
		auto created = world.addLadder(1, 0, 3, options);
		uint32_t lowerApproachId, upperApproachId;
		world.addSectorMarker(lower, 0, 1.0f, &lowerApproachId);
		world.addSectorMarker(upper, 0, 6.0f, &upperApproachId);
		world.finishBuild();

		auto lowerApproach = world.getGraph()->getVertexByIdentifier(lowerApproachId);
		auto upperApproach = world.getGraph()->getVertexByIdentifier(upperApproachId);
		auto upperTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 3.5f, 1.0f });
		auto lowerTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(lower).get(), { 3.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;

		// Occupy the sole Ladder position so later Agents must claim queue spots
		// before entering the queue footprint.
		auto blocker = world.createAgent("Current climber", lower, 0, 3.5f);
		auto blockerEntity = world.lookupAgent(blocker).entity;
		auto blockerPath = world.getGraph()->calculatePath(blockerEntity, upperTarget);
		if (!blockerPath) return false;
		blockerEntity->setPath(std::move(blockerPath), true);
		for (uint32_t tick = 0; tick < MaximumSimulationTicks
			&& blockerEntity->getSector() != created.ladder.sector.get(); ++tick)
			world.advanceTick();
		if (blockerEntity->getSector() != created.ladder.sector.get()) return false;

		auto lowerAgent = world.createAgent("Lower left approach", lower, 0, 1.0f);
		auto upperAgent = world.createAgent("Upper right approach", upper, 0, 6.0f);
		auto assignPath = [&](core::AgentId id, auto const& source, auto const& target)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, source, target);
			if (!path) return false;
			agent->setPath(std::move(path), true);
			return true;
		};
		if (!assignPath(lowerAgent, lowerApproach, upperTarget)
			|| !assignPath(upperAgent, upperApproach, lowerTarget)) return false;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalRequests.size() < 2) continue;
			auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end()) return false;
			auto lowerRequest = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == lowerAgent; });
			auto upperRequest = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == upperAgent; });
			if (lowerRequest == snapshot.traversalRequests.end()
				|| upperRequest == snapshot.traversalRequests.end()
				|| !lowerRequest->hasQueuePosition || !upperRequest->hasQueuePosition) continue;
			auto const& lowerLane = resource->queueLanes[lowerRequest->queueApproach];
			auto const& upperLane = resource->queueLanes[upperRequest->queueApproach];
			auto const lowerSpot = lowerLane.positions[lowerRequest->queuePosition].position;
			auto const upperSpot = upperLane.positions[upperRequest->queuePosition].position;
			auto lowerSnapshot = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& agent) { return agent.id == lowerAgent; });
			auto upperSnapshot = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& agent) { return agent.id == upperAgent; });
			return lowerSnapshot != snapshot.agents.end() && upperSnapshot != snapshot.agents.end()
				&& lowerSnapshot->globalPosition.x < lowerLane.origin.x
				&& upperSnapshot->globalPosition.x > upperLane.origin.x
				&& lowerSpot.x >= lowerSnapshot->globalPosition.x - 0.001f
				&& upperSpot.x <= upperSnapshot->globalPosition.x + 0.001f
				&& lowerSpot.x < lowerLane.origin.x && upperSpot.x > upperLane.origin.x;
		}
		return false;
	}

	bool forceBridgePreparationUsesNearControl()
	{
		core::World world("Near Force Bridge control", 6, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 5, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 1);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 4);
		core::World::CreateForceBridgeOptions options{ 1, CORE_SIDE_LEFT, true, false, 2 };
		auto bridge = world.addSectorForceBridge(room, 1, 2, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == bridge.traversalResource; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto left = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto right = edge->getOtherVertex(left);
		auto agentId = world.createAgent("Right-side operator", room, 1,
			right->getSectorOffset().x);
		world.lookupAgent(agentId).entity->setPath(twoNodePath(right, left, edge), true);

		for (uint32_t tick = 0; tick < 10; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto interaction = std::find_if(snapshot.interactionRequests.begin(),
				snapshot.interactionRequests.end(), [&](auto const& candidate)
					{ return candidate.actor == agentId; });
			if (interaction == snapshot.interactionRequests.end()) continue;
			auto selected = std::find_if(snapshot.interactionPoints.begin(),
				snapshot.interactionPoints.end(), [&](auto const& candidate)
					{ return candidate.id == interaction->point; });
			if (selected == snapshot.interactionPoints.end()) return false;
			float nearestDistance = std::numeric_limits<float>::max();
			for (auto const pointId : snapshot.traversalResources.front().controls)
			{
				auto point = std::find_if(snapshot.interactionPoints.begin(),
					snapshot.interactionPoints.end(), [&](auto const& candidate)
						{ return candidate.id == pointId; });
				if (point != snapshot.interactionPoints.end())
					nearestDistance = std::min(nearestDistance,
						point->position.distanceTo(right->getPosition()));
			}
			return selected->position.distanceTo(right->getPosition())
				<= nearestDistance + 0.001f;
		}
		return false;
	}

	bool extendedForceBridgeAllowsConcurrentTwoWayTraffic()
	{
		core::World world("Concurrent Force Bridge", 8, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 6, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 1, 4);
		world.addSectorWalkway(room, 1, 5);
		core::World::CreateForceBridgeOptions options{ 2, CORE_SIDE_LEFT, true, true, 1 };
		auto bridge = world.addSectorForceBridge(room, 1, 1, options);
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == bridge.traversalResource; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto left = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto right = edge->getOtherVertex(left);

		std::vector<core::AgentId> leftToRight;
		std::vector<core::AgentId> rightToLeft;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto forward = world.createAgent("Forward " + std::to_string(i), room, 1,
				left->getSectorOffset().x);
			auto reverse = world.createAgent("Reverse " + std::to_string(i), room, 1,
				right->getSectorOffset().x);
			world.lookupAgent(forward).entity->setPath(twoNodePath(left, right, edge), true);
			world.lookupAgent(reverse).entity->setPath(twoNodePath(right, left, edge), true);
			leftToRight.push_back(forward);
			rightToLeft.push_back(reverse);
		}

		bool sawConcurrentTwoWayTraffic = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			uint32_t forwardPermits = 0;
			uint32_t reversePermits = 0;
			for (auto const& permit : snapshot.traversalPermits)
			{
				if (std::find(leftToRight.begin(), leftToRight.end(), permit.owner) != leftToRight.end())
					++forwardPermits;
				if (std::find(rightToLeft.begin(), rightToLeft.end(), permit.owner) != rightToLeft.end())
					++reversePermits;
			}
			sawConcurrentTwoWayTraffic = sawConcurrentTwoWayTraffic
				|| (forwardPermits == leftToRight.size() && reversePermits == rightToLeft.size());
			if (std::all_of(leftToRight.begin(), leftToRight.end(), [&](auto id)
					{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })
				&& std::all_of(rightToLeft.begin(), rightToLeft.end(), [&](auto id)
					{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })) break;
		}

		return sawConcurrentTwoWayTraffic
			&& std::all_of(leftToRight.begin(), leftToRight.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getGlobalPosition().distanceTo(right->getPosition()) < 0.001f; })
			&& std::all_of(rightToLeft.begin(), rightToLeft.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getGlobalPosition().distanceTo(left->getPosition()) < 0.001f; });
	}

	bool extensibleForceBridgeCompletesThroughPhysicalControl()
	{
		core::World world("Extensible force bridge", 6, 4);
		auto room = world.addRoom("Bridge room", 1, 0, 0, 4, 3);
		world.addSectorWalkway(room, 1, 0);
		world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		core::World::CreateForceBridgeOptions options;
		options.fromSide = CORE_SIDE_LEFT;
		options.extensible = true;
		options.startExtended = false;
		options.controlCount = 1;
		auto bridge = world.addSectorForceBridge(room, 1, 1, options);
		auto bridgeObject = std::dynamic_pointer_cast<core::ForceBridgeSectorObject>(
			bridge.forceBridge.sector->getObject(bridge.forceBridge.index));
		if (!bridgeObject) return false;
		auto forceBridge = bridgeObject->getForceBridge();
		world.finishBuild();

		auto edgeIt = std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::ForceBridge; });
		if (edgeIt == world.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto operatorId = world.createAgent("Bridge operator", room, 1, 0.5f);
		auto followerId = world.createAgent("Bridge follower", room, 1, 0.1f);
		auto bridgeOperator = world.lookupAgent(operatorId).entity;
		auto follower = world.lookupAgent(followerId).entity;
		auto operatorPath = world.getGraph()->calculatePath(bridgeOperator, source, destination);
		auto followerPath = world.getGraph()->calculatePath(follower, source, destination);
		if (!operatorPath || !followerPath) return false;
		bridgeOperator->setPath(std::move(operatorPath), true);
		follower->setPath(std::move(followerPath), true);

		bool sawPreparation = false;
		bool sawExtensionLease = false;
		bool sawQueueStops = false;
		bool sawFollowerQueueWhileExtending = false;
		bool sawFullyExtendedBeforeCrossing = false;
		while ((bridgeOperator->getState() != core::Agent::State::Idle
				|| follower->getState() != core::Agent::State::Idle)
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == bridge.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isForceBridge
				|| !resource->isExtensible) return false;
			// Operating the wall-mounted control must not pull an Agent off the floor.
			if (std::abs(bridgeOperator->getGlobalPosition().y - source->getPosition().y) > 0.001f
				|| std::abs(follower->getGlobalPosition().y - source->getPosition().y) > 0.001f)
				return false;
			if (resource->preparationOperator
				&& bridgeOperator->getGlobalPosition().distanceTo(source->getPosition()) > 0.001f)
				return false;
			sawPreparation = sawPreparation || !snapshot.deviceOperations.empty();
			sawExtensionLease = sawExtensionLease || resource->extensionRequestLeaseCount > 0;
			sawQueueStops = sawQueueStops || (resource->queueLanes.size() == 2
				&& !resource->queueLanes[0].positions.empty()
				&& !resource->queueLanes[1].positions.empty());
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == followerId && request.queueTicket && request.hasQueuePosition
					&& !forceBridge->isExtended())
					sawFollowerQueueWhileExtending = true;

			auto crossing = bridgeOperator->getState() == core::Agent::State::TraversingEdge
				|| follower->getState() == core::Agent::State::TraversingEdge;
			if (crossing && !forceBridge->isExtended()) return false;
			sawFullyExtendedBeforeCrossing = sawFullyExtendedBeforeCrossing
				|| (crossing && forceBridge->isExtended());
		}
		auto final = world.getSimulationSnapshot();
		auto resource = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& value) { return value.id == bridge.traversalResource; });
		return sawPreparation && sawExtensionLease && sawQueueStops && sawFollowerQueueWhileExtending
			&& sawFullyExtendedBeforeCrossing
			&& bridgeOperator->getState() == core::Agent::State::Idle
			&& follower->getState() == core::Agent::State::Idle
			&& bridgeOperator->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& follower->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& resource != final.traversalResources.end() && resource->extended
			&& resource->extensionRequestLeaseCount == 0
			&& resource->extensionOccupantLeaseCount == 0
			&& final.traversalRequests.empty() && final.traversalPermits.empty();
	}

	bool ladderAdmissionsMaintainPhysicalSpacing()
	{
		// Mirrors resources/test-worlds/sector-ladder-test-1.world.yaml: twelve Agents cross
		// a four-level Ladder in both directions. Admission must stagger entry so
		// equal-speed climbers never overlap on the span.
		core::World world("Ladder spacing", 16, 6);
		auto lower = world.addCorridor(1, 0, 16);
		auto upper = world.addCorridor(4, 0, 16);
		core::World::CreateLadderOptions options{ 4, false, true };
		options.directionalBatchLimit = 4;
		auto created = world.addLadder(1, 1, 8, options);
		world.finishBuild();
		if (!created.traversalResource || !created.ladder.sector) return false;

		auto graph = world.getGraph();
		auto upperRight = graph->getClosestVertexInSector(world.getSector(upper).get(), { 15.5f, 4.0f });
		auto upperLeft = graph->getClosestVertexInSector(world.getSector(upper).get(), { 0.5f, 4.0f });
		auto lowerRight = graph->getClosestVertexInSector(world.getSector(lower).get(), { 15.5f, 1.0f });
		auto lowerLeft = graph->getClosestVertexInSector(world.getSector(lower).get(), { 0.5f, 1.0f });
		if (!upperRight || !upperLeft || !lowerRight || !lowerLeft) return false;

		std::vector<core::AgentId> ids;
		auto addAgent = [&](char const* name, uint32_t sector, float x,
			std::shared_ptr<const core::Vertex> const& target)
		{
			auto id = world.createAgent(name, sector, 0, x);
			auto agent = world.lookupAgent(id).entity;
			if (!agent) return false;
			auto path = graph->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
			ids.push_back(id);
			return true;
		};
		if (!addAgent("Lower Left 1", lower, 1.25f, upperRight)
			|| !addAgent("Lower Left 2", lower, 2.0f, upperRight)
			|| !addAgent("Lower Left 3", lower, 2.75f, upperRight)
			|| !addAgent("Lower Right 1", lower, 14.75f, upperLeft)
			|| !addAgent("Lower Right 2", lower, 14.0f, upperLeft)
			|| !addAgent("Lower Right 3", lower, 13.25f, upperLeft)
			|| !addAgent("Upper Left 1", upper, 1.25f, lowerRight)
			|| !addAgent("Upper Left 2", upper, 2.0f, lowerRight)
			|| !addAgent("Upper Left 3", upper, 2.75f, lowerRight)
			|| !addAgent("Upper Right 1", upper, 14.75f, lowerLeft)
			|| !addAgent("Upper Right 2", upper, 14.0f, lowerLeft)
			|| !addAgent("Upper Right 3", upper, 13.25f, lowerLeft)) return false;

		auto ladderSector = created.ladder.sector;
		float minimumSeparation = std::numeric_limits<float>::max();
		bool observedConcurrentClimbers = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 8; ++i)
		{
			world.advanceTick();
			std::vector<core::Vector2> climbers;
			for (auto id : ids)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getSector() == ladderSector.get())
					climbers.push_back(agent->getGlobalPosition());
			}
			observedConcurrentClimbers = observedConcurrentClimbers || climbers.size() > 1;
			for (uint32_t a = 0; a < climbers.size(); ++a)
				for (uint32_t b = a + 1; b < climbers.size(); ++b)
					minimumSeparation = std::min(minimumSeparation,
						climbers[a].distanceTo(climbers[b]));
			if (std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; }))
				break;
		}
		return observedConcurrentClimbers
			&& minimumSeparation >= CORE_AGENT_MAX_HEIGHT - 0.001f
			&& std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; });
	}

	bool extensibleLadderUsesDesiredStateAndLeases()
	{
		core::World world("Extensible ladder", 4, 4);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(2, 0, 3);
		core::World::CreateLadderOptions options{ 3, true, false };
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 1.5f, 2.0f });
		auto first = world.createAgent("Extension owner", lower, 0, 1.5f);
		auto second = world.createAgent("Shared extension owner", lower, 0, 1.5f);
		for (auto id : { first, second })
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool sawSharedOperation = false;
		bool sawLease = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isExtensible) return false;
			sawLease = sawLease || resource->extensionRequestLeaseCount > 0
				|| resource->extensionOccupantLeaseCount > 0;
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SetExtendedState
					&& operation.command.desiredState && operation.requesters.size() == 2)
					sawSharedOperation = true;
			if (world.lookupAgent(first).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(second).entity->getState() == core::Agent::State::Idle) break;
		}
		auto snapshot = world.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		return sawSharedOperation && sawLease && resource != snapshot.traversalResources.end()
			&& resource->extended && resource->extensionRequestLeaseCount == 0
			&& resource->extensionOccupantLeaseCount == 0
			&& world.lookupAgent(first).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(second).entity->getSector() == world.getSector(upper).get();
	}

	bool directionalLadderBoundsBatchesAndPreventsOpposingAdmission()
	{
		core::World world("Directional ladder", 4, 5);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(3, 0, 3);
		core::World::CreateLadderOptions options{ 4, false, true };
		options.directionalBatchLimit = 4;
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();

		auto graph = world.getGraph();
		auto upperTarget = graph->getClosestVertexInSector(world.getSector(upper).get(), { 1.5f, 3.0f });
		auto lowerTarget = graph->getClosestVertexInSector(world.getSector(lower).get(), { 1.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;
		std::vector<core::AgentId> ascending = {
			world.createAgent("Ascending one", lower, 0, 1.5f),
			world.createAgent("Ascending two", lower, 0, 1.5f),
			world.createAgent("Ascending three", lower, 0, 1.5f),
			world.createAgent("Ascending four", lower, 0, 1.5f),
			world.createAgent("Ascending next batch", lower, 0, 1.5f)
		};
		auto descending = world.createAgent("Descending waiter", upper, 0, 1.5f);
		for (auto id : ascending)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, upperTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}
		{
			auto agent = world.lookupAgent(descending).entity;
			auto path = graph->calculatePath(agent, lowerTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedFourConcurrent = false;
		bool observedFullBatch = false;
		uint64_t fourthAscendingFinished = 0;
		uint64_t descendingFinished = 0;
		uint64_t fifthAscendingFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 4; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || resource->capacity != 5
				|| resource->occupantCount + resource->admissionReservationCount > 5
				|| resource->directionalBatchLimit != 4) return false;

			core::TraversalDirection admittedDirection = core::TraversalDirection::None;
			for (auto const& request : snapshot.traversalRequests)
			{
				if (request.resource != created.traversalResource || !request.hasCapacityPosition) continue;
				if (admittedDirection != core::TraversalDirection::None
					&& admittedDirection != request.direction) return false;
				admittedDirection = request.direction;
			}
			if (resource->activeDirection != core::TraversalDirection::None
				&& admittedDirection != core::TraversalDirection::None
				&& resource->activeDirection != admittedDirection) return false;
			observedFourConcurrent = observedFourConcurrent
				|| (resource->activeDirection == core::TraversalDirection::Ascending
					&& resource->occupantCount + resource->admissionReservationCount == 4);
			observedFullBatch = observedFullBatch
				|| (resource->activeDirection == core::TraversalDirection::Ascending
					&& resource->descendingWaitingCount == 1
					&& resource->directionalBatchCount == 4);
			if (!fourthAscendingFinished && world.lookupAgent(ascending[3]).entity->getState() == core::Agent::State::Idle)
				fourthAscendingFinished = world.getSimulationTick();
			if (!descendingFinished && world.lookupAgent(descending).entity->getState() == core::Agent::State::Idle)
				descendingFinished = world.getSimulationTick();
			if (!fifthAscendingFinished && world.lookupAgent(ascending[4]).entity->getState() == core::Agent::State::Idle)
				fifthAscendingFinished = world.getSimulationTick();
			if (fourthAscendingFinished && descendingFinished && fifthAscendingFinished) break;
		}
		return observedFourConcurrent && observedFullBatch
			&& fourthAscendingFinished && descendingFinished && fifthAscendingFinished
			&& fourthAscendingFinished < descendingFinished
			&& descendingFinished < fifthAscendingFinished;
	}

	bool stairwellCoordinationIsExplicitlyOptIn()
	{
		core::World ordinary("Ordinary stairwell", 5, 3);
		ordinary.addCorridor(0, 0, 4);
		ordinary.addCorridor(1, 0, 4);
		ordinary.addStairwell(1, 0, 1, 2, CORE_SIDE_LEFT);
		ordinary.finishBuild();
		if (!ordinary.getSimulationSnapshot().traversalResources.empty()) return false;

		core::World narrow("Narrow stairwell", 5, 3);
		narrow.addCorridor(0, 0, 4);
		narrow.addCorridor(1, 0, 4);
		core::World::CreateStairwellOptions options{ 2, CORE_SIDE_LEFT };
		options.directionalCapacity = 1;
		options.directionalBatchLimit = 3;
		auto created = narrow.addStairwell(1, 0, 1, options);
		narrow.finishBuild();
		auto snapshot = narrow.getSimulationSnapshot();
		if (!created.traversalResource || snapshot.traversalResources.size() != 1
			|| !snapshot.traversalResources.front().isNarrowStairwell
			|| snapshot.traversalResources.front().capacity != 1
			|| snapshot.traversalResources.front().directionalBatchLimit != 3) return false;
		return std::all_of(narrow.getGraph()->getEdges().begin(), narrow.getGraph()->getEdges().end(),
			[&](auto const& edge)
			{
				return edge->getType() != core::EdgeType::Stairwell
					|| edge->getTraversalResourceId() == created.traversalResource;
			});
	}

	bool platformLiftAuthoringReconcilesWalkwayStops()
	{
		{
			core::World offset("PlatformLift initial level", 8, 6);
			auto offsetRoom = offset.addRoom("Offset room", 0, 1, 0, 7, 4);
			offset.addSectorWalkway(offsetRoom, 2, 2);
			offset.addSectorWalkway(offsetRoom, 2, 3);
			core::World::CreateLiftOptions offsetOptions;
			offsetOptions.stopOffsets = { 0, 2 };
			auto placed = offset.addSectorPlatformLift(offsetRoom, 0, 2, offsetOptions);
			auto object = std::dynamic_pointer_cast<const core::LiftSectorObject>(
				placed.lift.sector->getObject(placed.lift.index));
			offset.finishBuild();
			auto snapshot = offset.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == placed.traversalResource; });
			std::shared_ptr<const core::SectorObject> hitObject;
			auto hit = offset.getObjectAtPosition(0, 2.5f, 0.975f, &hitObject);
			if (!object || std::abs(object->getLift()->getPosition().y - 1.0f) > 0.001f
				|| resource == snapshot.traversalResources.end()
				|| std::abs(resource->liftPosition - 1.0f) > 0.001f
				|| hit.get() != object->getLift().get() || hitObject != object) return false;
		}
		core::World world("PlatformLift authoring", 8, 5);
		auto room = world.addRoom("Lift room", 0, 0, 0, 7, 4);
		world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 3, 2);
		world.addSectorWalkway(room, 3, 3);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1; options.stopOffsets = { 0, 1 };
		auto created = world.addSectorPlatformLift(room, 0, 2, options);
		world.finishBuild(); world.pauseSimulation();
		options.stopOffsets = { 0, 1, 3 };
		auto edit = world.planPlatformLiftEdit(room, created.lift.index, options);
		if (!edit.valid) return false;
		auto liftObject = world.applyPlatformLiftEdit(edit);
		if (!liftObject) return false;

		auto findObject = [&](core::SectorObjectType type, uint32_t x, uint32_t y)
		{
			auto sector = world.getSector(room);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (object && object->getObjectType() == type
					&& object->getCellX() == x && object->getCellY() == y) return i;
			}
			return ~0u;
		};
		auto lower = findObject(core::SectorObjectType::Walkway, 2, 1);
		auto removal = world.planRemoveSectorWalkway(room, lower);
		if (!removal.valid || !removal.consequences.empty() || !world.applyWalkwayEdit(removal)) return false;
		auto liftIndex = findObject(core::SectorObjectType::Lift, 2, 0);
		core::World::CreateLiftOptions retained;
		if (liftIndex == ~0u || !world.getPlatformLiftOptions(room, liftIndex, retained)
			|| retained.stopOffsets != std::vector<uint32_t>({ 0, 3 })) return false;
		auto upper = findObject(core::SectorObjectType::Walkway, 2, 3);
		removal = world.planRemoveSectorWalkway(room, upper);
		if (!removal.valid || removal.consequences.empty() || !world.applyWalkwayEdit(removal)) return false;
		if (findObject(core::SectorObjectType::Lift, 2, 0) != ~0u) return false;

		core::World resized("PlatformLift resize", 8, 5);
		auto resizedRoom = resized.addRoom("Lift room", 0, 0, 0, 7, 4);
		resized.addSectorWalkway(resizedRoom, 1, 2); resized.addSectorWalkway(resizedRoom, 1, 3);
		resized.addSectorWalkway(resizedRoom, 3, 2); resized.addSectorWalkway(resizedRoom, 3, 3);
		options.stopOffsets = { 0, 1, 3 };
		resized.addSectorPlatformLift(resizedRoom, 0, 2, options);
		resized.finishBuild(); resized.pauseSimulation();
		auto resize = resized.planResizeLocation(resizedRoom, 0, 0, 7, 2);
		if (!resize.valid || resize.consequences.empty()) return false;
		resizedRoom = resized.applyLocationEdit(resize);
		uint32_t resizedLift = ~0u;
		for (uint32_t i = 0; i < resized.getSector(resizedRoom)->getNumObjects(); ++i)
			if (auto object = resized.getSector(resizedRoom)->getObject(i);
				object && object->getObjectType() == core::SectorObjectType::Lift) resizedLift = i;
		if (resizedLift == ~0u || !resized.getPlatformLiftOptions(resizedRoom, resizedLift, retained)
			|| retained.stopOffsets != std::vector<uint32_t>({ 0, 1 })) return false;
		resize = resized.planResizeLocation(resizedRoom, 0, 0, 7, 1);
		if (!resize.valid || std::none_of(resize.consequences.begin(), resize.consequences.end(),
			[](auto const& value) { return value.find("Platform Lift") != std::string::npos; })) return false;
		resizedRoom = resized.applyLocationEdit(resize);
		for (uint32_t i = 0; i < resized.getSector(resizedRoom)->getNumObjects(); ++i)
			if (auto object = resized.getSector(resizedRoom)->getObject(i);
				object && object->getObjectType() == core::SectorObjectType::Lift) return false;

		core::World moving("PlatformLift movement", 9, 5);
		auto movingRoom = moving.addRoom("Lift room", 0, 0, 0, 8, 4);
		moving.addSectorWalkway(movingRoom, 1, 1); moving.addSectorWalkway(movingRoom, 1, 2);
		moving.addSectorWalkway(movingRoom, 2, 3); moving.addSectorWalkway(movingRoom, 2, 4);
		options.stopOffsets = { 0, 1 };
		auto movingLift = moving.addSectorPlatformLift(movingRoom, 0, 1, options);
		moving.finishBuild(); moving.pauseSimulation();
		auto move = moving.planMoveSectorObject(movingRoom, movingLift.lift.index, 3, 0);
		if (!move.valid || !move.requiresConfirmation()) return false;
		auto movedLift = moving.applyObjectMove(move);
		uint32_t movedLiftIndex = ~0u;
		for (uint32_t i = 0; i < moving.getSector(movingRoom)->getNumObjects(); ++i)
			if (moving.getSector(movingRoom)->getObject(i) == movedLift) movedLiftIndex = i;
		if (movedLiftIndex == ~0u || !moving.getPlatformLiftOptions(movingRoom, movedLiftIndex, retained)
			|| retained.stopOffsets != std::vector<uint32_t>({ 0, 2 })) return false;
		uint32_t connectedWalkway = ~0u;
		for (uint32_t i = 0; i < moving.getSector(movingRoom)->getNumObjects(); ++i)
		{
			auto object = moving.getSector(movingRoom)->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::Walkway
				&& object->getCellX() == 3 && object->getCellY() == 2) connectedWalkway = i;
		}
		move = moving.planMoveSectorObject(movingRoom, connectedWalkway, 6, 2);
		if (!move.valid || !move.requiresConfirmation() || !moving.applyObjectMove(move)) return false;
		for (uint32_t i = 0; i < moving.getSector(movingRoom)->getNumObjects(); ++i)
			if (auto object = moving.getSector(movingRoom)->getObject(i);
				object && object->getObjectType() == core::SectorObjectType::Lift) return false;
		return true;
	}

	bool openPlatformLiftUsesVirtualBoundaryAndTransportPolicy()
	{
		core::World world("Open platform lift", 7, 5);
		auto room = world.addRoom("Platform room", 0, 0, 0, 6, 4);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.platformStopDurationSeconds = 2.0f;
		world.addSectorWalkway(room, 2, 0);
		world.addSectorWalkway(room, 2, 1);
		world.addSectorWalkway(room, 2, 2);
		world.addSectorWalkway(room, 2, 3);
		auto created = world.addSectorPlatformLift(room, 0, 2, options);
		uint32_t destinationVertexId;
		world.addSectorMarker(room, 2, 0.5f, &destinationVertexId);
		world.finishBuild();
		if (!created.traversalResource || !created.interiorSelector || created.buttons.size() != 2)
			return false;

		auto target = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto passengerId = world.createAgent("Platform passenger", room, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path || std::count_if(path->nodes.begin(), path->nodes.end(), [](auto const& node)
			{ return node.edge && node.edge->getType() == core::EdgeType::Lift; }) != 1) return false;
		passenger->setPath(path, true);

		bool sawPhysicalQueuePosition = false;
		bool sawOnboard = false;
		bool sawBoardingCrossing = false;
		bool boardingStayedAtWalkSpeed = true;
		bool fullyInsideWhenRegistered = false;
		bool registeredAtAssignedPosition = false;
		bool reachedAssignedPosition = false;
		bool exitedTowardNextVertex = false;
		bool checkedExitDirection = false;
		bool wasOnboard = false;
		bool sawAttachedMotion = false;
		bool sawDestinationConfirmation = false;
		bool sawConfiguredStopDuration = false;
		auto previousPosition = passenger->getGlobalPosition();
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 6
			&& passenger->getState() != core::Agent::State::Idle; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto platform = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (platform == snapshot.traversalResources.end() || !platform->isOpenPlatformLift
				|| platform->liftCarDoorOpen
				|| platform->occupantCount + platform->admissionReservationCount > options.capacity)
				return false;
			if (platform->queueLanes.size() != options.stopOffsets.size()
				|| std::any_of(platform->queueLanes.begin(), platform->queueLanes.end(),
					[](auto const& lane) { return lane.positions.empty(); })) return false;
			sawPhysicalQueuePosition = sawPhysicalQueuePosition
				|| std::any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
					[&](auto const& request)
					{ return request.owner == passengerId && request.hasQueuePosition; });
			auto expectedStopTicks = (uint64_t)ceil(options.platformStopDurationSeconds
				/ world.getFixedTimestep());
			if (platform->liftBoardingCutoffTick >= platform->liftServiceStartedTick
				&& platform->liftBoardingCutoffTick - platform->liftServiceStartedTick
					== expectedStopTicks) sawConfiguredStopDuration = true;
			auto onboard = platform->occupantCount == 1;
			sawOnboard = sawOnboard || onboard;
			sawBoardingCrossing = sawBoardingCrossing
				|| (!onboard && platform->virtualBoundaryCrossingCount == 1);
			if (platform->virtualBoundaryCrossingCount > 1) return false;
			auto const assignedX = world.getSector(room)->getPosition().x
				+ platform->capacityPositions.front().position.x;
			if (onboard && !wasOnboard)
			{
				boardingStayedAtWalkSpeed = passenger->getGlobalPosition().distanceTo(previousPosition)
					<= passenger->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
				auto const centerX = passenger->getGlobalPosition().x;
				fullyInsideWhenRegistered = centerX - CORE_AGENT_MAX_WIDTH * 0.5f >= 2.0f - 0.001f
					&& centerX + CORE_AGENT_MAX_WIDTH * 0.5f <= 3.0f + 0.001f;
				registeredAtAssignedPosition = std::abs(centerX - assignedX) < 0.01f;
			}
			if (onboard)
			{
				reachedAssignedPosition = reachedAssignedPosition
					|| std::abs(passenger->getGlobalPosition().x - assignedX) < 0.01f;
			}
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SelectLiftDestination
					&& operation.state == core::DeviceOperationState::Succeeded)
					sawDestinationConfirmation = true;
			if (platform->liftMoving)
			{
				if (platform->virtualBoundaryCrossingCount != 0) return false;
				if (std::abs(passenger->getGlobalPosition().y - platform->liftPosition) < 0.001f)
					sawAttachedMotion = true;
			}
			auto horizontalStep = passenger->getGlobalPosition().x - previousPosition.x;
			if (!checkedExitDirection && platform->liftCurrentStop == 1 && !platform->liftMoving
				&& passenger->getState() == core::Agent::State::TraversingEdge
				&& std::abs(horizontalStep) > 0.0001f)
			{
				checkedExitDirection = true;
				exitedTowardNextVertex = horizontalStep < 0.0f;
			}
			wasOnboard = onboard;
			previousPosition = passenger->getGlobalPosition();
		}
		auto final = world.getSimulationSnapshot();
		auto platform = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawPhysicalQueuePosition && sawOnboard
			&& sawConfiguredStopDuration
			&& sawBoardingCrossing && boardingStayedAtWalkSpeed
			&& fullyInsideWhenRegistered && registeredAtAssignedPosition
			&& reachedAssignedPosition
			&& exitedTowardNextVertex && sawAttachedMotion && sawDestinationConfirmation
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == world.getSector(room).get()
			&& passenger->getGlobalPosition().distanceTo(target->getPosition()) < 0.001f
			&& platform != final.traversalResources.end() && platform->occupantCount == 0
			&& platform->virtualBoundaryCrossingCount == 0;
	}

	bool openPlatformLiftCrossingLaneSpreadsPassengers()
	{
		core::World world("Platform lift crossing lane", 7, 4);
		auto room = world.addRoom("Platform room", 0, 0, 0, 6, 3);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.platformStopDurationSeconds = 10.0f;
		for (uint32_t x = 0; x < 6; ++x) world.addSectorWalkway(room, 2, x);
		auto created = world.addSectorPlatformLift(room, 0, 2, options);
		uint32_t destinationVertexId;
		world.addSectorMarker(room, 2, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		std::vector<core::Agent*> passengers;
		for (float x : { 0.5f, 1.0f })
		{
			auto id = world.createAgent("Platform passenger", room, 0, x);
			auto passenger = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(passenger, destination);
			if (!path) return false;
			passenger->setPath(path, true);
			passengers.push_back(passenger);
		}

		if (std::abs(CORE_PLATFORM_LIFT_CROSSING_HALF_WIDTH(options.cellsWide) * 2.0f
			- ((float)options.cellsWide - CORE_AGENT_MAX_WIDTH)) > 0.001f) return false;
		bool sawTwoPassengersSpread = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 12; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto platform = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (platform == snapshot.traversalResources.end()
				|| platform->virtualBoundaryCrossingCount > 1) return false;
			if (platform->occupantCount == 2)
			{
				auto const roomX = world.getSector(room)->getPosition().x;
				bool bothAtSeparatedPositions = true;
				for (auto const& slot : platform->capacityPositions)
				{
					if (!slot.occupant) { bothAtSeparatedPositions = false; break; }
					auto passenger = world.lookupAgent(slot.occupant).entity;
					if (!passenger || std::abs(passenger->getGlobalPosition().x
						- (roomX + slot.position.x)) > 0.01f)
					{ bothAtSeparatedPositions = false; break; }
				}
				sawTwoPassengersSpread = sawTwoPassengersSpread || bothAtSeparatedPositions;
			}
			if (std::all_of(passengers.begin(), passengers.end(), [](auto passenger)
				{ return passenger->getState() == core::Agent::State::Idle; })) break;
		}
		return sawTwoPassengersSpread
			&& std::all_of(passengers.begin(), passengers.end(), [&](auto passenger)
			{
				return passenger->getState() == core::Agent::State::Idle
					&& passenger->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f;
			});
	}

	bool openPlatformLiftUsesOneJourneyAcrossIntermediateStops()
	{
		core::World world("Multi-stop open platform lift", 7, 5);
		auto room = world.addRoom("Platform room", 0, 0, 0, 6, 4);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 1, 2 };
		options.capacity = 1;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		for (uint32_t x = 0; x < 4; ++x) world.addSectorWalkway(room, 2, x);
		auto created = world.addSectorPlatformLift(room, 0, 2, options);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(room, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(room, 2, 0.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto passengerId = world.createAgent("Multi-stop platform passenger", room, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, source, destination);
		if (!path || std::count_if(path->nodes.begin(), path->nodes.end(), [](auto const& node)
			{ return node.edge && node.edge->getType() == core::EdgeType::Lift; }) != 2) return false;
		passenger->setPath(path, true);

		bool passedIntermediateLevelWhileMoving = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8
			&& passenger->getState() != core::Agent::State::Idle; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto platform = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (platform == snapshot.traversalResources.end()) return false;
			if (platform->liftCurrentStop == 1 && !platform->liftMoving) return false;
			passedIntermediateLevelWhileMoving = passedIntermediateLevelWhileMoving
				|| (platform->liftMoving && std::abs(platform->liftPosition - 1.0f) < 0.01f);
		}
		return passedIntermediateLevelWhileMoving
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f;
	}

	bool singlePassengerCompletesTwoStopLiftJourney()
	{
		core::World world("Two-stop lift journey", 6, 4);
		auto lower = world.addCorridor(0, 0, 5);
		auto upper = world.addCorridor(2, 0, 5);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		auto created = world.addLift(1, 0, 2, options);
		world.finishBuild();
		if (!created.traversalResource || created.doors.size() != 2 || !created.interiorSelector)
			return false;
		auto initial = world.getSimulationSnapshot();
		auto initialLift = std::find_if(initial.traversalResources.begin(),
			initial.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		if (initialLift == initial.traversalResources.end() || initialLift->capacity != 2) return false;
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 2.5f, 2.0f });
		auto passengerId = world.createAgent("Lift passenger", lower, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path) return false;
		uint32_t boardingEdges = 0, rideEdges = 0;
		for (auto const& node : path->nodes)
		{
			if (!node.edge) continue;
			boardingEdges += node.edge->getType() == core::EdgeType::Door;
			rideEdges += node.edge->getType() == core::EdgeType::Lift;
		}
		if (boardingEdges != 2 || rideEdges != 1) return false;
		passenger->setPath(path, true);

		bool sawIntentWithoutDispatch = false;
		bool sawReservedCapacity = false;
		bool sawOnboard = false;
		bool enteredAtWalkSpeed = false;
		bool enteredBeforeAssignedPosition = false;
		bool walkedToAssignedPosition = false;
		bool sawConfirmedDestination = false;
		bool sawMovingAttachedPassenger = false;
		bool sawQueuedDebug = false, sawEnteringDebug = false;
		bool sawInLiftDebug = false, sawExitingDebug = false;
		bool climbedTowardLandingCallButton = false;
		bool wasOnboard = false;
		auto previousPosition = passenger->getGlobalPosition();
		for (uint32_t i = 0; i < MaximumSimulationTicks * 4
			&& passenger->getState() != core::Agent::State::Idle; ++i)
		{
			world.advanceTick();
			if (passenger->getSector() == world.getSector(lower).get()
				&& passenger->getGlobalPosition().y > 0.001f)
				climbedTowardLandingCallButton = true;
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end() || !lift->isLift) return false;
			for (auto const& debug : lift->liftAgents)
			{
				if (debug.agent != passengerId || debug.targetStop != 1
					|| std::abs(debug.targetLevel - 2.0f) > 0.001f) continue;
				sawQueuedDebug = sawQueuedDebug
					|| debug.state == core::LiftAgentState::QueuingAtDoor;
				sawEnteringDebug = sawEnteringDebug
					|| debug.state == core::LiftAgentState::Entering;
				sawInLiftDebug = sawInLiftDebug
					|| debug.state == core::LiftAgentState::InLift;
				sawExitingDebug = sawExitingDebug
					|| debug.state == core::LiftAgentState::Exiting;
			}
			if (!snapshot.traversalRequests.empty() && !lift->liftPassenger
				&& std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(),
					[](auto const& operation)
					{
						return operation.command.type == core::DeviceCommandType::CallLift
							&& operation.state == core::DeviceOperationState::Pending;
					}))
				sawIntentWithoutDispatch = true;
			sawReservedCapacity = sawReservedCapacity || lift->admissionReservationCount == 1;
			auto const onboard = lift->liftPassenger == passengerId
				&& passenger->getSector() == world.getSector(created.lift.sector->getIndex()).get();
			sawOnboard = sawOnboard || onboard;
			auto assigned = std::find_if(lift->capacityPositions.begin(), lift->capacityPositions.end(),
				[&](auto const& position) { return position.occupant == passengerId; });
			if (onboard && assigned != lift->capacityPositions.end())
			{
				auto const assignedX = world.getSector(created.lift.sector->getIndex())->getPosition().x
					+ assigned->position.x;
				if (!wasOnboard)
				{
					enteredAtWalkSpeed = passenger->getGlobalPosition().distanceTo(previousPosition)
						<= passenger->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
					enteredBeforeAssignedPosition = std::abs(
						passenger->getGlobalPosition().x - assignedX) > 0.01f;
				}
				else if (std::abs(passenger->getGlobalPosition().x - previousPosition.x) > 0.0001f)
				{
					if (passenger->getGlobalPosition().distanceTo(previousPosition)
						> passenger->getWalkSpeed() * world.getFixedTimestep() + 0.001f) return false;
					walkedToAssignedPosition = walkedToAssignedPosition
						|| std::abs(passenger->getGlobalPosition().x - assignedX) < 0.01f;
				}
			}
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SelectLiftDestination
					&& operation.state == core::DeviceOperationState::Succeeded)
					sawConfirmedDestination = true;
			if (lift->liftMoving)
			{
				for (auto const& resource : snapshot.traversalResources)
					if (resource.isDoor && (!resource.crossingLanes.empty()
						&& (resource.crossingOwner || resource.doorState != core::DoorSnapshotState::Closed)))
						return false;
				if (std::abs(passenger->getGlobalPosition().y - lift->liftPosition) < 0.001f)
					sawMovingAttachedPassenger = true;
			}
			wasOnboard = onboard;
			previousPosition = passenger->getGlobalPosition();
		}
		auto final = world.getSimulationSnapshot();
		auto lift = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawIntentWithoutDispatch && sawReservedCapacity && sawOnboard
			&& enteredAtWalkSpeed && enteredBeforeAssignedPosition && walkedToAssignedPosition
			&& sawConfirmedDestination && sawMovingAttachedPassenger
			&& sawQueuedDebug && sawEnteringDebug && sawInLiftDebug && sawExitingDebug
			&& !climbedTowardLandingCallButton
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == world.getSector(upper).get()
			&& lift != final.traversalResources.end() && !lift->liftPassenger
			&& lift->occupantCount == 0;
	}

	bool liftDoorQueueRequestsBeforeOccupiedTail()
	{
		core::World world("Early Lift Door queue", 8, 4);
		auto lower = world.addCorridor(0, 0, 7);
		auto upper = world.addCorridor(2, 0, 7);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 1;
		auto created = world.addLift(1, 0, 3, options);
		uint32_t approachId;
		world.addSectorMarker(lower, 0, 2.75f, &approachId);
		world.finishBuild();

		auto upperTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 3.5f, 2.0f });
		auto lowerTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(lower).get(), { 3.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;

		// Send the sole-capacity car away with an occupant so the lower landing
		// queue remains unavailable while the following Agents approach it.
		auto rider = world.createAgent("Descending rider", upper, 0, 3.5f);
		auto riderEntity = world.lookupAgent(rider).entity;
		auto riderPath = world.getGraph()->calculatePath(riderEntity, lowerTarget);
		if (!riderPath) return false;
		riderEntity->setPath(std::move(riderPath), true);
		bool descending = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (lift != snapshot.traversalResources.end() && lift->liftMoving
				&& lift->liftDirection == core::TraversalDirection::Descending
				&& lift->occupantCount == 1)
			{
				descending = true;
				break;
			}
		}
		if (!descending) return false;

		auto blocker = world.createAgent("Lift queue head", lower, 0, 3.5f);
		auto blockerEntity = world.lookupAgent(blocker).entity;
		auto blockerPath = world.getGraph()->calculatePath(blockerEntity, upperTarget);
		if (!blockerPath) return false;
		blockerEntity->setPath(std::move(blockerPath), true);
		bool queueEstablished = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto landing = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.doors.front().traversalResource; });
			if (landing != snapshot.traversalResources.end()
				&& any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
					[&](auto const& request) { return request.resource == created.doors.front().traversalResource
						&& request.state == core::TraversalRequestState::Pending
						&& request.hasQueuePosition; }))
			{
				queueEstablished = true;
				break;
			}
		}
		if (!queueEstablished) return false;

		auto approach = world.getGraph()->getVertexByIdentifier(approachId);
		auto waiter = world.createAgent("Lift waiter", lower, 0, 2.75f);
		auto waiterEntity = world.lookupAgent(waiter).entity;
		auto path = world.getGraph()->calculatePath(waiterEntity, approach, upperTarget);
		if (!path) return false;
		waiterEntity->setPath(std::move(path), true);
		bool requestedBeforeOccupiedTail = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			auto const positionBeforeTick = waiterEntity->getGlobalPosition();
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto request = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == waiter
					&& value.resource == created.doors.front().traversalResource
					&& value.state == core::TraversalRequestState::Pending; });
			if (request == snapshot.traversalRequests.end()) continue;
			auto landing = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.doors.front().traversalResource; });
			if (landing == snapshot.traversalResources.end()) return false;
			if (request->queueApproach >= landing->queueLanes.size()) continue;
			auto const& lane = landing->queueLanes[request->queueApproach];
			requestedBeforeOccupiedTail = requestedBeforeOccupiedTail
				|| positionBeforeTick.x < lane.origin.x;
			if (!request->hasQueuePosition) continue;
			auto const target = lane.positions[request->queuePosition].position;
			return requestedBeforeOccupiedTail && target.x <= lane.origin.x + 0.001f;
		}
		return false;
	}

	bool liftCallOperatorDoesNotFightItsQueuePosition()
	{
		core::World world("Lift call operator queue", 16, 3);
		auto bottom = world.addCorridor(0, 0, 16);
		auto middle = world.addCorridor(1, 0, 16);
		auto top = world.addCorridor(2, 0, 16);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 1, 2 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.75f;
		options.maximumBoardingSeconds = 5.0f;
		world.addLift(1, 0, 8, options);
		uint32_t bottomTargetId, middleTargetId, topTargetId;
		world.addSectorMarker(bottom, 0, 0.5f, &bottomTargetId);
		world.addSectorMarker(middle, 0, 0.5f, &middleTargetId);
		world.addSectorMarker(top, 0, 0.5f, &topTargetId);
		world.finishBuild();

		auto graph = world.getGraph();
		auto bottomTarget = graph->getVertexByIdentifier(bottomTargetId);
		auto middleTarget = graph->getVertexByIdentifier(middleTargetId);
		auto topTarget = graph->getVertexByIdentifier(topTargetId);
		struct Group { uint32_t sector; std::shared_ptr<const core::Vertex> target; char const* name; };
		Group groups[] = {
			{ bottom, topTarget, "Bottom Right" },
			{ middle, bottomTarget, "Middle Right" },
			{ top, middleTarget, "Top Right" }
		};
		std::vector<core::AgentId> agents;
		for (auto const& group : groups)
			for (uint32_t i = 0; i < 3; ++i)
			{
				auto id = world.createAgent(
					std::string(group.name) + " " + std::to_string(i + 1),
					group.sector, 0, 14.75f - i * 0.75f);
				auto agent = world.lookupAgent(id).entity;
				auto path = graph->calculatePath(agent, group.target);
				if (!path) return false;
				agent->setPath(std::move(path), true);
				agents.push_back(id);
			}

		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 12; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (std::any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[](auto const& request)
				{ return request.failureReason == core::TraversalFailureReason::LocalGoalUnreachable; }))
				return false;
			if (std::all_of(agents.begin(), agents.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; }))
				return world.lookupAgent(agents[4]).entity->getSector()
					== world.getSector(bottom).get();
		}
		return false;
	}

	bool waitingLiftPassengersFillArrivingCar()
	{
		core::World world("Arriving lift boards waiting capacity", 7, 4);
		auto lower = world.addCorridor(0, 0, 6);
		auto upper = world.addCorridor(2, 0, 6);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = world.addLift(1, 0, 2, options);
		world.finishBuild();

		auto lowerTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(lower).get(), { 2.5f, 0.0f });
		auto upperTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 2.5f, 2.0f });
		if (!lowerTarget || !upperTarget) return false;
		auto downId = world.createAgent("Down passenger", upper, 0, 2.0f);
		auto down = world.lookupAgent(downId).entity;
		auto downPath = world.getGraph()->calculatePath(down, lowerTarget);
		if (!downPath) return false;
		down->setPath(downPath, true);

		bool descending = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift != snapshot.traversalResources.end() && lift->liftMoving
				&& lift->liftDirection == core::TraversalDirection::Descending
				&& lift->occupantCount == 1)
			{ descending = true; break; }
		}
		if (!descending) return false;

		for (uint32_t i = 0; i < 2; ++i)
		{
			auto id = world.createAgent("Waiting passenger", lower, 0, 1.7f - i * 0.35f);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, upperTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}
		bool sawBothWaiting = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 5; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end()) return false;
			auto waiting = std::count_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request)
				{ return request.resource == created.doors.front().traversalResource
					&& request.state == core::TraversalRequestState::Pending; });
			sawBothWaiting = sawBothWaiting || waiting == 2;
			if (lift->liftMoving && lift->liftDirection == core::TraversalDirection::Ascending
				&& lift->liftCurrentStop == 0)
				return sawBothWaiting && lift->occupantCount == options.capacity;
		}
		return false;
	}

	bool liftCapacityAndStopPhasesAreEnforced()
	{
		core::World world("Finite lift", 7, 4);
		auto lower = world.addCorridor(0, 0, 6);
		auto upper = world.addCorridor(2, 0, 6);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = world.addLift(1, 0, 2, options);
		world.finishBuild();
		auto initial = world.getSimulationSnapshot();
		for (auto const& door : created.doors)
		{
			auto resource = std::find_if(initial.traversalResources.begin(),
				initial.traversalResources.end(),
				[&](auto const& candidate) { return candidate.id == door.traversalResource; });
			if (resource == initial.traversalResources.end()) return false;
			bool foundCarLane = false, foundCorridorLane = false;
			for (auto const& lane : resource->queueLanes)
			{
				auto sector = world.getSector((uint32_t)lane.sector.value - 1);
				if (sector->getIndex() == created.lift.sector->getIndex())
				{
					foundCarLane = true;
					auto landingY = door.door.sector->getObject(door.door.index)->getCellY();
					if (lane.positions.size() != options.capacity
						|| std::any_of(lane.positions.begin(), lane.positions.end(),
							[landingY](auto const& position)
							{ return std::abs(position.position.y - landingY) > 0.001f; })) return false;
				}
				else
				{
					foundCorridorLane = true;
					if (lane.positions.size() <= options.capacity) return false;
				}
			}
			if (!foundCarLane || !foundCorridorLane) return false;
		}
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 2.5f, 2.0f });
		std::vector<core::AgentId> passengers;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto id = world.createAgent("Capacity passenger", lower, 0, 0.3f + i * 0.15f);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
			passengers.push_back(id);
		}

		bool sawFullCarWithWaitingPassenger = false;
		bool sawCutoffHonorReservations = false;
		bool sawDistinctCorridorQueuePositions = false;
		bool checkedFirstDepartureCapacity = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			std::vector<core::Vector2> corridorQueueTargets;
			for (auto const& request : snapshot.traversalRequests)
			{
				if (request.resource != created.doors.front().traversalResource) continue;
				if (request.state == core::TraversalRequestState::Pending
					&& request.hasCapacityPosition && !request.hasQueuePosition) return false;
				if (request.hasQueuePosition)
					corridorQueueTargets.push_back(request.queuePositionTarget);
			}
			if (corridorQueueTargets.size() >= 2)
			{
				for (size_t i = 0; i < corridorQueueTargets.size(); ++i)
					for (size_t j = i + 1; j < corridorQueueTargets.size(); ++j)
						if (corridorQueueTargets[i].distanceTo(corridorQueueTargets[j])
							< CORE_DOOR_QUEUE_STOP_WIDTH - 0.001f) return false;
				sawDistinctCorridorQueuePositions = true;
			}
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end()
				|| lift->occupantCount + lift->admissionReservationCount > options.capacity)
				return false;
			if (!checkedFirstDepartureCapacity && lift->liftMoving
				&& lift->liftCurrentStop == 0
				&& lift->liftDirection == core::TraversalDirection::Ascending)
			{
				checkedFirstDepartureCapacity = true;
				if (lift->occupantCount != options.capacity) return false;
			}
			if (lift->occupantCount == options.capacity && !lift->admissionQueue.empty())
				sawFullCarWithWaitingPassenger = true;
			if (snapshot.tick > lift->liftBoardingCutoffTick && lift->admissionReservationCount > 0)
				sawCutoffHonorReservations = true;
			if (std::all_of(passengers.begin(), passengers.end(), [&](auto id)
				{
					auto agent = world.lookupAgent(id).entity;
					return agent && agent->getState() == core::Agent::State::Idle
						&& agent->getSector() == world.getSector(upper).get();
				}))
			{
				return sawFullCarWithWaitingPassenger && sawCutoffHonorReservations
					&& sawDistinctCorridorQueuePositions && checkedFirstDepartureCapacity;
			}
		}
		return false;
	}

	bool multiStopLiftUsesDeterministicLookScheduling()
	{
		core::World world("LOOK lift", 7, 7);
		auto lower = world.addCorridor(0, 0, 6);
		auto middle = world.addCorridor(2, 0, 6);
		auto upper = world.addCorridor(5, 0, 6);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2, 5 }; // deliberately non-uniform
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 3.0f;
		auto created = world.addLift(1, 0, 2, options);
		world.finishBuild();

		auto graph = world.getGraph();
		auto lowerTarget = graph->getClosestVertexInSector(world.getSector(lower).get(), { 2.5f, 0.0f });
		auto middleTarget = graph->getClosestVertexInSector(world.getSector(middle).get(), { 2.5f, 2.0f });
		auto upperTarget = graph->getClosestVertexInSector(world.getSector(upper).get(), { 2.5f, 5.0f });
		if (!lowerTarget || !middleTarget || !upperTarget) return false;

		struct Journey { core::AgentId id; std::shared_ptr<const core::Vertex> target; };
		std::vector<Journey> journeys = {
			{ world.createAgent("Up through run", lower, 0, 2.5f), upperTarget },
			{ world.createAgent("Down middle", middle, 0, 2.5f), lowerTarget },
			{ world.createAgent("Down upper", upper, 0, 2.5f), middleTarget }
		};
		for (auto const& journey : journeys)
		{
			auto agent = world.lookupAgent(journey.id).entity;
			auto path = graph->calculatePath(agent, journey.target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		std::vector<uint32_t> serviceOrder;
		core::LiftStopPhase previousPhase = core::LiftStopPhase::Idle;
		float previousPosition = 0.0f;
		bool observedCoalescedMiddleDemand = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 12; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end()) return false;
			if (lift->liftStopRequestOwnerCounts.size() != 3
				|| lift->liftStopOldestRequestTicks.size() != 3) return false;
			observedCoalescedMiddleDemand = observedCoalescedMiddleDemand
				|| lift->liftStopRequestOwnerCounts[1] >= 2;
			if (lift->liftStopPhase == core::LiftStopPhase::Opening
				&& previousPhase != core::LiftStopPhase::Opening)
				serviceOrder.push_back(lift->liftCurrentStop);
			if (lift->liftMoving)
			{
				if (lift->liftDirection == core::TraversalDirection::Ascending
					&& lift->liftPosition + 0.0001f < previousPosition) return false;
				if (lift->liftDirection == core::TraversalDirection::Descending
					&& lift->liftPosition > previousPosition + 0.0001f) return false;
			}
			previousPhase = lift->liftStopPhase;
			previousPosition = lift->liftPosition;
			if (std::all_of(journeys.begin(), journeys.end(), [&](auto const& journey)
				{ return world.lookupAgent(journey.id).entity->getState() == core::Agent::State::Idle; }))
				break;
		}

		return observedCoalescedMiddleDemand
			&& serviceOrder == std::vector<uint32_t>({ 0, 2, 1, 0 })
			&& world.lookupAgent(journeys[0].id).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(journeys[1].id).entity->getSector() == world.getSector(lower).get()
			&& world.lookupAgent(journeys[2].id).entity->getSector() == world.getSector(middle).get();
	}

	bool shuttlePassengerWalksToForwardInteriorSpot()
	{
		core::World world("Shuttle interior walking", 16, 2);
		auto left = world.addRoom("Left platform", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right platform", 0, 0, 10, 4, 1);
		core::World::CreateShuttleOptions options{ 1, 4, { 0, 10 }, 0 };
		options.capacity = 3;
		options.doorMask = 0b0001;
		options.minimumDwellSeconds = 0.0f;
		options.maximumBoardingSeconds = 0.1f;
		auto created = world.addShuttle(1, 0, 0, 15, options);
		world.finishBuild();

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 11.5f, 0.0f });
		if (!target) return false;
		auto passengerId = world.createAgent("Walking shuttle passenger", left, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path) return false;
		passenger->setPath(path, true);

		bool boardedWithoutTeleport = false;
		bool selectedForwardmostSpot = false;
		bool reachedInteriorSpot = false;
		bool walkedWhileShuttleMoving = false;
		bool wasOnboard = false;
		float previousX = passenger->getGlobalPosition().x;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()
				|| shuttle->shuttleCarriages.size() != 1
				|| shuttle->shuttleCarriages.front().positions.size() != options.capacity) return false;

			auto onboard = passenger->getSector()
				== world.getSector(created.shuttle.sector->getIndex()).get();
			if (onboard && !wasOnboard)
			{
				boardedWithoutTeleport = std::abs(passenger->getGlobalPosition().x - previousX)
					<= passenger->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
				auto const& positions = shuttle->shuttleCarriages.front().positions;
				selectedForwardmostSpot = positions.back().occupant == passengerId;
			}
			if (onboard)
			{
				auto const& forward = shuttle->shuttleCarriages.front().positions.back().position;
				auto passengerCarriageX = passenger->getGlobalPosition().x - shuttle->liftPosition;
				reachedInteriorSpot = reachedInteriorSpot
					|| std::abs(passengerCarriageX - forward.x) < 0.01f;
				walkedWhileShuttleMoving = walkedWhileShuttleMoving
					|| (shuttle->liftMoving && passengerCarriageX < forward.x - 0.01f);
			}
			wasOnboard = onboard;
			previousX = passenger->getGlobalPosition().x;
			if (passenger->getState() == core::Agent::State::Idle
				&& passenger->getSector() == world.getSector(right).get()) break;
		}
		return boardedWithoutTeleport && selectedForwardmostSpot && reachedInteriorSpot
			&& walkedWhileShuttleMoving && passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == world.getSector(right).get();
	}

	bool shuttlePassengersSpreadAcrossCarriageAtWalkingSpeed()
	{
		core::World world("Shuttle passenger spacing", 16, 2);
		auto left = world.addRoom("Left platform", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right platform", 0, 0, 10, 4, 1);
		core::World::CreateShuttleOptions options{ 1, 4, { 0, 10 }, 0 };
		options.capacity = 3;
		options.doorMask = 0b0001;
		options.minimumDwellSeconds = 20.0f;
		options.maximumBoardingSeconds = 30.0f;
		auto created = world.addShuttle(1, 0, 0, 15, options);
		world.finishBuild();

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 11.5f, 0.0f });
		if (!target) return false;
		std::vector<core::AgentId> passengers;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto id = world.createAgent("Spacing passenger", left, 0, 0.4f + i * 0.7f);
			auto agent = world.lookupAgent(id).entity;
			if (i < 2)
			{
				auto path = world.getGraph()->calculatePath(agent, target);
				if (!path) return false;
				agent->setPath(path, true);
			}
			passengers.push_back(id);
		}

		std::map<core::AgentId, float> previousCarriageX;
		std::map<core::AgentId, float> twoPassengerTargets;
		bool sawTwoPassengers = false;
		bool retargetedExistingPassenger = false;
		bool reachedSpacedPositions = false;
		bool teleportedInside = false;
		bool violatedPassengerBuffer = false;
		bool thirdJourneyStarted = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
					{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()
				|| shuttle->shuttleCarriages.size() != 1) return false;
			auto const& carriage = shuttle->shuttleCarriages.front();

			std::map<core::AgentId, float> currentCarriageX;
			for (auto id : passengers)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getSector() != world.getSector(created.shuttle.sector->getIndex()).get())
					continue;
				auto relativeX = agent->getGlobalPosition().x - shuttle->liftPosition;
				currentCarriageX[id] = relativeX;
				if (auto previous = previousCarriageX.find(id); previous != previousCarriageX.end())
					teleportedInside = teleportedInside
						|| std::abs(relativeX - previous->second)
							> agent->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
			}
			previousCarriageX = currentCarriageX;
			for (auto left = currentCarriageX.begin(); left != currentCarriageX.end(); ++left)
				for (auto right = std::next(left); right != currentCarriageX.end(); ++right)
					violatedPassengerBuffer = violatedPassengerBuffer
						|| std::abs(left->second - right->second) + 0.001f
							< CORE_AGENT_MAX_WIDTH + CORE_SHUTTLE_AGENT_BUFFER;

			if (carriage.occupantCount == 2 && !thirdJourneyStarted
				&& currentCarriageX.size() == 2)
			{
				std::vector<float> positions;
				for (auto const& [id, x] : currentCarriageX) positions.push_back(x);
				std::sort(positions.begin(), positions.end());
				auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
				auto const first = CORE_SHUTTLE_AGENT_BUFFER + halfWidth;
				auto const last = options.carWidth - CORE_SHUTTLE_AGENT_BUFFER - halfWidth;
				if (std::abs(positions.front() - first) < 0.02f
					&& std::abs(positions.back() - last) < 0.02f)
				{
					sawTwoPassengers = true;
					for (auto const& position : carriage.positions)
						if (position.occupant)
							twoPassengerTargets[position.occupant] = position.position.x;
					auto third = world.lookupAgent(passengers.back()).entity;
					auto path = world.getGraph()->calculatePath(third, target);
					if (!path) return false;
					third->setPath(path, true);
					thirdJourneyStarted = true;
				}
			}
			if (carriage.occupantCount == 3)
			{
				for (auto const& position : carriage.positions)
					if (auto previous = twoPassengerTargets.find(position.occupant);
						previous != twoPassengerTargets.end()
						&& std::abs(previous->second - position.position.x) > 0.1f)
						retargetedExistingPassenger = true;

				std::vector<float> positions;
				for (auto const& [id, x] : currentCarriageX) positions.push_back(x);
				std::sort(positions.begin(), positions.end());
				if (positions.size() == 3)
				{
					auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
					auto const first = CORE_SHUTTLE_AGENT_BUFFER + halfWidth;
					auto const last = options.carWidth - CORE_SHUTTLE_AGENT_BUFFER - halfWidth;
					reachedSpacedPositions = reachedSpacedPositions
						|| (std::abs(positions.front() - first) < 0.02f
							&& std::abs(positions[1] - options.carWidth * 0.5f) < 0.02f
							&& std::abs(positions.back() - last) < 0.02f);
				}
			}
			if (reachedSpacedPositions && retargetedExistingPassenger) break;
		}
		return sawTwoPassengers && retargetedExistingPassenger
			&& reachedSpacedPositions && !teleportedInside && !violatedPassengerBuffer;
	}

	bool shuttleBoardingRequiresDoorAlignment()
	{
		core::World world("Shuttle boarding alignment", 16, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
		auto right = world.addRoom("Right", 0, 0, 10, 3, 1);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 10 }, 0 };
		options.capacity = 3;
		options.doorMask = 0b101;
		auto created = world.addShuttle(1, 0, 0, 13, options);
		world.finishBuild();
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 11.5f, 0.0f });
		std::vector<core::AgentId> passengers;
		for (float x : { 0.4f, 1.5f, 2.6f })
		{
			auto id = world.createAgent("Passenger", left, 0, x);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
			passengers.push_back(id);
		}
		bool sawGrant = false;
		for (unsigned tick = 0; tick < 12000; ++tick)
		{
			std::map<core::AgentId, float> approaching;
			for (auto id : passengers)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getSector() == world.getSector(left).get())
					approaching[id] = agent->getGlobalPosition().x;
			}
			world.advanceTick();
			for (auto const& [id, previousX] : approaching)
			{
				auto agent = world.lookupAgent(id).entity;
				if (std::abs(agent->getGlobalPosition().x - previousX)
					> agent->getWalkSpeed() * world.getFixedTimestep() + 0.001f) return false;
			}
			for (auto const& request : world.getSimulationSnapshot().traversalRequests)
			{
				if (request.edgeType != core::EdgeType::Door
					|| request.sourceSector != core::SectorId{ (uint64_t)left + 1 }
					|| request.state != core::TraversalRequestState::Granted) continue;
				sawGrant = true;
				auto agent = world.lookupAgent(request.owner).entity;
				if (!core::isWithinDoorCrossingBand(agent->getGlobalPosition(),
					request.sourceEndpoint, CORE_DOOR_CROSSING_HALF_WIDTH(1))) return false;
			}
			if (std::all_of(passengers.begin(), passengers.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getSector() == world.getSector(right).get(); }))
				return sawGrant;
		}
		return false;
	}

	bool shuttlePassengerUsesNearestDisembarkDoor()
	{
		core::World world("Nearest Shuttle exit", 16, 2);
		auto left = world.addRoom("Left platform", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right platform", 0, 0, 10, 4, 1);
		core::World::CreateShuttleOptions options{ 1, 4, { 0, 10 }, 0 };
		options.capacity = 1;
		options.doorMask = 0b1001;
		auto created = world.addShuttle(1, 0, 0, 15, options);
		world.finishBuild();
		if (created.doors.size() != 4) return false;

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 10.5f, 0.0f });
		if (!target) return false;
		auto passengerId = world.createAgent("Nearest-exit passenger", left, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path) return false;
		auto const plannedDoor = created.doors[2].traversalResource;
		auto const nearestDoor = created.doors[3].traversalResource;
		bool plannedLeftDoor = std::any_of(path->nodes.begin(), path->nodes.end(),
			[&](auto const& node)
				{ return node.edge && node.edge->getTraversalResourceId() == plannedDoor; });
		if (!plannedLeftDoor) return false;
		passenger->setPath(path, true);

		auto const shuttleSector = core::SectorId{
			(uint64_t)created.shuttle.sector->getIndex() + 1 };
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == passengerId && request.edgeType == core::EdgeType::Door
					&& request.sourceSector == shuttleSector && request.shuttleDoor)
				{
					auto const x = passenger->getGlobalPosition().x;
					auto const nearestDoorIsPhysicallyNearest = std::abs(x - 13.5f)
						< std::abs(x - 10.5f);
					return nearestDoorIsPhysicallyNearest && request.shuttleDoor == nearestDoor;
				}
		}
		return false;
	}

	bool singleCarriageShuttleUsesTransportJourneyProtocol()
	{
		core::World world("Single carriage shuttle", 12, 2);
		auto left = world.addRoom("Left platform", 0, 0, 0, 3, 1);
		auto right = world.addRoom("Right platform", 0, 0, 7, 3, 1);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 7 }, 0 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = world.addShuttle(1, 0, 0, 11, options);
		world.finishBuild();
		if (!created.traversalResource || !created.interiorSelector || created.doors.size() != 2)
			return false;
		auto initial = world.getSimulationSnapshot();
		for (auto const& door : created.doors)
		{
			auto landing = std::find_if(initial.traversalResources.begin(),
				initial.traversalResources.end(),
				[&](auto const& resource) { return resource.id == door.traversalResource; });
			if (landing == initial.traversalResources.end()) return false;
			auto carLane = std::find_if(landing->queueLanes.begin(), landing->queueLanes.end(),
				[&](auto const& lane)
				{ return lane.sector.value == created.shuttle.sector->getIndex() + 1; });
			if (carLane == landing->queueLanes.end()
				|| carLane->positions.size() != options.capacity) return false;
		}
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 8.5f, 0.0f });
		if (!target) return false;
		std::vector<core::AgentId> passengers;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto id = world.createAgent("Shuttle passenger", left, 0, 1.0f + i * 0.15f);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			uint32_t rides = 0, doors = 0;
			for (auto const& node : path->nodes) if (node.edge)
			{
				rides += node.edge->getType() == core::EdgeType::Shuttle;
				doors += node.edge->getType() == core::EdgeType::Door;
			}
			if (rides != 1 || doors != 2) return false;
			agent->setPath(path, true);
			passengers.push_back(id);
		}

		bool sawPhysicalCall = false;
		bool sawFullWithWaiter = false;
		bool sawPlatformQueuePosition = false;
		bool sawAttachedMotion = false;
		bool sawDisembarkBeforeBoard = false;
		std::map<core::AgentId, float> previousCarriageX;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 14; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end() || !shuttle->isShuttle
				|| shuttle->occupantCount + shuttle->admissionReservationCount > options.capacity)
				return false;
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::CallShuttle
					&& operation.state == core::DeviceOperationState::Succeeded) sawPhysicalCall = true;
			std::vector<core::Vector2> platformQueueTargets;
			for (auto const& request : snapshot.traversalRequests)
				if (request.state == core::TraversalRequestState::Pending
					&& request.sourceSector.value == left + 1 && request.hasQueuePosition)
					platformQueueTargets.push_back(request.queuePositionTarget);
			sawPlatformQueuePosition = sawPlatformQueuePosition || !platformQueueTargets.empty();
			for (auto passengerId : passengers)
			{
				auto passenger = world.lookupAgent(passengerId).entity;
				if (passenger->getSector() != world.getSector(created.shuttle.sector->getIndex()).get())
				{
					previousCarriageX.erase(passengerId);
					continue;
				}
				auto carriageX = passenger->getGlobalPosition().x - shuttle->liftPosition;
				if (auto previous = previousCarriageX.find(passengerId); previous != previousCarriageX.end())
				{
					if (std::abs(carriageX - previous->second) > passenger->getWalkSpeed()
						* world.getFixedTimestep() + 0.001f) return false;
				}
				previousCarriageX[passengerId] = carriageX;
			}
			sawFullWithWaiter = sawFullWithWaiter
				|| (shuttle->occupantCount == options.capacity && !shuttle->admissionQueue.empty());
			if (shuttle->liftStopPhase == core::LiftStopPhase::Disembarking)
			{
				if (shuttle->admissionReservationCount != 0) return false;
				sawDisembarkBeforeBoard = true;
			}
			for (auto const& passenger : passengers)
			{
				auto agent = world.lookupAgent(passenger).entity;
				if (agent->getSector() == world.getSector(left).get()
					&& std::abs(agent->getGlobalPosition().y) > 0.001f) return false;
			}
			if (shuttle->liftMoving)
				for (auto const& passenger : passengers)
				{
					auto agent = world.lookupAgent(passenger).entity;
					if (agent->getSector() == world.getSector(created.shuttle.sector->getIndex()).get()
						&& agent->getGlobalPosition().x >= shuttle->liftPosition)
						sawAttachedMotion = true;
				}
			if (std::all_of(passengers.begin(), passengers.end(), [&](auto id)
				{ auto agent = world.lookupAgent(id).entity; return agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == world.getSector(right).get(); })) break;
		}
		auto final = world.getSimulationSnapshot();
		auto shuttle = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawPhysicalCall && sawFullWithWaiter && sawPlatformQueuePosition
			&& sawAttachedMotion && sawDisembarkBeforeBoard
			&& shuttle != final.traversalResources.end() && shuttle->occupantCount == 0
			&& shuttle->admissionReservationCount == 0;
	}

	bool shuttleArrivalFollowsFinalPathNodeWithoutBacktracking()
	{
		core::World world("Multi-door Shuttle arrival", 48, 6);
		auto left = world.addCorridor(1, 1, 6);
		auto right = world.addCorridor(1, 15, 6);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 13 }, 0 };
		options.capacity = 5;
		options.doorMask = 0b101;
		options.minimumDwellSeconds = 0.75f;
		options.maximumBoardingSeconds = 5.0f;
		auto created = world.addShuttle(1, 1, 3, 16, options);
		world.finishBuild();

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 20.5f, 1.0f });
		if (!target) return false;
		auto passengerId = world.createAgent("Multi-door passenger", left, 0, 0.4f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path) return false;
		uint32_t shuttleEdges = 0;
		float finalShuttleX = 0.0f;
		for (auto const& node : path->nodes)
			if (node.edge && node.edge->getType() == core::EdgeType::Shuttle && node.targetVertex)
			{
				++shuttleEdges;
				finalShuttleX = node.targetVertex->getPosition().x;
			}
		if (shuttleEdges < 2 || std::abs(finalShuttleX - 18.5f) > 0.001f) return false;
		passenger->setPath(std::move(path), true);

		std::optional<float> previousStoppedX;
		std::optional<uint64_t> lastOccupiedAtDestination;
		bool reachedFinalDoorBand = false;
		bool sawAllDestinationDoorsOpen = false;
		bool sawBoardingWindowAfterDisembark = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 14; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()) return false;
			passenger = world.lookupAgent(passengerId).entity;
			if (!shuttle->liftMoving && shuttle->liftCurrentStop == 1)
			{
				if (passenger->getSector()
					== world.getSector(created.shuttle.sector->getIndex()).get())
					lastOccupiedAtDestination = snapshot.tick;
				if (shuttle->liftStopPhase == core::LiftStopPhase::Disembarking)
				{
					bool allOpen = true;
					for (uint32_t doorIndex = 2; doorIndex < 4; ++doorIndex)
					{
						auto door = std::find_if(snapshot.traversalResources.begin(),
							snapshot.traversalResources.end(), [&](auto const& resource)
							{ return resource.id == created.doors[doorIndex].traversalResource; });
						allOpen = allOpen && door != snapshot.traversalResources.end()
							&& (door->doorState == core::DoorSnapshotState::Opening
								|| door->doorState == core::DoorSnapshotState::Open);
					}
					if (!allOpen) return false;
					sawAllDestinationDoorsOpen = true;
				}
				if (shuttle->liftStopPhase == core::LiftStopPhase::Boarding
					&& lastOccupiedAtDestination)
				{
					auto boardingTicks = (uint64_t)std::ceil(options.maximumBoardingSeconds
						/ world.getFixedTimestep());
					if (shuttle->liftServiceStartedTick <= *lastOccupiedAtDestination
						|| shuttle->liftBoardingCutoffTick
							!= shuttle->liftServiceStartedTick + boardingTicks) return false;
					sawBoardingWindowAfterDisembark = true;
				}
			}
			if (passenger->getSector() == world.getSector(created.shuttle.sector->getIndex()).get()
				&& !shuttle->liftMoving && shuttle->liftCurrentStop == 1)
			{
				auto x = passenger->getGlobalPosition().x;
				if (previousStoppedX && x < *previousStoppedX - 0.001f) return false;
				reachedFinalDoorBand = reachedFinalDoorBand
					|| std::abs(x - finalShuttleX) <= CORE_DOOR_CROSSING_HALF_WIDTH(1) + 0.001f;
				previousStoppedX = x;
			}
			if (sawBoardingWindowAfterDisembark && passenger->getState() == core::Agent::State::Idle
				&& passenger->getSector() == world.getSector(right).get()) break;
		}
		return reachedFinalDoorBand && sawAllDestinationDoorsOpen && sawBoardingWindowAfterDisembark
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == world.getSector(right).get();
	}

	bool multiCarriageShuttleCoordinatesIndependentCarriagesAndAccessZones()
	{
		core::World world("Coupled shuttle", 20, 2);
		auto leftA = world.addRoom("Left A", 0, 0, 0, 3, 1);
		auto leftB = world.addRoom("Left B", 0, 0, 4, 3, 1);
		auto rightA = world.addRoom("Right A", 0, 0, 12, 3, 1);
		auto rightB = world.addRoom("Right B", 0, 0, 16, 3, 1);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 12 }, 0 };
		options.capacity = 1;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 2.0f;
		auto created = world.addShuttle(1, 0, 0, 19, options);
		world.finishBuild();
		if (!created.traversalResource || created.doors.size() != 4) return false;

		struct Journey { core::AgentId agent; uint32_t targetSector; float targetX; };
		std::vector<Journey> journeys = {
			{ world.createAgent("A first", leftA, 0, 1.35f), rightA, 13.5f },
			{ world.createAgent("B", leftB, 0, 1.5f), rightB, 17.5f },
			{ world.createAgent("A overflow", leftA, 0, 1.65f), rightA, 13.5f }
		};
		for (auto const& journey : journeys)
		{
			auto agent = world.lookupAgent(journey.agent).entity;
			auto target = world.getGraph()->getClosestVertexInSector(
				world.getSector(journey.targetSector).get(), { journey.targetX, 0.0f });
			if (!target) return false;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(std::move(path), true);
		}

		bool sawIndependentFullCarriages = false;
		std::array<bool, 2> sawCarriageOccupied{};
		bool sawSeparatedAccessZones = false;
		bool sawBoundAssignment = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 24; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end() || shuttle->shuttleCarriages.size() != 2
				|| shuttle->capacity != 2 || shuttle->shuttleCapacityPerCarriage != 1) return false;
			for (auto const& carriage : shuttle->shuttleCarriages)
			{
				if (carriage.occupantCount + carriage.admissionReservationCount > carriage.capacity) return false;
				if (carriage.index < sawCarriageOccupied.size() && carriage.occupantCount)
					sawCarriageOccupied[carriage.index] = true;
			}
			sawIndependentFullCarriages = sawIndependentFullCarriages
				|| (shuttle->shuttleCarriages[0].occupantCount == 1
					&& shuttle->shuttleCarriages[1].occupantCount == 1);
			uint32_t leftZones = 0;
			for (auto const& zone : shuttle->shuttleAccessZones)
				if (zone.stopIndex == 0 && zone.direction == core::TraversalDirection::Ascending) ++leftZones;
			sawSeparatedAccessZones = sawSeparatedAccessZones || leftZones >= 2;
			for (auto const& request : snapshot.traversalRequests)
				if (request.shuttleCarriage != ~0u && request.shuttleDoor)
					sawBoundAssignment = true;
			if (std::all_of(journeys.begin(), journeys.end(), [&](auto const& journey)
				{ auto agent = world.lookupAgent(journey.agent).entity;
					return agent->getState() == core::Agent::State::Idle
						&& agent->getSector() == world.getSector(journey.targetSector).get(); })) break;
		}
		auto complete = std::all_of(journeys.begin(), journeys.end(), [&](auto const& journey)
			{ auto agent = world.lookupAgent(journey.agent).entity;
				return agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == world.getSector(journey.targetSector).get(); });
		return (sawIndependentFullCarriages || std::all_of(sawCarriageOccupied.begin(), sawCarriageOccupied.end(),
			[](bool occupied) { return occupied; }))
			&& sawSeparatedAccessZones && sawBoundAssignment && complete;
	}

	bool liftFailuresCancellationAndDisableDrainSafely()
	{
		// Repeated selector failures keep the landing open and eventually return the
		// passenger to the current stop without leaking lift ownership.
		{
			core::World world("Failed lift selector", 6, 4);
			auto lower = world.addCorridor(0, 0, 5);
			auto upper = world.addCorridor(2, 0, 5);
			core::World::CreateLiftOptions options;
			options.stopOffsets = { 0, 2 };
			auto created = world.addLift(1, 0, 2, options);
			world.finishBuild();
			auto target = world.getGraph()->getClosestVertexInSector(
				world.getSector(upper).get(), { 2.5f, 2.0f });
			auto id = world.createAgent("Failed selector passenger", lower, 0, 2.5f);
			auto agent = world.lookupAgent(id).entity;
			agent->setPath(world.getGraph()->calculatePath(agent, target), true);
			std::set<core::DeviceOperationId> failed;
			bool stayedOpen = true;
			for (uint32_t tick = 0; tick < MaximumSimulationTicks * 5; ++tick)
			{
				world.advanceTick();
				auto snapshot = world.getSimulationSnapshot();
				for (auto const& operation : snapshot.deviceOperations)
				{
					if (operation.command.type != core::DeviceCommandType::SelectLiftDestination
						|| failed.contains(operation.id)
						|| (operation.state != core::DeviceOperationState::Pending
							&& operation.state != core::DeviceOperationState::Running)) continue;
					world.lookupDeviceOperation(operation.id).entity->setState(core::DeviceOperationState::Failed);
					failed.insert(operation.id);
				}
				auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
					[&](auto const& resource) { return resource.id == created.traversalResource; });
				if (lift != snapshot.traversalResources.end() && lift->occupantCount > 0
					&& lift->liftStopPhase == core::LiftStopPhase::Closing) stayedOpen = false;
				if (failed.size() == 3 && agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == world.getSector(lower).get()) break;
			}
			auto final = world.getSimulationSnapshot();
			auto lift = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (failed.size() != 3 || !stayedOpen || lift == final.traversalResources.end()
				|| lift->occupantCount != 0 || lift->admissionReservationCount != 0
				|| !lift->admissionQueue.empty() || lift->liftPendingSafeExits != 0) return false;
		}

		// Cancellation while moving and subsequent disable both preserve occupancy
		// until alignment, reject fresh demand, and unload through a landing permit.
		{
			core::World world("Disabled moving lift", 6, 4);
			auto lower = world.addCorridor(0, 0, 5);
			auto upper = world.addCorridor(2, 0, 5);
			core::World::CreateLiftOptions options;
			options.stopOffsets = { 0, 2 };
			auto created = world.addLift(1, 0, 2, options);
			world.finishBuild();
			auto target = world.getGraph()->getClosestVertexInSector(
				world.getSector(upper).get(), { 2.5f, 2.0f });
			auto id = world.createAgent("Cancelled onboard passenger", lower, 0, 2.5f);
			auto agent = world.lookupAgent(id).entity;
			agent->setPath(world.getGraph()->calculatePath(agent, target), true);
			bool cancelledMoving = false;
			for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
			{
				world.advanceTick();
				auto snapshot = world.getSimulationSnapshot();
				auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
					[&](auto const& resource) { return resource.id == created.traversalResource; });
				if (!cancelledMoving && lift != snapshot.traversalResources.end() && lift->liftMoving)
				{
					agent->clearPath();
					cancelledMoving = world.setTraversalResourceEnabled(created.traversalResource, false);
				}
				if (cancelledMoving && agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == world.getSector(upper).get()) break;
			}
			world.advanceTick(); // publish the terminal unavailable state after unload commit
			auto final = world.getSimulationSnapshot();
			auto lift = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (!cancelledMoving || lift == final.traversalResources.end() || lift->enabled
				|| lift->liftDraining || lift->occupantCount != 0 || lift->liftPendingSafeExits != 0
				|| !lift->liftScheduledStops.empty() || !final.traversalRequests.empty()
				|| !final.traversalPermits.empty()) return false;
		}
		return true;
	}

	bool editorLiftAuthoringReconcilesOwnedLandings()
	{
		core::World world("Editor lift authoring", 10, 8);
		world.addCorridor(1, 0, 8);
		world.addCorridor(4, 0, 8);
		auto created = world.addLift(1, 0, 2, 2, 6);
		world.finishBuild();
		auto lift = std::dynamic_pointer_cast<const core::LiftTransit>(created.lift.sector);
		if (!lift || lift->getCellsWide() != 2 || lift->getLevelsHigh() != 6
			|| lift->getNumStops() != 2 || created.doors.size() != 2) return false;
		for (auto const& door : created.doors)
			if (!world.isLiftOwnedDoor(door.door.sector->getObject(door.door.index))) return false;

		world.pauseSimulation();
		world.addCorridor(3, 0, 8);
		world.finishBuild();
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(
			world.getSectorAtPosition(1, 2.0f, 0.0f));
		if (!lift || lift->getNumStops() != 2) return false; // Corridors do not create stops.
		uint32_t landingX = 0, landingWidth = 0;
		if (!world.getLiftLandingGeometry(1, 3, 3, landingX, landingWidth)
			|| landingX != 2 || landingWidth != 2) return false;
		auto added = world.addSectorDoor(0, 3, 3);
		if (!world.isLiftOwnedDoor(added.door.sector->getObject(added.door.index))) return false;
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(
			world.getSectorAtPosition(1, 2.0f, 0.0f));
		if (!lift || lift->getNumStops() != 3) return false;

		auto move = world.planResizeLift(lift->getIndex(), 5, 0, 2, 6);
		if (!move.valid || !move.move) return false;
		auto movedIndex = world.applyLiftEdit(move);
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(world.getSector(movedIndex));
		if (!lift || lift->getCellX() != 5 || lift->getLevelsHigh() != 6
			|| lift->getNumStops() != 3) return false;
		for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
		{
			auto level = (uint32_t)((int)lift->getStop(stop).sector->getCellY()
				+ lift->getStop(stop).sectorOffsetY);
			auto const& cell = static_cast<core::World const&>(world)
				.getLayer(0)->getCellDefinition(5, level);
			auto door = world.getSector(cell.sectorIndex)->getObject(cell.sectorObjectIndex);
			if (!world.isLiftOwnedDoor(door)) return false;
		}
		auto removeStop = world.planRemoveLiftStop(lift->getIndex(), 1);
		if (!removeStop.valid || !removeStop.requiresConfirmation()) return false;
		auto afterStopRemoval = world.applyLiftEdit(removeStop);
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(world.getSector(afterStopRemoval));
		if (!lift || lift->getNumStops() != 2) return false;
		auto remove = world.planRemoveLift(lift->getIndex());
		if (!remove.valid) return false;
		world.applyLiftEdit(remove);
		return !world.getSectorAtPosition(1, 5.0f, 0.0f);
	}

	bool editorShuttleAuthoringReconcilesOwnedLandings()
	{
		core::World world("Editor shuttle authoring", 32, 3);
		world.addCorridor(0, 0, 31);
		world.addCorridor(1, 0, 31);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.doorMask = 0b101;
		auto candidates = world.getValidShuttleStopOffsets(1, 0, 0, 27, 2, 3, false, 2);
		if (find(candidates.begin(), candidates.end(), 0) == candidates.end()
			|| find(candidates.begin(), candidates.end(), 18) == candidates.end()) return false;
		auto created = world.addShuttle(1, 0, 0, 27, options);
		world.finishBuild();
		auto shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(created.shuttle.sector);
		if (!shuttle || shuttle->getNumStops() != 2 || created.doors.size() != 8) return false;
		for (auto const& door : created.doors)
		{
			uint32_t owner, stop, carriage;
			if (!world.isShuttleOwnedDoor(door.door.sector->getObject(door.door.index),
				&owner, &stop, &carriage) || owner != shuttle->getIndex()
				|| stop >= 2 || carriage >= 2) return false;
		}
		auto doorCandidates = world.getShuttleStopCandidatesForDoor(1, 0, 9);
		if (none_of(doorCandidates.begin(), doorCandidates.end(), [&](auto const& candidate)
			{ return candidate.sectorIndex == shuttle->getIndex() && candidate.stopOffset == 9; })) return false;

		world.pauseSimulation();
		auto move = world.planResizeShuttle(shuttle->getIndex(), 1, 1, 27);
		if (!move.valid || !move.move || move.stopOffsets != std::vector<uint32_t>({ 0, 18 })) return false;
		auto movedIndex = world.applyShuttleEdit(move);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(world.getSector(movedIndex));
		if (!shuttle || shuttle->getCellX() != 1 || shuttle->getCellY() != 1) return false;

		auto resize = world.planResizeShuttle(movedIndex, 1, 1, 26);
		if (!resize.valid || resize.move || resize.stopOffsets != std::vector<uint32_t>({ 0, 18 })) return false;
		movedIndex = world.applyShuttleEdit(resize);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(world.getSector(movedIndex));
		if (!shuttle || shuttle->getCellsWide() != 26) return false;

		auto add = world.planAddShuttleStop(movedIndex, 9);
		if (!add.valid || !add.requiresConfirmation()) return false;
		movedIndex = world.applyShuttleEdit(add);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(world.getSector(movedIndex));
		if (!shuttle || shuttle->getNumStops() != 3) return false;
		auto removeStop = world.planRemoveShuttleStop(movedIndex, 1);
		if (!removeStop.valid || !removeStop.requiresConfirmation()) return false;
		movedIndex = world.applyShuttleEdit(removeStop);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(world.getSector(movedIndex));
		if (!shuttle || shuttle->getNumStops() != 2) return false;
		world.addSectorWindow(0, 1, 10, 1, 1);
		auto remove = world.planRemoveShuttle(movedIndex);
		if (!remove.valid) return false;
		world.applyShuttleEdit(remove);
		if (world.getSectorAtPosition(1, 1.0f, 1.0f)) return false;

		core::World manyDoors("Schematic Shuttle doors", 24, 2);
		manyDoors.addCorridor(0, 0, 23);
		core::World::CreateShuttleOptions manyDoorOptions{ 1, 4, { 0, 10 }, 0 };
		manyDoorOptions.doorMask = 0b1111;
		auto manyDoorResult = manyDoors.addShuttle(1, 0, 0, 20, manyDoorOptions);
		manyDoors.finishBuild();
		if (manyDoorResult.doors.size() != 8
			|| !manyDoors.getValidShuttleStopOffsets(1, 0, 0, 20, 1, 4, false, 1u << 4).empty()) return false;
		for (uint32_t door = 0; door < 4; ++door)
			if (manyDoorResult.doors[door].door.sector->getObject(
				manyDoorResult.doors[door].door.index)->getCellX() != door) return false;

		core::World partial("Partial Shuttle authoring", 24, 2);
		partial.addCorridor(0, 0, 4);
		partial.addCorridor(0, 10, 4);
		core::World::CreateShuttleOptions partialOptions{ 2, 3, { 0, 10 }, 0 };
		partialOptions.allowPartialLandings = true;
		partialOptions.doorMask = 0b101;
		auto partialCreated = partial.addShuttle(1, 0, 0, 20, partialOptions);
		partial.finishBuild();
		return partialCreated.doors.size() == 8
			&& partialCreated.doors[0].traversalResource
			&& partialCreated.doors[1].traversalResource
			&& !partialCreated.doors[2].traversalResource
			&& !partialCreated.doors[3].traversalResource
			&& partialCreated.doors[4].traversalResource
			&& partialCreated.doors[5].traversalResource
			&& !partialCreated.doors[6].traversalResource
			&& !partialCreated.doors[7].traversalResource;
	}

	bool unavailableDoorRejectsTraversal()
	{
		core::World world("Unavailable door", 6, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 5, 1);
		world.addRoom("Back", 1, 0, 0, 5, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Unavailable;
		world.addSectorDoor(0, 0, 2, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = world.createAgent("Rejected traveller", fore, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		// This is the same two-step path assignment used by the UI for a
		// player-directed agent: preview the route, then explicitly start it.
		agent->setPath(twoNodePath(source, destination, edge), false);
		world.advanceTick();
		if (agent->getState() != core::Agent::State::Idle
			|| !world.getSimulationSnapshot().traversalRequests.empty()) return false;
		agent->startPathing();
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& world.getSimulationSnapshot().traversalRequests.empty(); ++i)
		{
			world.advanceTick();
		}
		auto snapshot = world.getSimulationSnapshot();
		return agent->getSector() == world.getSector(fore).get()
			&& agent->getState() == core::Agent::State::WaitingForTraversal
			&& snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.deviceOperations.empty() && snapshot.traversalPermits.empty();
	}

	bool interactionBindingAggregationIsMeaningful()
	{
		core::World world("Binding aggregation", 3, 2);
		auto corridorIndex = world.addCorridor(0, 0, 2);
		world.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto actor = world.createAgent("Binding operator", corridorIndex, 0, 0.5f);

		core::InteractionBinding required{ { core::DeviceCommandType::SetSectorLights, sectorId, false },
			core::InteractionBindingRequirement::Required };
		core::InteractionBinding bestEffort{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::BestEffort };
		auto point = world.createInteractionPoint("Multi-binding control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { required, bestEffort });
		auto requestId = world.requestInteraction(point, actor);
		auto request = world.lookupInteractionRequest(requestId);
		if (!request || request.entity->getOperations().size() != 2)
		{
			return false;
		}
		auto failedBestEffort = request.entity->getOperations()[1].first;
		world.lookupDeviceOperation(failedBestEffort).entity->setState(core::DeviceOperationState::Failed);
		world.advanceTicks(4);
		request = world.lookupInteractionRequest(requestId);
		if (!request || request.entity->getResult() != core::InteractionResult::SucceededWithBestEffortFailure)
		{
			return false;
		}

		core::InteractionBinding failingRequired{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::Required };
		auto requiredPoint = world.createInteractionPoint("Required control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { failingRequired });
		auto failedRequestId = world.requestInteraction(requiredPoint, actor);
		auto failedRequest = world.lookupInteractionRequest(failedRequestId);
		if (!failedRequest)
		{
			return false;
		}
		world.lookupDeviceOperation(failedRequest.entity->getOperations().front().first).entity->setState(
			core::DeviceOperationState::Failed);
		world.advanceTick();
		return world.lookupInteractionRequest(failedRequestId).entity->getResult() == core::InteractionResult::Failed;
	}

	struct ScaleObservation
	{
		bool valid{ false };
		uint64_t deterministicDigest{ 1469598103934665603ull };
		double elapsedMilliseconds{ 0.0 };
		size_t workingSetBytes{ 0 };
	};

	void digestValue(uint64_t& digest, uint64_t value)
	{
		for (uint32_t byte = 0; byte < 8; ++byte)
		{
			digest ^= (value >> (byte * 8)) & 0xffu;
			digest *= 1099511628211ull;
		}
	}

	size_t currentWorkingSetBytes()
	{
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS_EX counters{};
		counters.cb = sizeof(counters);
		return GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))
			? counters.WorkingSetSize : 0;
#elif defined(__linux__)
		long totalPages = 0;
		long residentPages = 0;
		std::ifstream statm("/proc/self/statm");
		if (!(statm >> totalPages >> residentPages)) return 0;
		auto const pageSize = sysconf(_SC_PAGESIZE);
		return pageSize > 0 ? static_cast<size_t>(residentPages) * static_cast<size_t>(pageSize) : 0;
#else
#error "Unsupported platform"
#endif
	}

	ScaleObservation runScaledWorld(uint32_t agentCount, uint64_t ticks)
	{
		constexpr uint32_t ResourceCount = 32;
		core::World world("Scale world", 80, 2);
		auto corridor = world.addCorridor(0, 0, 79);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 70.5f, &destinationVertexId);
		world.finishBuild();
		for (uint32_t i = 0; i < ResourceCount; ++i)
		{
			world.createTraversalResource("Scale resource " + std::to_string(i));
		}

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto edge = std::make_shared<core::SectorEdge>();
		for (uint32_t i = 0; i < agentCount; ++i)
		{
			auto id = world.createAgent("Scale agent " + std::to_string(i), corridor, 0, 0.5f);
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		ScaleObservation result;
		auto digestEvents = [&](std::vector<core::SimulationEvent> const& events)
		{
			for (auto const& event : events)
			{
				digestValue(result.deterministicDigest, event.sequence);
				digestValue(result.deterministicDigest, event.tick);
				digestValue(result.deterministicDigest, (uint64_t)event.type);
				digestValue(result.deterministicDigest, (uint64_t)event.phase);
				digestValue(result.deterministicDigest, event.agent.id.value);
				digestValue(result.deterministicDigest, event.traversalRequest.id.value);
				digestValue(result.deterministicDigest, event.traversalPermit.id.value);
			}
		};
		digestEvents(world.consumeSimulationEvents());
		auto started = std::chrono::steady_clock::now();
		for (uint64_t tick = 0; tick < ticks; ++tick)
		{
			world.advanceTick();
			// Event delivery is intentionally incremental in a long-running host.
			if ((tick + 1) % 10 == 0) digestEvents(world.consumeSimulationEvents());
		}
		digestEvents(world.consumeSimulationEvents());
		result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		result.workingSetBytes = currentWorkingSetBytes();

		auto snapshot = world.getSimulationSnapshot();
		std::set<uint64_t> requestOwners;
		result.valid = snapshot.tick == ticks && snapshot.agents.size() == agentCount
			&& snapshot.traversalResources.size() == ResourceCount;
		for (auto const& agent : snapshot.agents)
		{
			result.valid = result.valid && agent.hasPath && agent.globalPosition.x > 0.5f;
			digestValue(result.deterministicDigest, agent.id.value);
			digestValue(result.deterministicDigest, std::bit_cast<uint32_t>(agent.globalPosition.x));
			digestValue(result.deterministicDigest, std::bit_cast<uint32_t>(agent.globalPosition.y));
			digestValue(result.deterministicDigest, (uint64_t)agent.state);
		}
		for (auto const& request : snapshot.traversalRequests)
		{
			result.valid = result.valid && request.state != core::TraversalRequestState::Cancelled
				&& request.state != core::TraversalRequestState::Denied
				&& !request.diagnostic.empty() && requestOwners.insert(request.owner.value).second;
		}
		for (auto const& resource : snapshot.traversalResources)
		{
			result.valid = result.valid
				&& resource.occupantCount + resource.admissionReservationCount <= resource.capacity
				&& resource.virtualBoundaryCrossingCount <= resource.capacity;
			for (auto const& carriage : resource.shuttleCarriages)
				result.valid = result.valid && carriage.occupantCount
					+ carriage.admissionReservationCount <= carriage.capacity;
		}
		return result;
	}

	// Ticket #14: a three-Layer World whose back-most Layer carries a Transit
	// landing on the Layer directly in front of it.  The Agent starts on the
	// front-most Layer, crosses a Door authored on the 0<->1 pair into Layer 1,
	// boards the Layer 2 Lift through its landing Doors, rides it, and disembarks
	// into the upper level of the Layer 1 Corridor.
	struct ThreeLayerJourneyResult
	{
		bool pathShapeValid{ false };
		bool sawTransitOccupant{ false };
		bool reachedDestination{ false };
		ScenarioResult run;
	};

	ThreeLayerJourneyResult runThreeLayerTransitJourney()
	{
		ThreeLayerJourneyResult result;

		core::World world("Three-layer transit traversal", 8, 4);
		while (world.getLayerCount() < 3) world.addLayer();
		if (world.getLayerCount() != 3) return result;

		// Layer 0 is the Agent's entry Layer, Layer 1 the Transit's landing Layer,
		// and Layer 2 the Transit Layer itself.
		auto entry = world.addCorridor(0, 0, 0, 6, 1);
		auto lower = world.addCorridor(1, 0, 0, 6, 1);
		auto upper = world.addCorridor(1, 2, 0, 6, 1);

		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 2 };
		auto lift = world.addLift(2, 0, 4, liftOptions);

		// A Door is authored on the front Layer of the pair it crosses.
		auto door = world.addSectorDoor(0, 0, 1);
		world.finishBuild();

		if (!world.isTraversalTopologyValid() || lift.doors.size() != 2
			|| door.door.type != core::SectorObjectType::Door) return result;

		// The Transit sits on Layer 2 and every landing it owns is on Layer 1.
		auto transit = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(lift.lift.sector->getIndex()));
		if (!transit || transit->getLayerIndex() != 2 || transit->getNumStops() != 2) return result;
		if (transit->getStop(0).sector->getIndex() != lower
			|| transit->getStop(1).sector->getIndex() != upper)
			return result;
		for (uint32_t stop = 0; stop < transit->getNumStops(); ++stop)
			if (transit->getStop(stop).sector->getLayerIndex() != 1) return result;

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 5.5f, 2.0f });
		if (!target) return result;

		auto agentId = world.createAgent("Deep traveller", entry, 0, 0.5f);
		auto agent = world.lookupAgent(agentId).entity;
		if (!agent) return result;
		auto path = world.getGraph()->calculatePath(agent, target);
		if (!path) return result;

		// The route must cross into Layer 1 exactly once, board and leave the Layer 2
		// Transit through its landing Doors, and ride it exactly once.
		uint32_t entryDoors{ 0 }, landingDoors{ 0 }, rides{ 0 };
		for (auto const& node : path->nodes)
		{
			if (!node.edge) continue;
			auto const a = node.edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = node.edge->getVertex(1)->getSector()->getLayerIndex();
			auto const type = node.edge->getType();
			if (type == core::EdgeType::Door)
			{
				if ((a == 0 && b == 1) || (a == 1 && b == 0)) ++entryDoors;
				else if ((a == 1 && b == 2) || (a == 2 && b == 1)) ++landingDoors;
			}
			else if (type == core::EdgeType::Lift) ++rides;
		}
		if (entryDoors != 1 || landingDoors != 2 || rides != 1) return result;
		result.pathShapeValid = true;

		agent->setPath(path, true);
		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks * 4)
		{
			world.advanceTick();
			if (agent->getSector() == transit.get()) result.sawTransitOccupant = true;
		}

		result.run.snapshot = world.getSimulationSnapshot();
		result.run.events = world.consumeSimulationEvents();
		auto const finalPosition = agent->getGlobalPosition();
		result.reachedDestination = result.sawTransitOccupant
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(upper).get()
			&& agent->getSector()->getLayerIndex() == 1
			&& finalPosition.distanceTo(target->getPosition()) < 0.001f;
		return result;
	}

	// Ticket #15: pulling a middle Layer out of a four-Layer World while Agents
	// are still using it.  The plan the editor would confirm has to name every
	// casualty: the Locations on the deleted Layer, the Transit on that Layer, the
	// Lift one Layer behind which loses its landings, the Door crossing it, and
	// the Agents standing in each doomed Sector.  Applying the plan must then
	// leave a compacted World whose surviving Layers are still valid and
	// walkable, with the deeper Ladder and its landing pair moved forward intact.
	struct MiddleLayerDeletionResult
	{
		bool planValid{ false };
		bool planCountsValid{ false };
		bool casualtiesRemoved{ false };
		bool layersCompacted{ false };
		bool survivorsTraversable{ false };
		std::string diagnostic;
		ScenarioResult run;
	};

	// Authored Sector handles, recorded before a deletion renumbers them.
	struct MiddleLayerLayout
	{
		uint32_t entry{ 0 };
		uint32_t lobby{ 0 };
		uint32_t lowerLobby{ 0 };
		uint32_t middleLevel{ 0 };
		uint32_t middleStore{ 0 };
		uint32_t doomedLadder{ 0 };
		uint32_t deepStore{ 0 };
		uint32_t deepCorridor{ 0 };
		uint32_t deepYard{ 0 };
		uint32_t doomedLift{ 0 };
		uint32_t annexe{ 0 };
		uint32_t keptLadder{ 0 };
	};

	// Layer 0 is the entry Layer, Layer 1 the Layer under test, and Layers 2 and 3
	// the deeper Layers which must survive.  Every Location is one level high so a
	// Ladder can land on two stacked Locations, and the two Doors are authored at
	// different cells so the deletion counts the Door it really crosses rather
	// than one which merely shares a cell.
	MiddleLayerLayout authorMiddleLayerDeletionWorld(core::World& world)
	{
		while (world.getLayerCount() < 4) world.addLayer();
		world.setLayerName(1, "Middle");
		world.setLayerName(2, "Deep");
		world.setLayerName(3, "Attic");

		MiddleLayerLayout layout;
		layout.entry = world.addCorridor(0, 0, 0, 12, 1);
		layout.lobby = world.addRoom("Lobby", 0, 1, 0, 12, 1);
		layout.lowerLobby = world.addRoom("Lower Lobby", 0, 2, 0, 12, 1);
		layout.middleLevel = world.addRoom("Middle Level", 1, 0, 0, 12, 1);
		layout.middleStore = world.addRoom("Middle Store", 1, 2, 0, 11, 1);
		layout.doomedLadder = world.addLadder(1, 1, 11, { 2, false, true })
			.ladder.sector->getIndex();
		layout.deepStore = world.addRoom("Deep Store", 2, 0, 0, 10, 1);
		layout.deepCorridor = world.addRoom("Deep Corridor", 2, 1, 0, 10, 1);
		layout.deepYard = world.addRoom("Deep Yard", 2, 2, 0, 10, 1);

		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.stopOffsets = { 0, 2 };
		layout.doomedLift = world.addLift(2, 0, 10, liftOptions).lift.sector->getIndex();

		layout.annexe = world.addRoom("Annexe", 3, 0, 0, 10, 1);
		layout.keptLadder = world.addLadder(3, 1, 9, { 2, false, true })
			.ladder.sector->getIndex();

		// A Door is authored on the front Layer of the pair it crosses.
		world.addSectorDoor(0, 0, 4);
		world.addSectorDoor(2, 0, 6);
		world.addSectorMarker(layout.deepStore, 0, 1.5f, nullptr);
		world.addSectorMarker(layout.annexe, 0, 8.5f, nullptr);
		world.finishBuild();
		return layout;
	}

	std::shared_ptr<const core::Sector> sectorByName(core::World const& world, std::string const& name)
	{
		for (uint32_t i = 0; i < world.getNumSectors(); ++i)
			if (world.getSector(i)->getName() == name) return world.getSector(i);
		return nullptr;
	}

	MiddleLayerDeletionResult runMiddleLayerDeletion()
	{
		MiddleLayerDeletionResult result;
		core::World world("Layer deletion smoke world", 12, 4);
		auto const layout = authorMiddleLayerDeletionWorld(world);
		if (!world.isTraversalTopologyValid())
		{
			result.diagnostic = "authored World is invalid: " + world.getTopologyDiagnostic();
			return result;
		}

		// The doomed Lift lands on the Layer under test, and the doomed Ladder sits
		// on it, so both are casualties of the deletion even though only one of
		// them is actually on it.
		auto const doomedLift = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(layout.doomedLift));
		auto const doomedLadder = std::dynamic_pointer_cast<const core::Transit>(
			world.getSector(layout.doomedLadder));
		if (!doomedLift || doomedLift->getLayerIndex() != 2 || doomedLift->getNumStops() != 2)
		{
			result.diagnostic = "the authored Lift does not sit on Layer 2 with two stops";
			return result;
		}
		if (!doomedLadder || doomedLadder->getLayerIndex() != 1 || doomedLadder->getNumStops() != 2)
		{
			result.diagnostic = "the authored Ladder does not sit on Layer 1 with two stops";
			return result;
		}
		for (uint32_t stop = 0; stop < doomedLift->getNumStops(); ++stop)
			if (doomedLift->getStop(stop).sector->getLayerIndex() != 1)
			{
				result.diagnostic = "the doomed Lift does not land on Layer 1";
				return result;
			}

		// One Agent stands in every kind of Sector the deletion touches, and two of
		// them are mid-journey when the Layer is pulled out from under them.
		auto const entryAgentId = world.createAgent("Entry walker", layout.entry, 0, 0.5f);
		auto const sitterAgentId = world.createAgent("Middle sitter", layout.middleLevel, 0, 0.5f);
		auto const climberAgentId = world.createAgent("Doomed climber", layout.doomedLadder, 0, 0.5f);
		auto const riderAgentId = world.createAgent("Doomed rider", layout.doomedLift, 0, 0.5f);
		auto const deepAgentId = world.createAgent("Deep traveller", layout.deepStore, 0, 0.5f);
		auto const yardAgentId = world.createAgent("Yard keeper", layout.deepYard, 0, 0.5f);

		auto const annexeBefore = world.getSector(layout.annexe).get();
		auto const annexeTarget = world.getGraph()->getClosestVertexInSector(annexeBefore, { 8.5f, 0.0f });
		if (!annexeTarget
			|| annexeTarget->getPosition().distanceTo({ 8.5f, 0.0f }) > 0.001f)
		{
			result.diagnostic = "the Annexe destination Marker is not where the scenario authors it";
			return result;
		}
		auto const traveller = world.lookupAgent(deepAgentId).entity;
		if (!traveller)
		{
			result.diagnostic = "the traveller Agent was not created";
			return result;
		}
		auto const outbound = world.getGraph()->calculatePath(traveller, annexeTarget);
		if (!outbound)
		{
			result.diagnostic = "the authored World has no route from the Deep Store to the Annexe";
			return result;
		}
		traveller->setPath(outbound, true);
		for (uint64_t tick = 0; tick < 60; ++tick) world.advanceTick();
		if (traveller->getState() == core::Agent::State::Idle)
		{
			result.diagnostic = "the traveller finished before the deletion could catch it mid-journey";
			return result;
		}

		world.pauseSimulation();
		auto const plan = world.planDeleteLayer(1);
		result.planValid = plan.valid;
		if (!plan.valid)
		{
			result.diagnostic = plan.diagnostic;
			return result;
		}

		result.planCountsValid = plan.layerCountBefore == 4 && plan.layerCountAfter == 3
			&& plan.locationsRemoved == 2 && plan.transitsRemoved == 2
			&& plan.doorsRemoved == 1 && plan.agentsRemoved == 3
			&& plan.requiresConfirmation();
		if (!result.planCountsValid)
		{
			std::ostringstream detail;
			detail << "layers " << plan.layerCountBefore << "->" << plan.layerCountAfter
				<< ", locations=" << plan.locationsRemoved
				<< ", transits=" << plan.transitsRemoved
				<< ", doors=" << plan.doorsRemoved
				<< ", agents=" << plan.agentsRemoved;
			result.diagnostic = detail.str();
			return result;
		}

		world.applyDeleteLayer(plan);

		// Every casualty is gone: the two Locations and the Ladder on the deleted
		// Layer, the Lift which lost its landings, and the three Agents which were
		// standing in them.  The deeper Ladder survives because its landing pair
		// was never touched.
		uint32_t lifts{ 0 };
		std::shared_ptr<const core::Sector> survivingLadder;
		for (uint32_t i = 0; i < world.getNumSectors(); ++i)
		{
			auto const sector = world.getSector(i);
			if (!sector) continue;
			if (sector->getName() == "Middle Level" || sector->getName() == "Middle Store")
			{
				result.diagnostic = "a Location survived on the deleted Layer";
				return result;
			}
			if (sector->getType() == core::SectorType::Lift) ++lifts;
			if (sector->getType() == core::SectorType::Ladder) survivingLadder = sector;
		}
		result.casualtiesRemoved = world.getNumSectors() == 8 && lifts == 0 && survivingLadder != nullptr
			&& world.lookupAgent(sitterAgentId).entity == nullptr
			&& world.lookupAgent(climberAgentId).entity == nullptr
			&& world.lookupAgent(riderAgentId).entity == nullptr;
		result.casualtiesRemoved = result.casualtiesRemoved
			&& world.isSimulationPaused() && world.isTraversalTopologyValid();
		if (!result.casualtiesRemoved)
		{
			std::ostringstream detail;
			detail << "sectors=" << world.getNumSectors() << ", lifts=" << lifts
				<< ", topology=" << (world.isTraversalTopologyValid() ? "valid" : world.getTopologyDiagnostic());
			result.diagnostic = detail.str();
			return result;
		}

		// The Layers behind the deletion moved forward one, names and all, and the
		// Sectors on them moved with their Layers.
		result.layersCompacted = world.getLayerCount() == 3
			&& world.getLayerName(1) == "Deep" && world.getLayerName(2) == "Attic";
		for (auto const* name : { "Deep Store", "Deep Corridor", "Deep Yard" })
		{
			auto const sector = sectorByName(world, name);
			if (!sector || sector->getLayerIndex() != 1) result.layersCompacted = false;
		}
		auto const annexe = sectorByName(world, "Annexe");
		auto const survivingTransit = std::dynamic_pointer_cast<const core::Transit>(survivingLadder);
		if (!annexe || annexe->getLayerIndex() != 2) result.layersCompacted = false;
		if (!survivingTransit || survivingTransit->getLayerIndex() != 2
			|| survivingTransit->getNumStops() != 2)
			result.layersCompacted = false;
		else
			for (uint32_t stop = 0; stop < survivingTransit->getNumStops(); ++stop)
				if (survivingTransit->getStop(stop).sector->getLayerIndex() != 1)
					result.layersCompacted = false;

		// The Door which crossed the deleted Layer is gone; the one behind it now
		// crosses the compacted pair.
		uint32_t deletedCrossing{ 0 }, compactedCrossing{ 0 };
		for (auto const& edge : world.getGraph()->getEdges())
		{
			if (!edge || edge->getType() != core::EdgeType::Door) continue;
			auto const a = edge->getVertex(0)->getSector()->getLayerIndex();
			auto const b = edge->getVertex(1)->getSector()->getLayerIndex();
			if ((a == 0 && b == 1) || (a == 1 && b == 0)) ++deletedCrossing;
			if ((a == 1 && b == 2) || (a == 2 && b == 1)) ++compactedCrossing;
		}
		result.layersCompacted = result.layersCompacted && deletedCrossing == 0 && compactedCrossing == 1;
		if (!result.layersCompacted)
		{
			std::ostringstream detail;
			detail << "layers=" << world.getLayerCount() << ", door crossings into the deleted pair="
				<< deletedCrossing << ", door crossings over the compacted pair=" << compactedCrossing;
			result.diagnostic = detail.str();
			return result;
		}

		// The Agents which were not in a doomed Sector are still there, resting on
		// the Layer their Sector compacted to.
		if (world.lookupAgent(entryAgentId).entity == nullptr
			|| world.lookupAgent(deepAgentId).entity == nullptr
			|| world.lookupAgent(yardAgentId).entity == nullptr)
		{
			result.diagnostic = "a surviving Agent was removed by the deletion";
			return result;
		}
		auto const entry = world.lookupAgent(entryAgentId).entity;
		auto const deep = world.lookupAgent(deepAgentId).entity;
		auto const yard = world.lookupAgent(yardAgentId).entity;
		if (entry->getSector()->getLayerIndex() != 0
			|| deep->getSector()->getName() != "Deep Store"
			|| deep->getSector()->getLayerIndex() != 1
			|| yard->getSector()->getName() != "Deep Yard"
			|| yard->getSector()->getLayerIndex() != 1)
		{
			result.diagnostic = "a surviving Agent was not re-placed on its compacted Layer";
			return result;
		}

		// And the compacted World still works: one Agent crosses the surviving
		// Door into the compacted back Layer, and the other rides the compacted
		// Ladder between the two Locations which were never in danger.
		world.resumeSimulation();
		auto const annexeVertex = world.getGraph()->getClosestVertexInSector(annexe.get(), { 8.5f, 0.0f });
		auto const corridorVertex = world.getGraph()->getClosestVertexInSector(
			sectorByName(world, "Deep Corridor").get(), { 1.5f, 1.5f });
		// The destination Marker has to have followed its Sector through the record
		// rewrite rather than landing on a renumbered neighbour.
		if (!annexeVertex || annexeVertex->getPosition().distanceTo({ 8.5f, 0.0f }) > 0.001f
			|| !corridorVertex)
		{
			result.diagnostic = "a Marker did not follow its Sector through the compaction";
			return result;
		}
		auto const deepPath = world.getGraph()->calculatePath(deep, annexeVertex);
		auto const yardPath = world.getGraph()->calculatePath(yard, corridorVertex);
		if (!deepPath || !yardPath)
		{
			result.diagnostic = "the compacted World has no route for a surviving Agent";
			return result;
		}

		bool rodeLadder{ false };
		for (auto const& node : yardPath->nodes)
			if (node.edge && node.edge->getType() == core::EdgeType::Ladder) rodeLadder = true;
		if (!rodeLadder)
		{
			result.diagnostic = "the compacted Ladder is no longer part of the Yard keeper's route";
			return result;
		}

		deep->setPath(deepPath, true);
		yard->setPath(yardPath, true);
		while ((deep->getState() != core::Agent::State::Idle || yard->getState() != core::Agent::State::Idle)
			&& world.getSimulationTick() < MaximumSimulationTicks * 4)
		{
			world.advanceTick();
		}

		result.survivorsTraversable = deep->getState() == core::Agent::State::Idle
			&& deep->getSector() == annexe.get()
			&& deep->getGlobalPosition().distanceTo(annexeVertex->getPosition()) < 0.001f
			&& yard->getState() == core::Agent::State::Idle
			&& yard->getSector() == sectorByName(world, "Deep Corridor").get()
			&& yard->getGlobalPosition().distanceTo(corridorVertex->getPosition()) < 0.001f;

		result.run.snapshot = world.getSimulationSnapshot();
		result.run.events = world.consumeSimulationEvents();
		return result;
	}

	ScenarioResult runOrdinaryPathScenario()
	{
		core::World world("Headless smoke world", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);

		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto agentId = world.createAgent("Headless smoke agent", corridor, 0, 0.5f);
		auto agentLookup = world.lookupAgent(agentId);
		if (!agentLookup)
		{
			return {};
		}
		auto agent = agentLookup.entity;

		auto graph = world.getGraph();
		auto source = graph->getVertexByIdentifier(sourceVertexId);
		auto destination = graph->getVertexByIdentifier(destinationVertexId);
		auto path = graph->calculatePath(agent, source, destination);
		if (!path)
		{
			return {};
		}
		agent->setPath(path, true);

		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
		}

		ScenarioResult result;
		result.snapshot = world.getSimulationSnapshot();
		result.events = world.consumeSimulationEvents();
		auto finalPosition = agent->getGlobalPosition();
		result.reachedDestination = agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(corridor).get()
			&& finalPosition.distanceTo(destination->getPosition()) < 0.001f
			&& phasesAreOrdered(result.events, result.snapshot.tick);
		return result;
	}
}

int main(int argc, char** argv)
{
	bool const graphicsStartupOnly = argc > 1
		&& std::string(argv[1]) == "--graphics-startup-smoke";

	try
	{
		if (graphicsStartupOnly)
		{
			runGraphicsStartupSmokeChecks();
			return 0;
		}

		runSerializationSmokeChecks();
		runAgentGroupSmokeChecks();
		runAgentGroupAssignmentSmokeChecks();
		runAgentGroupCountSmokeChecks();
		runAgentGroupDeleteSmokeChecks();
		runAgentGroupIdAllocationSmokeChecks();
		runAgentGroupClipboardSmokeChecks();
		runAgentGroupTopologySmokeChecks();
		runAgentActivationSmokeChecks();
		runMarkerIdentitySmokeChecks();
		runMovementCommandSmokeChecks();
		runRenderOrderSmokeChecks();
		runShuttleDoorRenderSmokeChecks();
		runDoorOpenApartRenderSmokeChecks();
		runDoorOpenLeftRenderSmokeChecks();
		runDoorOpenRightRenderSmokeChecks();
		runEditorLayerSmokeChecks();
		runWindowLayerSmokeChecks();
		runBackgroundSectorSmokeChecks();
		runBackgroundPaintSmokeChecks();
		runBackgroundPlacementSmokeChecks();
		runBackgroundCascadeDeleteSmokeChecks();
		runBackgroundSelectionPanelSmokeChecks();
		runDoorPanelScopeSmokeChecks();
		runDoorTwoSidedButtonSmokeChecks();
		runDocumentHistorySmokeChecks();
		runAgentTagRegistrySmokeChecks();
		runAgentBehaviourRegistrySmokeChecks();
		runAgentBehaviourAssignmentSmokeChecks();
		runAgentBehaviourPortabilitySmokeChecks();
		runAgentBehaviourDeleteSmokeChecks();
		runAgentBehaviourSchemaReconciliationSmokeChecks();
		runAgentBehaviourRuntimeSmokeChecks();
		runAgentBehaviourWorkflowSmokeChecks();
		runAgentTagRegistryChangeSmokeChecks();
		runAgentTagDocumentSaveSmokeChecks();
		runAgentTagReloadSmokeChecks();
		runAgentTagAssignmentSmokeChecks();
		runAgentTagDeleteSmokeChecks();
		runAgentTagCoordinationSmokeChecks();
		runAgentTagReconciliationSmokeChecks();
		runAgentTagClipboardSmokeChecks();
		runAgentColourSmokeChecks();
		runAgentWalkSpeedSmokeChecks();
		runAgentHeightSmokeChecks();
		runShuttleDoorQuerySmokeChecks();
		runThresholdRefusalSmokeChecks();
		runThresholdLayerOverlapSmokeChecks();
		runWindowIntoBackgroundSmokeChecks();
		runWindowMultiBackgroundSmokeChecks();
		runFacadeSmokeChecks();
		runFacadeRenderSmokeChecks();
		runFacadeDrawOrderSmokeChecks();
		runFacadeEditorSmokeChecks();
		runPaletteTraySmokeChecks();
		runOnboardAgentDeletionSmokeChecks();
		runWallRenderSmokeChecks();
		runViewportCullingSmokeChecks();
		runZeroSizeLocationSmokeChecks();
		runIsolatedSectorPathingSmokeChecks();

		auto const deepJourney = runThreeLayerTransitJourney();
		if (!deepJourney.pathShapeValid)
		{
			std::cerr << "FAIL: three-layer route did not cross into Layer 1 and ride the Layer 2 Transit\n";
			return 1;
		}
		if (!deepJourney.reachedDestination)
		{
			std::cerr << "FAIL: Agent did not traverse the three-layer world through its back-layer Transit\n";
			return 1;
		}
		auto const deepRepeat = runThreeLayerTransitJourney();
		if (canonicalResult(deepJourney.run) != canonicalResult(deepRepeat.run))
		{
			std::cerr << "FAIL: three-layer Transit traversal was not deterministic\n";
			return 1;
		}

		auto const deletion = runMiddleLayerDeletion();
		if (!deletion.planValid)
		{
			std::cerr << "FAIL: middle Layer deletion was rejected: " << deletion.diagnostic << "\n";
			return 1;
		}
		if (!deletion.planCountsValid)
		{
			std::cerr << "FAIL: middle Layer deletion did not report its casualties: "
				<< deletion.diagnostic << "\n";
			return 1;
		}
		if (!deletion.casualtiesRemoved)
		{
			std::cerr << "FAIL: middle Layer deletion left Sectors, Transits, or Agents behind: "
				<< deletion.diagnostic << "\n";
			return 1;
		}
		if (!deletion.layersCompacted)
		{
			std::cerr << "FAIL: Layers behind the deleted Layer did not compact correctly: "
				<< deletion.diagnostic << "\n";
			return 1;
		}
		if (!deletion.survivorsTraversable)
		{
			std::cerr << "FAIL: surviving Agents could not travel the compacted World\n";
			return 1;
		}
		auto const deletionRepeat = runMiddleLayerDeletion();
		if (canonicalResult(deletion.run) != canonicalResult(deletionRepeat.run))
		{
			std::cerr << "FAIL: Layer deletion and compaction was not deterministic\n";
			return 1;
		}

		if (!accumulatedRenderTimeAdvancesWholeTicksOnly())
		{
			std::cerr << "FAIL: render-time accumulation did not advance exactly one whole tick\n";
			return 1;
		}
		if (!worldOwnsTypedEntitiesAndInvalidatesHandles())
		{
			std::cerr << "FAIL: typed world ownership or handle invalidation failed\n";
			return 1;
		}
		if (!inferredPathSourceDoesNotMakeAgentDoubleBack())
		{
			std::cerr << "FAIL: inferred path source made the agent double back\n";
			return 1;
		}
		if (!markerPlacementEnforcesPaletteCoreRules())
		{
			std::cerr << "FAIL: Marker placement did not enforce core viability rules\n";
			return 1;
		}
		if (!corridorDoorPlacementEnforcesPaletteRules())
		{
			std::cerr << "FAIL: Door placement did not enforce Location or obstruction rules\n";
			return 1;
		}
		if (!objectMoveValidatesAndRebuildsOnceCommitted())
		{
			std::cerr << "FAIL: Object movement did not validate and rebuild atomically\n";
			return 1;
		}
		if (!windowResizeUsesWindowPlacementRules())
		{
			std::cerr << "FAIL: Window resizing did not preserve options or placement rules\n";
			return 1;
		}
		if (!doorResizeRespectsDoorPlacementRules())
		{
			std::cerr << "FAIL: Door resizing did not preserve options or placement rules\n";
			return 1;
		}
		if (!staircasePathSpansOuterCellEdges())
		{
			std::cerr << "FAIL: Staircase path did not span the outer edges of its endpoint cells\n";
			return 1;
		}
		if (!staircaseCanUseForeRoomEndpoints())
		{
			std::cerr << "FAIL: Staircase could not use valid Fore-layer Room endpoints\n";
			return 1;
		}
		if (!sharedLocationWallsCanBeOpenedAndRestored())
		{
			std::cerr << "FAIL: shared Location walls could not be opened and restored\n";
			return 1;
		}
		if (!walkwayEditingEnforcesPlacementMovementAndOccupancyRules())
		{
			std::cerr << "FAIL: Walkway editing violated placement, movement, deletion, or occupancy rules\n";
			return 1;
		}
		if (!forceBridgeObjectEditingIsAtomic())
		{
			std::cerr << "FAIL: Force Bridge object placement, movement, settings, or deletion was not atomic\n";
			return 1;
		}
		if (!forceBridgeWalkwayDeletionUpdatesItsDestination())
		{
			std::cerr << "FAIL: Force Bridge supports were not protected or extended after Walkway deletion\n";
			return 1;
		}
		if (!deletingWalkwayPreservesUnrelatedRoomDoor())
		{
			std::cerr << "FAIL: deleting a Walkway removed an unrelated Room Door\n";
			return 1;
		}
		if (!roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints())
		{
			std::cerr << "FAIL: Room Ladder placement, stacking, movement, or Walkway recalculation failed\n";
			return 1;
		}
		if (!ordinaryTraversalCommitsOnlyAtDestination())
		{
			std::cerr << "FAIL: ordinary traversal did not hold a permit through atomic commit\n";
			return 1;
		}
		if (!deniedTraversalCannotBeCrossed())
		{
			std::cerr << "FAIL: denied traversal was crossed or leaked its request\n";
			return 1;
		}
		if (!cancellationReleasesPermitWithoutCommitting())
		{
			std::cerr << "FAIL: traversal cancellation leaked or committed membership\n";
			return 1;
		}
		if (!typedLightingInteractionCoalescesAndCancelsByRequester())
		{
			std::cerr << "FAIL: typed lighting interaction, coalescing, or requester cancellation failed\n";
			return 1;
		}
		if (!interactionBindingAggregationIsMeaningful())
		{
			std::cerr << "FAIL: required and best-effort interaction aggregation failed\n";
			return 1;
		}
		if (!singleAgentDoorJourney(core::DoorActivationMode::Manual))
		{
			std::cerr << "FAIL: manual door journey or hold-open safety failed\n";
			return 1;
		}
		if (!singleAgentDoorJourney(core::DoorActivationMode::Automatic))
		{
			std::cerr << "FAIL: automatic door journey or hold-open safety failed\n";
			return 1;
		}
		if (!bulkheadAndWindowThresholdsUseTraversalResources())
		{
			std::cerr << "FAIL: bulkhead or window threshold migration failed\n";
			return 1;
		}
		if (!pausedTopologyRebuildIsAtomicAndCleansOwnership())
		{
			std::cerr << "FAIL: paused topology rebuild was not safe and atomic\n";
			return 1;
		}
		if (!agentsPressUpcomingDoorButtonsWhilePassing())
		{
			std::cerr << "FAIL: agents did not press upcoming Door Buttons while passing\n";
			return 1;
		}
		if (!remoteDoorUsesOnePhysicalOperatorAndSharedOperation())
		{
			std::cerr << "FAIL: remote door did not share physical preparation or survive operator cancellation\n";
			return 1;
		}
		if (!remoteDoorWithoutReachableControlIsUnavailable())
		{
			std::cerr << "FAIL: remote door without a reachable control was not reported unavailable\n";
			return 1;
		}
		if (!fairDoorQueuesServeBothSidesInStableOrder())
		{
			std::cerr << "FAIL: two-sided door queues were not separated, FIFO, or fair\n";
			return 1;
		}
		if (!queuePositionsPreferObjectProximityThenAgentProximity())
		{
			std::cerr << "FAIL: queue positions were not selected by object then agent proximity\n";
			return 1;
		}
		if (!doorQueueRequestsBeforeOccupiedTail())
		{
			std::cerr << "FAIL: Door queue request was not made before its occupied tail\n";
			return 1;
		}
		if (!queuedCancellationReleasesAndAdvancesPositions())
		{
			std::cerr << "FAIL: queued cancellation leaked a ticket or physical position\n";
			return 1;
		}
		if (!wideDoorLanesAndGracefulDisableAreSafe())
		{
			std::cerr << "FAIL: wide door lanes exceeded capacity or deactivation was unsafe\n";
			return 1;
		}
		if (!doorCrossingBandPredicateShape())
		{
			std::cerr << "FAIL: door crossing width band predicate admitted or refused the wrong positions\n";
			return 1;
		}
		if (!doorVertexCarriesCrossingWidth())
		{
			std::cerr << "FAIL: Door vertices did not carry the derived crossing width\n";
			return 1;
		}
		if (!crossingWidthGrantsHeadOfQueueBeforeCentre())
		{
			std::cerr << "FAIL: wide door head of queue was not granted from within the crossing band\n";
			return 1;
		}
		if (!narrowDoorBandArrivalGrantsAtCentreTolerance())
		{
			std::cerr << "FAIL: 1-cell door band arrival was not granted at its centre tolerance\n";
			return 1;
		}
		if (!bandArrivalCrossesWideDoorFromStandingPosition())
		{
			std::cerr << "FAIL: lone agent did not cross a wide door from its band-entry position\n";
			return 1;
		}
		if (!bandArrivalComposesWithEarlyStopForContendedDoor())
		{
			std::cerr << "FAIL: band arrival did not compose with the queue at a contended door\n";
			return 1;
		}
		if (!bandArrivalLeavesNonCrossingAgentsUnaffected())
		{
			std::cerr << "FAIL: non-crossing agent inside the band x range created a request\n";
			return 1;
		}
		if (!resilientWaitingRetainsPriorityAndExpiresPermits())
		{
			std::cerr << "FAIL: resilient waiting lost priority, leaked reservations, or failed permit expiry\n";
			return 1;
		}
		if (!doorLeasesAndSensorObservationsPreventUnsafeClosure())
		{
			std::cerr << "FAIL: door leases or sensor observations allowed unsafe closure\n";
			return 1;
		}
		if (!singlePassengerCompletesTwoStopLiftJourney())
		{
			std::cerr << "FAIL: single passenger did not complete an interlocked two-stop lift journey\n";
			return 1;
		}
		if (!platformLiftAuthoringReconcilesWalkwayStops())
		{
			std::cerr << "FAIL: PlatformLift authoring did not reconcile Walkway stops\n";
			return 1;
		}
		if (!openPlatformLiftUsesVirtualBoundaryAndTransportPolicy())
		{
			std::cerr << "FAIL: open platform lift journey, virtual boundary, or attachment failed\n";
			return 1;
		}
		if (!openPlatformLiftCrossingLaneSpreadsPassengers())
		{
			std::cerr << "FAIL: open platform lift crossing lane or passenger spreading failed\n";
			return 1;
		}
		if (!openPlatformLiftUsesOneJourneyAcrossIntermediateStops())
		{
			std::cerr << "FAIL: open platform lift did not apply LOOK across intermediate stops\n";
			return 1;
		}
		if (!liftDoorQueueRequestsBeforeOccupiedTail())
		{
			std::cerr << "FAIL: Lift Door queue request was not made before its occupied tail\n";
			return 1;
		}
		if (!waitingLiftPassengersFillArrivingCar())
		{
			std::cerr << "FAIL: waiting passengers did not fill an arriving lift with available capacity\n";
			return 1;
		}
		if (!liftCallOperatorDoesNotFightItsQueuePosition())
		{
			std::cerr << "FAIL: Lift call operator fought its reserved queue position\n";
			return 1;
		}
		if (!liftCapacityAndStopPhasesAreEnforced())
		{
			std::cerr << "FAIL: lift capacity, boarding cutoff, or later service was not enforced\n";
			return 1;
		}
		if (!multiStopLiftUsesDeterministicLookScheduling())
		{
			std::cerr << "FAIL: multi-stop lift did not follow deterministic LOOK scheduling\n";
			return 1;
		}
		if (!liftFailuresCancellationAndDisableDrainSafely())
		{
			std::cerr << "FAIL: lift failure, cancellation, or disabled draining was unsafe\n";
			return 1;
		}
		if (!shuttlePassengerWalksToForwardInteriorSpot())
		{
			std::cerr << "FAIL: Shuttle passenger teleported or did not walk to the forward interior spot\n";
			return 1;
		}
		if (!shuttlePassengersSpreadAcrossCarriageAtWalkingSpeed())
		{
			std::cerr << "FAIL: Shuttle passengers did not spread through the carriage at walking speed\n";
			return 1;
		}
		if (!shuttleBoardingRequiresDoorAlignment())
		{
			std::cerr << "FAIL: Shuttle boarding granted outside the Door crossing band\n";
			return 1;
		}
		if (!shuttlePassengerUsesNearestDisembarkDoor())
		{
			std::cerr << "FAIL: Shuttle passenger did not select the nearest carriage Door to exit\n";
			return 1;
		}
		if (!singleCarriageShuttleUsesTransportJourneyProtocol())
		{
			std::cerr << "FAIL: single-carriage shuttle journey coordination failed\n";
			return 1;
		}
		if (!shuttleArrivalFollowsFinalPathNodeWithoutBacktracking())
		{
			std::cerr << "FAIL: multi-door Shuttle arrival backtracked through an intermediate path node\n";
			return 1;
		}
		if (!multiCarriageShuttleCoordinatesIndependentCarriagesAndAccessZones())
		{
			std::cerr << "FAIL: multi-carriage shuttle or access-zone coordination failed\n";
			return 1;
		}
		if (!editorLiftAuthoringReconcilesOwnedLandings())
		{
			std::cerr << "FAIL: editor Lift authoring did not reconcile owned landings\n";
			return 1;
		}
		if (!editorShuttleAuthoringReconcilesOwnedLandings())
		{
			std::cerr << "FAIL: editor Shuttle authoring did not reconcile owned landings\n";
			return 1;
		}
		if (!unavailableDoorRejectsTraversal())
		{
			std::cerr << "FAIL: unavailable door did not reject traversal\n";
			return 1;
		}
		if (!finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed())
		{
			std::cerr << "FAIL: finite ladder capacity, reservations, cancellation, or climb speed failed\n";
			return 1;
		}
		if (!ladderQueuePositionsPreferAgentApproachSide())
		{
			std::cerr << "FAIL: Ladder queue spots ignored the Agents' approach sides\n";
			return 1;
		}
		if (!ladderAdmissionsMaintainPhysicalSpacing())
		{
			std::cerr << "FAIL: Ladder admissions overlapped climbers on the span\n";
			return 1;
		}
		if (!directionalLadderBoundsBatchesAndPreventsOpposingAdmission())
		{
			std::cerr << "FAIL: directional ladder admission or bounded-batch fairness failed\n";
			return 1;
		}
		if (!extensibleLadderUsesDesiredStateAndLeases())
		{
			std::cerr << "FAIL: extensible ladder preparation or leases failed\n";
			return 1;
		}
		if (!forceBridgePreparationUsesNearControl())
		{
			std::cerr << "FAIL: Force Bridge preparation did not use the near control\n";
			return 1;
		}
		if (!extendedForceBridgeAllowsConcurrentTwoWayTraffic())
		{
			std::cerr << "FAIL: extended Force Bridge did not allow concurrent two-way traffic\n";
			return 1;
		}
		if (!extensibleForceBridgeCompletesThroughPhysicalControl())
		{
			std::cerr << "FAIL: extensible force bridge preparation or lease cleanup failed\n";
			return 1;
		}
		if (!stairwellCoordinationIsExplicitlyOptIn())
		{
			std::cerr << "FAIL: ordinary/narrow stairwell coordination policy was incorrect\n";
			return 1;
		}

		auto first = runOrdinaryPathScenario();
		auto second = runOrdinaryPathScenario();
		if (!first.reachedDestination || !second.reachedDestination)
		{
			std::cerr << "FAIL: deterministic ordinary path scenario did not complete\n";
			return 1;
		}
		if (canonicalResult(first) != canonicalResult(second))
		{
			std::cerr << "FAIL: repeated runs produced different snapshots or events\n";
			return 1;
		}

		auto representative = runScaledWorld(500, 60);
		auto repeatedRepresentative = runScaledWorld(500, 60);
		if (!representative.valid || !repeatedRepresentative.valid
			|| representative.deterministicDigest != repeatedRepresentative.deterministicDigest)
		{
			std::cerr << "FAIL: representative scale run violated ownership, capacity, or determinism\n";
			return 1;
		}
		auto stretch = runScaledWorld(1000, 60);
		if (!stretch.valid)
		{
			std::cerr << "FAIL: 1,000-agent stretch run violated ownership or capacity\n";
			return 1;
		}

		auto const& agent = first.snapshot.agents.front();
		std::cout << "SCALE: 500 agents, 32 resources, 60 ticks in "
			<< representative.elapsedMilliseconds << " ms; working set "
			<< representative.workingSetBytes / (1024.0 * 1024.0) << " MiB\n";
		std::cout << "STRETCH: 1000 agents, 32 resources, 60 ticks in "
			<< stretch.elapsedMilliseconds << " ms; working set "
			<< stretch.workingSetBytes / (1024.0 * 1024.0) << " MiB\n";
		std::cout << "PASS: deterministic snapshot and events matched after "
			<< first.snapshot.tick << " fixed ticks; final position=("
			<< agent.globalPosition.x << ", " << agent.globalPosition.y << ")\n";
		return 0;
	}
	catch (std::exception const& exception)
	{
		std::cerr << "FAIL: headless smoke scenario threw: " << exception.what() << '\n';
		return 1;
	}
	catch (...)
	{
		std::cerr << "FAIL: headless smoke scenario threw an unknown exception\n";
		return 1;
	}
}
