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
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Button.h"
#include "core/GapEdge.h"
#include "core/Graph.h"
#include "core/LiftTransit.h"
#include "core/LiftSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/Path.h"
#include "core/SectorEdge.h"
#include "core/Simulation.h"
#include "core/Vector2.h"

#ifdef _MSC_VER
#pragma comment(lib, "Psapi.lib")
#endif

void runSerializationSmokeChecks();

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
		core::Building building("Accumulator check", 1, 1);
		auto halfTick = core::Building::getFixedTimestep() * 0.5f;
		building.update(halfTick);
		if (building.getSimulationTick() != 0)
		{
			return false;
		}
		building.update(halfTick);
		return building.getSimulationTick() == 1;
	}

	bool buildingOwnsTypedEntitiesAndInvalidatesHandles()
	{
		core::Building building("Ownership check", 3, 2);
		auto corridor = building.addCorridor(0, 0, 2);
		building.finishBuild();

		auto agentId = building.createAgent("Owned idle agent", corridor, 0, 0.5f);
		auto pointId = building.createInteractionPoint("Light switch");
		auto operationId = building.createDeviceOperation("Turn lights on", agentId);
		auto resourceId = building.createTraversalResource("Ordinary passage");

		auto snapshot = building.getSimulationSnapshot();
		if (snapshot.agents.size() != 1 || snapshot.agents.front().id != agentId
			|| snapshot.interactionPoints.size() != 1 || snapshot.interactionPoints.front().id != pointId
			|| snapshot.deviceOperations.size() != 1 || snapshot.deviceOperations.front().id != operationId
			|| snapshot.deviceOperations.front().requester != agentId
			|| snapshot.traversalResources.size() != 1 || snapshot.traversalResources.front().id != resourceId)
		{
			return false;
		}

		if (!building.removeAgent(agentId)
			|| building.lookupAgent(agentId)
			|| building.lookupAgent(agentId).diagnostic.empty()
			|| building.lookupDeviceOperation(operationId)
			|| building.lookupDeviceOperation(operationId).diagnostic.empty())
		{
			return false;
		}
		if (!building.removeInteractionPoint(pointId) || !building.removeTraversalResource(resourceId))
		{
			return false;
		}

		building.advanceTick();
		auto afterRemoval = building.getSimulationSnapshot();
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
		core::Building building("Path source selection", 7, 2);
		auto corridor = building.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		building.finishBuild();

		auto source = building.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = building.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = building.createAgent("Path source traveller", corridor, 0, 2.5f);
		auto agent = building.lookupAgent(agentId).entity;

		auto inferredPath = building.getGraph()->calculatePath(agent, destination);
		auto explicitPath = building.getGraph()->calculatePath(agent, source, destination);
		return inferredPath && inferredPath->nodes.size() == 1
			&& inferredPath->nodes.front().targetVertex->sameAs(destination)
			&& !inferredPath->nodes.front().edge
			&& inferredPath->nodes.front().edgeWeight == 0.0f
			&& explicitPath && explicitPath->nodes.size() == 2
			&& explicitPath->nodes.front().targetVertex->sameAs(source);
	}

	bool markerPlacementEnforcesPaletteCoreRules()
	{
		core::Building building("Marker placement rules", 7, 3);
		auto room = building.addRoom("Marker room", CORE_LAYER_FORE, 0, 0, 6, 2);
		building.finishBuild();

		std::string diagnostic;
		if (!building.canAddSectorMarker(room, 0, 2.5f, &diagnostic)
			|| building.canAddSectorMarker(room, 1, 2.5f, &diagnostic)) return false;

		bool runningRejected = false;
		try { building.addSectorMarker(room, 0, 2.5f); }
		catch (std::exception const&) { runningRejected = true; }
		if (!runningRejected || building.isTraversalTopologyDirty()) return false;

		building.pauseSimulation();
		auto created = building.addSectorMarker(room, 0, 2.5f);
		if (created.type != core::SectorObjectType::Marker || created.index == ~0u
			|| building.canAddSectorMarker(room, 0, 2.52f, &diagnostic)
			|| diagnostic.find("already exists") == std::string::npos) return false;

		bool duplicateRejected = false;
		try { building.addSectorMarker(room, 0, 2.52f); }
		catch (std::exception const&) { duplicateRejected = true; }
		return duplicateRejected && building.rebuildTraversalTopology()
			&& building.resumeSimulation() && !building.isSimulationPaused();
	}

	bool corridorDoorPlacementEnforcesPaletteRules()
	{
		core::Building building("Corridor Door placement rules", 10, 4);
		auto corridor = building.addCorridor(0, 0, 6);
		auto backRoom = building.addRoom("Back room", CORE_LAYER_BACK, 0, 0, 6, 1);
		building.addRoom("Not a corridor", CORE_LAYER_FORE, 1, 0, 3, 1);
		building.addRoom("Second back room", CORE_LAYER_BACK, 1, 0, 3, 1);
		building.addCorridor(2, 0, 4);

		std::string diagnostic;
		if (!building.canAddCorridorDoor(0, 2, &diagnostic)
			|| building.canAddCorridorDoor(0, 7, &diagnostic)
			|| diagnostic.find("corridor") == std::string::npos
			|| building.canAddCorridorDoor(1, 1, &diagnostic)
			|| diagnostic.find("corridor") == std::string::npos
			|| building.canAddCorridorDoor(2, 1, &diagnostic)
			|| diagnostic.find("Back Layer Room") == std::string::npos)
			return false;

		building.addSectorMarker(corridor, 0, 4.5f);
		building.addSectorMarker(backRoom, 0, 0.5f);
		if (building.canAddCorridorDoor(0, 4, &diagnostic)
			|| diagnostic.find("blocks") == std::string::npos) return false;

		auto door = building.addSectorDoor(0, 2);
		building.finishBuild();
		return door.door.type == core::SectorObjectType::Door
			&& !building.canAddCorridorDoor(0, 2, &diagnostic)
			&& diagnostic.find("blocks") != std::string::npos
			&& building.isTraversalTopologyValid();
	}

	bool objectMoveValidatesAndRebuildsOnceCommitted()
	{
		core::Building building("Object movement", 10, 3);
		auto corridor = building.addCorridor(0, 0, 8);
		building.addRoom("Back room", CORE_LAYER_BACK, 0, 0, 8, 1);
		core::Building::CreateDoorOptions doorOptions;
		doorOptions.controls[0] = true;
		doorOptions.controls[1] = true;
		doorOptions.activationMode = core::DoorActivationMode::RemoteControlled;
		auto created = building.addSectorDoor(0, 1, doorOptions);
		building.addSectorMarker(corridor, 0, 5.5f);
		building.finishBuild();
		building.pauseSimulation();
		auto agentId = building.createAgent("Stationary", corridor, 0, 0.5f);

		auto outsideBothSectors = building.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 1, 1);
		auto blocked = building.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 5, 0);
		auto valid = building.planMoveSectorObject(
			created.door.sector->getIndex(), created.door.index, 3, 0);
		if (outsideBothSectors.valid || blocked.valid || !valid.valid) return false;

		auto moved = building.applyObjectMove(valid);
		if (!moved || moved->getObjectType() != core::SectorObjectType::Door
			|| moved->getCellX() != 3 || moved->getCellY() != 0
			|| building.lookupAgent(agentId).entity == nullptr
			|| !building.isSimulationPaused() || !building.isTraversalTopologyValid()) return false;
		auto doorOwner = moved->getSector();
		uint32_t movedDoorIndex = ~0u;
		for (uint32_t i = 0; i < doorOwner->getNumObjects(); ++i)
			if (doorOwner->getObject(i) == moved) { movedDoorIndex = i; break; }
		if (movedDoorIndex == ~0u
			|| !building.removeSectorDoor(doorOwner->getIndex(), movedDoorIndex)
			|| building.lookupAgent(agentId).entity == nullptr
			|| !building.getSimulationSnapshot().traversalResources.empty()) return false;
		for (auto const& sector : building.getSectors(CORE_LAYER_FORE))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = sector->getObject(i))
					if (object->getObjectType() == core::SectorObjectType::Door) return false;

		core::Building windowBuilding("Window editing", 10, 3);
		auto fore = windowBuilding.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 8, 1);
		windowBuilding.addRoom("Back", CORE_LAYER_BACK, 0, 0, 8, 1);
		auto createdWindow = windowBuilding.addSectorWindow(CORE_LAYER_FORE, 0, 1, 1, 1, {});
		windowBuilding.finishBuild();
		windowBuilding.pauseSimulation();
		auto windowAgent = windowBuilding.createAgent("Stationary", fore, 0, 0.5f);
		auto windowMove = windowBuilding.planMoveSectorObject(
			createdWindow.window.sector->getIndex(), createdWindow.window.index, 3, 0);
		if (!windowMove.valid) return false;
		auto movedWindow = windowBuilding.applyObjectMove(windowMove);
		if (!movedWindow || movedWindow->getObjectType() != core::SectorObjectType::Window
			|| movedWindow->getCellX() != 3) return false;
		auto owner = movedWindow->getSector();
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == movedWindow) { movedIndex = i; break; }
		if (movedIndex == ~0u || !windowBuilding.removeSectorWindow(owner->getIndex(), movedIndex))
			return false;
		for (auto const& sector : windowBuilding.getSectors(CORE_LAYER_FORE))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = sector->getObject(i))
					if (object->getObjectType() == core::SectorObjectType::Window) return false;
		if (windowBuilding.lookupAgent(windowAgent).entity == nullptr
			|| !windowBuilding.isSimulationPaused() || !windowBuilding.isTraversalTopologyValid()) return false;

		core::Building pasteMoveBuilding("Paste-style movement", 10, 2);
		pasteMoveBuilding.addCorridor(0, 0, 4);
		auto right = pasteMoveBuilding.addCorridor(0, 6, 4);
		pasteMoveBuilding.addRoom("Left back", CORE_LAYER_BACK, 0, 0, 4, 1);
		pasteMoveBuilding.addRoom("Right back", CORE_LAYER_BACK, 0, 6, 4, 1);
		auto crossSectorDoor = pasteMoveBuilding.addSectorDoor(0, 1);
		pasteMoveBuilding.finishBuild();
		pasteMoveBuilding.pauseSimulation();
		auto doorPlan = pasteMoveBuilding.planMoveSectorObject(
			crossSectorDoor.door.sector->getIndex(), crossSectorDoor.door.index, 7, 0);
		if (!doorPlan.valid) return false;
		auto movedAcrossSectors = pasteMoveBuilding.applyObjectMove(doorPlan);
		if (!movedAcrossSectors || movedAcrossSectors->getSector()->getIndex() != right) return false;

		core::Building markerMoveBuilding("Marker movement", 10, 1);
		auto markerLeft = markerMoveBuilding.addCorridor(0, 0, 4);
		auto markerRight = markerMoveBuilding.addCorridor(0, 6, 4);
		auto marker = markerMoveBuilding.addSectorMarker(markerLeft, 0, 2.5f);
		markerMoveBuilding.finishBuild();
		markerMoveBuilding.pauseSimulation();
		auto markerPlan = markerMoveBuilding.planMoveSectorObject(markerLeft, marker.index, 8, 0);
		if (!markerPlan.valid) return false;
		auto movedMarker = markerMoveBuilding.applyObjectMove(markerPlan);
		return movedMarker && movedMarker->getSector()->getIndex() == markerRight
			&& movedMarker->getCellX() == 8;
	}

	bool walkwayEditingEnforcesPlacementMovementAndOccupancyRules()
	{
		core::Building building("Walkway editing", 10, 4);
		auto room = building.addRoom("Walkway room", CORE_LAYER_FORE, 0, 0, 4, 3);
		auto otherRoom = building.addRoom("Other room", CORE_LAYER_FORE, 0, 6, 3, 3);
		std::string diagnostic;
		if (building.canAddSectorWalkway(room, 0, 1, &diagnostic)
			|| !building.canAddSectorWalkway(room, 1, 1, &diagnostic)) return false;
		auto created = building.addSectorWalkway(room, 1, 1);
		building.finishBuild();
		building.pauseSimulation();

		auto acrossRooms = building.planMoveSectorObject(room, created.index, 6, 1);
		auto withinRoom = building.planMoveSectorObject(room, created.index, 2, 1);
		if (acrossRooms.valid || !withinRoom.valid) return false;

		auto agentId = building.createAgent("Walkway occupant", room, 1, 1.5f);
		if (building.planMoveSectorObject(room, created.index, 2, 1).valid) return false;
		bool occupiedDeleteRejected = false;
		try { building.removeSectorWalkway(room, created.index); }
		catch (std::exception const&) { occupiedDeleteRejected = true; }
		if (!occupiedDeleteRejected) return false;
		auto cropped = building.planResizeLocation(room, 0, 0, 1, 3);
		if (cropped.valid) return false;

		if (!building.removeAgent(agentId)) return false;
		withinRoom = building.planMoveSectorObject(room, created.index, 2, 1);
		if (!withinRoom.valid) return false;
		auto moved = building.applyObjectMove(withinRoom);
		if (!moved || moved->getCellX() != 2 || moved->getCellY() != 1
			|| moved->getSector()->getIndex() != room) return false;
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < moved->getSector()->getNumObjects(); ++i)
			if (moved->getSector()->getObject(i) == moved) { movedIndex = i; break; }
		if (movedIndex == ~0u || !building.removeSectorWalkway(room, movedIndex)) return false;
		for (uint32_t i = 0; i < building.getSector(room)->getNumObjects(); ++i)
			if (auto object = building.getSector(room)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::Walkway) return false;

		building.addSectorWalkway(room, 1, 3);
		building.finishBuild();
		auto cropUnoccupied = building.planResizeLocation(room, 0, 0, 3, 3);
		if (!cropUnoccupied.valid) return false;
		auto resizedRoom = building.applyLocationEdit(cropUnoccupied);
		for (uint32_t i = 0; i < building.getSector(resizedRoom)->getNumObjects(); ++i)
			if (auto object = building.getSector(resizedRoom)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::Walkway) return false;
		return building.getSector(otherRoom) != nullptr && building.isTraversalTopologyValid();
	}

	bool forceBridgeObjectEditingIsAtomic()
	{
		core::Building building("Force Bridge editing", 10, 4);
		auto room = building.addRoom("Bridge room", CORE_LAYER_FORE, 0, 0, 8, 3);
		building.addSectorWalkway(room, 1, 0);
		building.addSectorWalkway(room, 1, 3);
		building.addSectorWalkway(room, 1, 6);
		core::Building::CreateForceBridgeOptions options{ 2, CORE_SIDE_LEFT, true, true, 1 };
		std::string diagnostic;
		if (!building.canAddSectorForceBridge(room, 1, 1, options, &diagnostic)
			|| building.canAddSectorForceBridge(room, 1, 2, options, &diagnostic)) return false;
		auto created = building.addSectorForceBridge(room, 1, 1, options);
		building.finishBuild();
		building.pauseSimulation();

		core::Building::CreateForceBridgeOptions authored;
		if (!building.getSectorForceBridgeOptions(room, created.forceBridge.index, authored)
			|| authored.width != 2 || authored.controlCount != 1) return false;
		auto move = building.planMoveSectorObject(room, created.forceBridge.index, 4, 1);
		if (!move.valid || move.previewWidth != 2) return false;
		auto moved = building.applyObjectMove(move);
		if (!moved || moved->getCellX() != 4) return false;
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < moved->getSector()->getNumObjects(); ++i)
			if (moved->getSector()->getObject(i) == moved) { movedIndex = i; break; }
		if (movedIndex == ~0u) return false;
		options.fromSide = CORE_SIDE_RIGHT;
		options.controlCount = 2;
		auto edited = building.applySectorForceBridgeOptions(room, movedIndex, options);
		if (!edited || edited->getCellX() != 4) return false;
		uint32_t editedIndex = ~0u;
		for (uint32_t i = 0; i < edited->getSector()->getNumObjects(); ++i)
			if (edited->getSector()->getObject(i) == edited) { editedIndex = i; break; }
		if (editedIndex == ~0u) return false;
		auto occupant = building.createAgent("Bridge occupant", room, 1, 4.5f);
		bool occupiedDeleteRejected = false;
		try { building.removeSectorForceBridge(room, editedIndex); }
		catch (std::exception const&) { occupiedDeleteRejected = true; }
		if (!occupiedDeleteRejected || !building.removeAgent(occupant)
			|| !building.removeSectorForceBridge(room, editedIndex)) return false;
		for (uint32_t i = 0; i < building.getSector(room)->getNumObjects(); ++i)
			if (auto object = building.getSector(room)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::ForceBridge) return false;
		return building.isTraversalTopologyValid();
	}

	bool forceBridgeWalkwayDeletionUpdatesItsDestination()
	{
		{
			core::Building placement("Force Bridge inferred width", 8, 4);
			auto placementRoom = placement.addRoom("Bridge room", CORE_LAYER_FORE, 0, 0, 6, 3);
			placement.addSectorWalkway(placementRoom, 1, 0);
			placement.addSectorWalkway(placementRoom, 1, 3);
			uint32_t inferredWidth = 0;
			std::string diagnostic;
			core::Building::CreateForceBridgeOptions inferred;
			if (!placement.calculateSectorForceBridgeWidthToRight(placementRoom, 1, 1,
				inferredWidth, &diagnostic) || inferredWidth != 2) return false;
			inferred.width = inferredWidth;
			if (!placement.canAddSectorForceBridge(placementRoom, 1, 1, inferred, &diagnostic))
				return false;
		}

		{
			core::Building right("Right-origin Force Bridge dependencies", 9, 4);
			auto rightRoom = right.addRoom("Bridge room", CORE_LAYER_FORE, 0, 0, 7, 3);
			right.addSectorWalkway(rightRoom, 1, 2);
			auto rightDestination = right.addSectorWalkway(rightRoom, 1, 3);
			auto rightOrigin = right.addSectorWalkway(rightRoom, 1, 5);
			core::Building::CreateForceBridgeOptions rightOptions{
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
				core::Building::CreateForceBridgeOptions updated;
				resized = right.getSectorForceBridgeOptions(rightRoom, i, updated)
					&& candidate->getCellX() == 3 && updated.width == 2
					&& updated.fromSide == CORE_SIDE_RIGHT;
			}
			if (!resized) return false;
		}

		core::Building building("Force Bridge walkway dependencies", 10, 4);
		auto room = building.addRoom("Bridge room", CORE_LAYER_FORE, 0, 0, 7, 3);
		auto origin = building.addSectorWalkway(room, 1, 0);
		auto destination = building.addSectorWalkway(room, 1, 2);
		building.addSectorWalkway(room, 1, 3);
		core::Building::CreateForceBridgeOptions options{ 1, CORE_SIDE_LEFT, true, true, 1 };
		building.addSectorForceBridge(room, 1, 1, options);
		building.finishBuild();
		building.pauseSimulation();

		bool originRejected = false;
		try { building.removeSectorWalkway(room, origin.index); }
		catch (std::exception const&) { originRejected = true; }
		if (!originRejected || !building.removeSectorWalkway(room, destination.index)) return false;
		auto sector = building.getSector(room);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::ForceBridge) continue;
			core::Building::CreateForceBridgeOptions updated;
			return building.getSectorForceBridgeOptions(room, i, updated)
				&& object->getCellX() == 1 && updated.width == 2;
		}
		return false;
	}

	bool roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints()
	{
		core::Building building("Room Ladder editing", 8, 6);
		auto room = building.addRoom("Ladder room", CORE_LAYER_FORE, 0, 0, 5, 5);
		building.addSectorWalkway(room, 2, 1);
		building.addSectorWalkway(room, 4, 1);
		building.addSectorWalkway(room, 3, 3);
		building.addSectorWalkway(room, 3, 4);
		uint32_t height = 0; std::string diagnostic;
		if (!building.canAddRoomLadder(room, 0, 1, &height, &diagnostic) || height != 3) return false;
		auto lower = building.addRoomLadder(room, 0, 1);
		auto upper = building.addRoomLadder(room, 2, 1);
		if (std::static_pointer_cast<const core::LadderSectorObject>(
			lower.ladder.sector->getObject(lower.ladder.index))->getLadder()->getDecksHigh() != 3) return false;
		if (std::static_pointer_cast<const core::LadderSectorObject>(
			upper.ladder.sector->getObject(upper.ladder.index))->getLadder()->getDecksHigh() != 3) return false;
		if (building.canAddRoomLadder(room, 0, 2, &height, &diagnostic)
			|| diagnostic.find("No Walkway") == std::string::npos) return false;
		auto corridor = building.addCorridor(5, 0, 3);
		if (building.canAddRoomLadder(corridor, 0, 0, &height, &diagnostic)) return false;

		building.finishBuild();
		building.pauseSimulation();
		auto move = building.planMoveSectorObject(room, lower.ladder.index, 3, 0);
		if (!move.valid || move.previewHeight != 4) return false;
		auto moved = building.applyObjectMove(move);
		if (!moved || std::static_pointer_cast<const core::LadderSectorObject>(moved)
			->getLadder()->getDecksHigh() != 4) return false;

		auto nearer = building.addSectorWalkway(room, 1, 3);
		auto rebuiltRoom = building.getSector(room);
		std::shared_ptr<const core::LadderSectorObject> recalculated;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0) recalculated = ladder;
		}
		if (!recalculated || recalculated->getLadder()->getDecksHigh() != 2) return false;
		if (!building.removeSectorWalkway(room, nearer.index)) return false;
		rebuiltRoom = building.getSector(room);
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0)
			{
				if (ladder->getLadder()->getDecksHigh() != 4) return false;
				movedIndex = i;
			}
		}
		if (movedIndex == ~0u) return false;
		auto edited = building.applyRoomLadderOptions(room, movedIndex, { 0, true, false, 0.5f, 2 });
		if (!edited) return false;
		core::Building::CreateLadderOptions options{};
		rebuiltRoom = building.getSector(room);
		movedIndex = ~0u;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
			if (rebuiltRoom->getObject(i) == edited) { movedIndex = i; break; }
		if (movedIndex == ~0u || !building.getRoomLadderOptions(room, movedIndex, options)
			|| !options.extensible || options.startExtended || options.agentSpacing != 0.5f
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
		if (!building.removeRoomLadder(room, movedIndex)) return false;
		rebuiltRoom = building.getSector(room);
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0) return false;
		}
		auto edge = building.addRoomLadder(room, 0, 4, { 0, true, true });
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
		core::Building building("Walkway deletion isolation", 16, 6);
		building.addCorridor(4, 9, 4);
		auto room = building.addRoom("Walkway room", CORE_LAYER_BACK, 3, 9, 4, 2);
		core::Building::CreateObjectResult walkways[4];
		for (uint32_t x = 0; x < 4; ++x)
			walkways[x] = building.addSectorWalkway(room, 1, x);
		building.addSectorDoor(4, 12);
		building.finishBuild();
		building.pauseSimulation();

		try
		{
			// The Walkway at 11,4 is not beneath the Door at 12,4. Removing it
			// must shrink the physical queue rather than invalidate the Door.
			if (!building.removeSectorWalkway(room, walkways[2].index)) return false;
		}
		catch (std::exception const&)
		{
			return false;
		}
		uint32_t doorsInRoom = 0, remainingWalkways = 0;
		bool walkwayAt11 = false, walkwayAt12 = false;
		for (uint32_t i = 0; i < building.getSector(room)->getNumObjects(); ++i)
			if (auto object = building.getSector(room)->getObject(i))
			{
				doorsInRoom += object->getObjectType() == core::SectorObjectType::Door;
				if (object->getObjectType() != core::SectorObjectType::Walkway) continue;
				++remainingWalkways;
				walkwayAt11 = walkwayAt11 || object->getCellX() == 11;
				walkwayAt12 = walkwayAt12 || object->getCellX() == 12;
			}
		return doorsInRoom == 1 && remainingWalkways == 3
			&& !walkwayAt11 && walkwayAt12 && building.isTraversalTopologyValid()
			&& building.getSimulationSnapshot().traversalResources.size() == 1;
	}

	bool ordinaryTraversalCommitsOnlyAtDestination()
	{
		core::Building building("Ordinary transition", 10, 2);
		auto sourceSector = building.addCorridor(0, 0, 3);
		auto destinationSector = building.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		building.finishBuild();

		auto source = building.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = building.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = building.createAgent("Ordinary traveller", sourceSector, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		bool observedPermit = false;
		while (agent->getState() != core::Agent::State::Idle
			&& building.getSimulationTick() < MaximumSimulationTicks)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			if (!snapshot.traversalPermits.empty())
			{
				observedPermit = snapshot.traversalPermits.size() == 1
					&& snapshot.traversalRequests.size() == 1
					&& snapshot.traversalRequests.front().diagnostic.starts_with("Active:")
					&& snapshot.agents.front().hasLocomotionTask;
			}

			if (agent->getState() != core::Agent::State::Idle
				&& agent->getSector() != building.getSector(sourceSector).get())
			{
				return false;
			}
		}

		auto snapshot = building.getSimulationSnapshot();
		return observedPermit
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == building.getSector(destinationSector).get()
			&& agent->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool deniedTraversalCannotBeCrossed()
	{
		core::Building building("Denied transition", 7, 2);
		auto corridor = building.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		building.finishBuild();

		auto source = building.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = building.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = building.createAgent("Blocked traveller", corridor, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::GapEdge>()), true);
		building.advanceTicks(30);

		auto snapshot = building.getSimulationSnapshot();
		if (agent->getGlobalPosition().distanceTo(source->getPosition()) >= 0.001f
			|| agent->getSector() != building.getSector(corridor).get()
			|| agent->getState() != core::Agent::State::WaitingForTraversal
			|| snapshot.traversalRequests.size() != 1
			|| snapshot.traversalRequests.front().state != core::TraversalRequestState::Denied
			|| !snapshot.traversalRequests.front().diagnostic.starts_with("Denied:")
			|| !snapshot.traversalPermits.empty())
		{
			return false;
		}

		agent->clearPath();
		snapshot = building.getSimulationSnapshot();
		return snapshot.traversalRequests.empty() && snapshot.traversalPermits.empty()
			&& agent->getSector() == building.getSector(corridor).get();
	}

	bool cancellationReleasesPermitWithoutCommitting()
	{
		core::Building building("Cancelled transition", 10, 2);
		auto sourceSector = building.addCorridor(0, 0, 3);
		auto destinationSector = building.addCorridor(0, 5, 3);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(sourceSector, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(destinationSector, 0, 1.5f, &destinationVertexId);
		building.finishBuild();

		auto source = building.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = building.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = building.createAgent("Cancelling traveller", sourceSector, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, std::make_shared<core::SectorEdge>()), true);

		for (uint32_t i = 0; i < 10 && !agent->getTraversalPermitId(); ++i)
		{
			building.advanceTick();
		}
		if (!agent->getTraversalPermitId() || agent->getSector() != building.getSector(sourceSector).get())
		{
			return false;
		}

		agent->clearPath();
		building.advanceTicks(10);
		auto snapshot = building.getSimulationSnapshot();
		return agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == building.getSector(sourceSector).get()
			&& snapshot.traversalRequests.empty()
			&& snapshot.traversalPermits.empty();
	}

	bool typedLightingInteractionCoalescesAndCancelsByRequester()
	{
		core::Building building("Typed lighting interaction", 6, 2);
		auto corridorIndex = building.addCorridor(0, 0, 5);
		building.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto firstAgent = building.createAgent("First operator", corridorIndex, 0, 0.5f);
		auto secondAgent = building.createAgent("Dependent operator", corridorIndex, 0, 0.7f);

		core::InteractionBinding binding;
		binding.command = { core::DeviceCommandType::SetSectorLights, sectorId, false };
		binding.requirement = core::InteractionBindingRequirement::Required;
		auto point = building.createInteractionPoint("Typed light control", sectorId,
			{ 3.5f, 0.5f }, 0.1f, core::Building::getFixedTimestep() * 3.0f, { binding });
		auto firstRequest = building.requestInteraction(point, firstAgent);
		auto secondRequest = building.requestInteraction(point, secondAgent);
		if (!firstRequest || !secondRequest)
		{
			return false;
		}

		auto first = building.lookupInteractionRequest(firstRequest);
		auto second = building.lookupInteractionRequest(secondRequest);
		if (!first || !second || first.entity->getOperations().size() != 1
			|| second.entity->getOperations().size() != 1
			|| first.entity->getOperations().front().first != second.entity->getOperations().front().first)
		{
			return false;
		}
		auto operationId = first.entity->getOperations().front().first;

		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			building.advanceTick();
			auto operation = building.lookupDeviceOperation(operationId);
			if (operation && operation.entity->getState() == core::DeviceOperationState::Running)
			{
				break;
			}
		}
		auto firstPosition = building.lookupAgent(firstAgent).entity->getGlobalPosition();
		if (firstPosition.distanceTo({ 3.5f, 0.5f }) > 0.101f || !building.cancelInteraction(firstRequest))
		{
			return false;
		}
		auto operation = building.lookupDeviceOperation(operationId);
		if (!operation || operation.entity->getState() == core::DeviceOperationState::Cancelled
			|| operation.entity->getRequesters().size() != 1
			|| !operation.entity->getRequesters().contains(secondAgent))
		{
			return false;
		}

		building.advanceTicks(3);
		first = building.lookupInteractionRequest(firstRequest);
		second = building.lookupInteractionRequest(secondRequest);
		auto snapshot = building.getSimulationSnapshot();
		return first && first.entity->getResult() == core::InteractionResult::Cancelled
			&& second && second.entity->getResult() == core::InteractionResult::Succeeded
			&& !building.getSector(corridorIndex)->areLightsOn()
			&& snapshot.deviceOperations.size() == 1
			&& snapshot.deviceOperations.front().hasCommand
			&& snapshot.deviceOperations.front().command.type == core::DeviceCommandType::SetSectorLights
			&& snapshot.deviceOperations.front().state == core::DeviceOperationState::Succeeded
			&& snapshot.interactionPoints.front().activeRequest == core::InteractionRequestId{};
	}

	bool singleAgentDoorJourney(core::DoorActivationMode mode)
	{
		core::Building building("Single-agent door", 6, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 5, 1);
		auto back = building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 5, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = mode;
		options.holdOpenSeconds = core::Building::getFixedTimestep() * 8.0f;
		auto created = building.addSectorDoor(0, 2, options);
		building.finishBuild();

		auto edgeIt = std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& edge) { return edge->getType() == core::EdgeType::Door; });
		if (edgeIt == building.getGraph()->getEdges().end() || !created.traversalResource)
		{
			return false;
		}
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = building.createAgent("Door traveller", fore, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);

		bool observedWaitingForFullOpen = false;
		bool observedVisibleCrossingLease = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks && agent->getState() != core::Agent::State::Idle; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto const& resource = snapshot.traversalResources.front();
			if (resource.doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()
				&& agent->getSector() == building.getSector(fore).get())
			{
				observedWaitingForFullOpen = true;
			}
			if (agent->getState() == core::Agent::State::TraversingEdge
				&& resource.doorState == core::DoorSnapshotState::Open
				&& resource.openLeaseCount == 1
				&& agent->getSector() == building.getSector(fore).get())
			{
				observedVisibleCrossingLease = true;
			}
		}

		auto completed = building.getSimulationSnapshot();
		if (!observedWaitingForFullOpen || !observedVisibleCrossingLease
			|| agent->getState() != core::Agent::State::Idle
			|| agent->getSector() != building.getSector(back).get()
			|| completed.deviceOperations.size() != 1
			|| completed.deviceOperations.front().command.type != core::DeviceCommandType::OpenDoor
			|| completed.deviceOperations.front().state != core::DeviceOperationState::Succeeded
			|| completed.traversalResources.front().doorActivationMode != mode
			|| completed.traversalResources.front().openLeaseCount != 0)
		{
			return false;
		}

		for (uint32_t i = 0; i < 120
			&& building.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closed; ++i)
		{
			building.advanceTick();
		}
		return building.getSimulationSnapshot().traversalResources.front().doorState == core::DoorSnapshotState::Closed;
	}

	bool bulkheadAndWindowThresholdsUseTraversalResources()
	{
		// A bulkhead is horizontal and same-layer, but still queues and waits for
		// its fully-open resource permit.
		core::Building bulkheadBuilding("Bulkhead threshold", 8, 2);
		auto left = bulkheadBuilding.addRoom("Left", CORE_LAYER_FORE, 0, 0, 3, 1);
		auto right = bulkheadBuilding.addRoom("Right", CORE_LAYER_FORE, 0, 3, 3, 1);
		core::Building::CreateBulkheadDoorOptions bulkheadOptions;
		bulkheadOptions.activationMode = core::DoorActivationMode::Manual;
		bulkheadOptions.controls[0] = bulkheadOptions.controls[1] = false;
		auto bulkhead = bulkheadBuilding.addSectorBulkheadDoor(CORE_LAYER_FORE, 0, 3,
			CORE_SIDE_LEFT, bulkheadOptions);
		bulkheadBuilding.finishBuild();
		auto bulkheadEdge = std::find_if(bulkheadBuilding.getGraph()->getEdges().begin(),
			bulkheadBuilding.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::BulkheadDoor; });
		if (bulkheadEdge == bulkheadBuilding.getGraph()->getEdges().end()
			|| (*bulkheadEdge)->getTraversalResourceId() != bulkhead.traversalResource) return false;
		auto source = (*bulkheadEdge)->getVertex(0)->getSector()->getIndex() == left
			? (*bulkheadEdge)->getVertex(0) : (*bulkheadEdge)->getVertex(1);
		auto destination = (*bulkheadEdge)->getOtherVertex(source);
		auto agentId = bulkheadBuilding.createAgent("Left bulkhead traveller", left, 0, 1.0f);
		auto opposingId = bulkheadBuilding.createAgent("Right bulkhead traveller", right, 0, 1.0f);
		auto agent = bulkheadBuilding.lookupAgent(agentId).entity;
		auto opposing = bulkheadBuilding.lookupAgent(opposingId).entity;
		agent->setPath(twoNodePath(source, destination, *bulkheadEdge), true);
		opposing->setPath(twoNodePath(destination, source, *bulkheadEdge), true);
		bool waitedForOpen = false;
		bool serializedContention = true;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& (agent->getState() != core::Agent::State::Idle
				|| opposing->getState() != core::Agent::State::Idle); ++i)
		{
			bulkheadBuilding.advanceTick();
			auto const& snapshot = bulkheadBuilding.getSimulationSnapshot();
			if (!snapshot.traversalResources.empty()
				&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening
				&& snapshot.traversalPermits.empty()) waitedForOpen = true;
			if (snapshot.traversalPermits.size() > 1) serializedContention = false;
		}
		if (!waitedForOpen || !serializedContention
			|| agent->getSector() != bulkheadBuilding.getSector(right).get()
			|| opposing->getSector() != bulkheadBuilding.getSector(left).get()
			|| !bulkheadBuilding.getSimulationSnapshot().traversalRequests.empty()) return false;

		// Traversable windows contribute conditional topology, and only the clear,
		// fully-open state can receive a permit.
		core::Building windowBuilding("Window threshold", 6, 2);
		auto fore = windowBuilding.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 5, 1);
		auto back = windowBuilding.addRoom("Back", CORE_LAYER_BACK, 0, 0, 5, 1);
		core::Building::CreateWindowOptions windowOptions;
		windowOptions.traversable = true;
		windowOptions.initialState = core::Window::State::Open;
		auto window = windowBuilding.addSectorWindow(CORE_LAYER_FORE, 0, 2, 1, 1, windowOptions);
		windowBuilding.finishBuild();
		auto windowEdge = std::find_if(windowBuilding.getGraph()->getEdges().begin(),
			windowBuilding.getGraph()->getEdges().end(), [](auto const& edge)
			{ return edge->getType() == core::EdgeType::Window; });
		if (windowEdge == windowBuilding.getGraph()->getEdges().end()
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
		auto windowAgentId = windowBuilding.createAgent("Window traveller", fore, 0, 0.5f);
		auto windowAgent = windowBuilding.lookupAgent(windowAgentId).entity;
		windowAgent->setPath(twoNodePath(windowSource, windowDestination, *windowEdge), true);
		for (uint32_t i = 0; i < MaximumSimulationTicks && windowAgent->getState() != core::Agent::State::Idle; ++i)
			windowBuilding.advanceTick();
		return windowAgent->getSector() == windowBuilding.getSector(back).get()
			&& windowBuilding.getSimulationSnapshot().traversalRequests.empty();
	}

	bool pausedTopologyRebuildIsAtomicAndCleansOwnership()
	{
		core::Building building("Paused topology rebuild", 8, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 7, 1);
		auto back = building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 7, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		options.holdOpenSeconds = core::Building::getFixedTimestep() * 8.0f;
		auto door = building.addSectorDoor(0, 3, options);
		building.finishBuild();
		auto generation = building.getTopologyGeneration();
		auto oldGraph = building.getGraph();
		auto edge = *std::find_if(oldGraph->getEdges().begin(), oldGraph->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = building.createAgent("Rebuild traveller", fore, 0,
			source->getPosition().x - building.getSector(fore)->getPosition().x);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		building.advanceTicks(3);
		auto active = building.getSimulationSnapshot();
		if (active.traversalRequests.empty()) return false;

		// An active simulation cannot be structurally changed.
		bool rejected = false;
		try { building.addSectorMarker(fore, 0, 0.5f); }
		catch (std::exception const&) { rejected = true; }
		if (!rejected || building.isSimulationPaused()) return false;

		building.pauseSimulation();
		auto pausedTick = building.getSimulationTick();
		building.advanceTicks(10);
		auto paused = building.getSimulationSnapshot();
		if (!paused.paused || building.getSimulationTick() != pausedTick
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
		building.addSectorMarker(fore, 0, 0.5f, &marker);
		if (!building.isTraversalTopologyDirty() || !building.rebuildTraversalTopology()
			|| building.getGraph() == oldGraph || building.getTopologyGeneration() != generation + 1
			|| !building.getGraph()->getVertexByIdentifier(marker)
			|| !building.resumeSimulation()) return false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& agent->getState() != core::Agent::State::Idle; ++i) building.advanceTick();
		if (agent->getSector() != building.getSector(back).get()) return false;

		// Candidate failure leaves the previous graph installed, the simulation
		// paused, and removed handles permanently invalid.
		core::Building invalid("Invalid paused rebuild", 6, 2);
		invalid.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 5, 1);
		invalid.addRoom("Back", CORE_LAYER_BACK, 0, 0, 5, 1);
		auto invalidDoor = invalid.addSectorDoor(0, 2);
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
		core::Building building("Opportunistic remote door", 8, 2);
		auto corridor = building.addCorridor(0, 1, 5);
		building.addRoom("Destination", CORE_LAYER_BACK, 0, 0, 3, 1);
		building.addRoom("Far room", CORE_LAYER_BACK, 0, 4, 3, 1);
		uint32_t markerId;
		building.addSectorMarker(1, 0, 0.5f, &markerId);
		core::Building::CreateDoorOptions remote;
		remote.activationMode = core::DoorActivationMode::RemoteControlled;
		remote.controls[0] = true;
		auto created = building.addSectorDoor(0, 1, remote);
		building.addSectorDoor(0, 5);
		building.finishBuild();

		auto target = building.getGraph()->getVertexByIdentifier(markerId);
		std::vector<core::AgentId> ids = {
			building.createAgent("Early presser one", corridor, 0, 2.75f),
			building.createAgent("Early presser two", corridor, 0, 2.75f)
		};
		for (auto id : ids)
		{
			auto agent = building.lookupAgent(id).entity;
			auto path = building.getGraph()->calculatePath(agent, target);
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
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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
				{ return building.lookupAgent(id).entity->getState() == core::Agent::State::Idle; })) break;
		}

		return observedIndependentPressesBeforeDoorRequest
			&& std::all_of(ids.begin(), ids.end(), [&](auto id)
			{
				return building.lookupAgent(id).entity->getSector() == building.getSector(1).get();
			})
			&& building.lookupInteractionPoint(created.controls[0].interactionPoint).entity->getReach()
				== CORE_AGENT_MAX_HEIGHT * 0.4f;
	}

	bool remoteDoorUsesOnePhysicalOperatorAndSharedOperation()
	{
		core::Building building("Shared remote door", 7, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 6, 1);
		auto back = building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 6, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = true;
		options.controls[1] = true;
		auto created = building.addSectorDoor(0, 3, options);
		building.finishBuild();

		for (auto const& control : created.controls)
		{
			auto object = control.sector->getObject(control.index)->_getObject();
			auto button = std::dynamic_pointer_cast<core::Button>(object);
			if (!button || !control.interactionPoint
				|| button->getInteractionPointId() != control.interactionPoint
				|| !building.lookupInteractionPoint(control.interactionPoint)) return false;
		}

		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = building.createAgent("First remote waiter", fore, 0, 0.4f);
		auto secondId = building.createAgent("Second remote waiter", fore, 0, 0.6f);
		auto first = building.lookupAgent(firstId).entity;
		auto second = building.lookupAgent(secondId).entity;
		first->setPath(twoNodePath(source, destination, edge), true);
		second->setPath(twoNodePath(source, destination, edge), true);

		bool observedSharedPreparation = false;
		bool cancelledFirst = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& second->getState() != core::Agent::State::Idle; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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

		auto snapshot = building.getSimulationSnapshot();
		return observedSharedPreparation && cancelledFirst
			&& first->getSector() == building.getSector(fore).get()
			&& second->getState() == core::Agent::State::Idle
			&& second->getSector() == building.getSector(back).get()
			&& snapshot.deviceOperations.size() >= 1
			&& std::any_of(snapshot.deviceOperations.begin(), snapshot.deviceOperations.end(), [](auto const& operation)
				{ return operation.command.type == core::DeviceCommandType::OpenDoor
					&& operation.state == core::DeviceOperationState::Succeeded; });
	}

	bool remoteDoorWithoutReachableControlIsUnavailable()
	{
		core::Building building("Uncontrolled remote door", 6, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 5, 1);
		building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 5, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = false;
		options.controls[1] = false;
		auto created = building.addSectorDoor(0, 2, options);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = building.createAgent("Stranded remote waiter", fore, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);
		building.advanceTicks(400);
		auto snapshot = building.getSimulationSnapshot();
		return snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.traversalRequests.front().failureReason == core::TraversalFailureReason::NoReachableControl
			&& snapshot.deviceOperations.empty() && snapshot.interactionRequests.empty();
	}

	bool fairDoorQueuesServeBothSidesInStableOrder()
	{
		core::Building building("Fair two-sided door", 8, 2);
		auto fore = building.addRoom("Fore queue", CORE_LAYER_FORE, 0, 0, 7, 1);
		auto back = building.addRoom("Back queue", CORE_LAYER_BACK, 0, 0, 7, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = building.addSectorDoor(0, 3, options);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto foreVertex = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto backVertex = edge->getOtherVertex(foreVertex);

		std::vector<core::AgentId> ids = {
			building.createAgent("Fore first", fore, 0, 3.5f),
			building.createAgent("Back first", back, 0, 3.5f),
			building.createAgent("Fore second", fore, 0, 3.5f),
			building.createAgent("Back second", back, 0, 3.5f)
		};
		for (size_t i = 0; i < ids.size(); ++i)
		{
			auto source = i % 2 == 0 ? foreVertex : backVertex;
			auto destination = i % 2 == 0 ? backVertex : foreVertex;
			building.lookupAgent(ids[i]).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		bool observedSeparatedPositions = false;
		bool observedQueueDiagnostics = false;
		std::vector<core::AgentId> completionOrder;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2 && completionOrder.size() < ids.size(); ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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
					&& building.lookupAgent(id).entity->getState() == core::Agent::State::Idle)
				{
					completionOrder.push_back(id);
				}
			}
		}
		return completionOrder == ids && observedSeparatedPositions && observedQueueDiagnostics
			&& building.getSimulationSnapshot().traversalResources.front().crossingOwner == core::TraversalRequestId{};
	}

	bool queuePositionsPreferObjectProximityThenAgentProximity()
	{
		core::Building building("Nearest queue position", 8, 2);
		auto fore = building.addRoom("Queue room", CORE_LAYER_FORE, 0, 0, 7, 1);
		building.addRoom("Destination", CORE_LAYER_BACK, 0, 0, 7, 1);
		building.addSectorDoor(0, 3);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			building.createAgent("Queue head", fore, 0, 3.5f),
			building.createAgent("Second waiter", fore, 0, 3.5f),
			building.createAgent("Third waiter", fore, 0, 3.5f)
		};
		for (auto id : ids)
			building.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		building.advanceTicks(3);
		auto initial = building.getSimulationSnapshot();
		if (initial.traversalRequests.size() != 3) return false;
		auto initialThird = std::find_if(initial.traversalRequests.begin(), initial.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		if (initialThird == initial.traversalRequests.end() || !initialThird->hasQueuePosition) return false;
		auto const& initialLane = initial.traversalResources.front().queueLanes[initialThird->queueApproach];
		auto initialThirdTarget = initialLane.positions[initialThird->queuePosition].position;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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

	bool queuedCancellationReleasesAndAdvancesPositions()
	{
		core::Building building("Queue cancellation", 8, 2);
		auto fore = building.addRoom("Queue room", CORE_LAYER_FORE, 0, 0, 7, 1);
		building.addRoom("Destination", CORE_LAYER_BACK, 0, 0, 7, 1);
		auto created = building.addSectorDoor(0, 3);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = building.createAgent("First", fore, 0, 3.5f);
		auto cancelledId = building.createAgent("Cancelled", fore, 0, 3.5f);
		auto lastId = building.createAgent("Last", fore, 0, 3.5f);
		for (auto id : { firstId, cancelledId, lastId })
		{
			building.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}
		building.advanceTicks(3);
		auto before = building.getSimulationSnapshot();
		if (before.traversalRequests.size() != 3
			|| std::count_if(before.traversalRequests.begin(), before.traversalRequests.end(),
				[](auto const& request) { return request.queueTicket && request.hasQueuePosition; }) != 3)
		{
			return false;
		}
		building.lookupAgent(cancelledId).entity->clearPath();
		auto after = building.getSimulationSnapshot();
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
		building.advanceTicks(MaximumSimulationTicks);
		return building.lookupAgent(firstId).entity->getState() == core::Agent::State::Idle
			&& building.lookupAgent(lastId).entity->getState() == core::Agent::State::Idle
			&& building.lookupAgent(cancelledId).entity->getSector() == building.getSector(fore).get();
	}

	bool resilientWaitingRetainsPriorityAndExpiresPermits()
	{
		core::Building building("Resilient door waiting", 8, 2);
		auto fore = building.addRoom("Waiting side", CORE_LAYER_FORE, 0, 0, 7, 1);
		building.addRoom("Destination side", CORE_LAYER_BACK, 0, 0, 7, 1);
		auto created = building.addSectorDoor(0, 3);
		building.finishBuild();
		auto initialEdge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto initialSource = initialEdge->getVertex(0)->getSector()->getIndex() == fore
			? initialEdge->getVertex(0) : initialEdge->getVertex(1);

		// One physical position deliberately forces logical overflow. Runtime queue
		// geometry is a structural edit and therefore uses the paused rebuild seam.
		building.pauseSimulation();
		auto sourceSectorId = core::SectorId{ (uint64_t)fore + 1 };
		if (!building.configureDoorQueueLane(created.traversalResource, sourceSectorId,
			initialSource->getPosition(), { -1.0f, 0.0f }, 0.0f)
			|| !building.rebuildTraversalTopology() || !building.resumeSimulation()) return false;
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			building.createAgent("Queue head", fore, 0, 3.5f),
			building.createAgent("Overflow one", fore, 0, 3.5f),
			building.createAgent("Overflow two", fore, 0, 3.5f)
		};
		for (auto id : ids) building.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		building.advanceTicks(2);
		auto queued = building.getSimulationSnapshot();
		if (queued.traversalRequests.size() != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return (bool)request.queueTicket; }) != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return request.hasQueuePosition; }) != 1)
		{
			return false;
		}

		auto firstRequest = queued.traversalRequests.front();
		building.lookupAgent(ids.front()).entity->setPath(twoNodePath(source, destination, edge), true);
		auto compatible = building.getSimulationSnapshot();
		auto retained = std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == compatible.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		// Changing the immediate authority is incompatible and must release the old
		// logical ticket before a later compatible route can queue afresh.
		auto oldOverflow = *std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		building.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination,
			std::make_shared<core::SectorEdge>()), true);
		auto incompatible = building.getSimulationSnapshot();
		if (std::any_of(incompatible.traversalRequests.begin(), incompatible.traversalRequests.end(),
			[&](auto const& request) { return request.id == oldOverflow.id; })) return false;
		building.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination, edge), true);
		building.advanceTicks(2);
		auto fresh = building.getSimulationSnapshot();
		auto freshOverflow = std::find_if(fresh.traversalRequests.begin(), fresh.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		if (freshOverflow == fresh.traversalRequests.end() || freshOverflow->queueTicket == oldOverflow.queueTicket)
			return false;

		// Route estimation observes queue demand but creates no coordination state.
		auto requestCount = fresh.traversalRequests.size();
		auto permitCount = fresh.traversalPermits.size();
		if (edge->getWeight(destination, building.lookupAgent(ids.front()).entity, true) <= 0.0f
			|| building.getSimulationSnapshot().traversalRequests.size() != requestCount
			|| building.getSimulationSnapshot().traversalPermits.size() != permitCount) return false;

		// A deliberately short no-progress deadline expires the coincident threshold
		// crossing. The request and ticket survive and a fresh permit is assigned.
		auto policy = building.getTraversalWaitingPolicy();
		policy.permitProgressTimeoutTicks = 1;
		building.setTraversalWaitingPolicy(policy);
		core::TraversalPermitId firstPermit;
		core::TraversalPermitId replacementPermit;
		for (uint32_t i = 0; i < MaximumSimulationTicks && !replacementPermit; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			for (auto const& permit : snapshot.traversalPermits)
			{
				if (permit.owner != ids.front()) continue;
				if (!firstPermit) firstPermit = permit.id;
				else if (permit.id != firstPermit) replacementPermit = permit.id;
			}
		}
		if (!firstPermit || !replacementPermit) return false;
		auto afterExpiry = building.getSimulationSnapshot();
		retained = std::find_if(afterExpiry.traversalRequests.begin(), afterExpiry.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == afterExpiry.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		policy.permitProgressTimeoutTicks = 120;
		building.setTraversalWaitingPolicy(policy);
		building.advanceTicks(MaximumSimulationTicks * 2);
		return std::all_of(ids.begin(), ids.end(), [&](auto id)
		{
			auto agent = building.lookupAgent(id).entity;
			return agent->getState() == core::Agent::State::Idle
				&& agent->getSector() == destination->getSector().get();
		}) && building.getSimulationSnapshot().traversalRequests.empty()
			&& building.getSimulationSnapshot().traversalPermits.empty();
	}

	bool wideDoorLanesAndGracefulDisableAreSafe()
	{
		core::Building building("Wide safe door", 9, 2);
		auto fore = building.addRoom("Wide fore", CORE_LAYER_FORE, 0, 0, 8, 1);
		auto back = building.addRoom("Wide back", CORE_LAYER_BACK, 0, 0, 8, 1);
		core::Building::CreateDoorOptions options;
		options.width = 2;
		options.crossingLanes = 2;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = building.addSectorDoor(0, 3, options);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			building.createAgent("Wide first", fore, 0, 4.0f),
			building.createAgent("Wide second", fore, 0, 4.0f),
			building.createAgent("Disabled waiter", fore, 0, 4.0f)
		};
		for (auto id : ids) building.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		bool disabledWithTwoCrossings = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 2 || snapshot.traversalResources.front().crossingLanes.size() != 2)
				return false;
			if (!disabledWithTwoCrossings && snapshot.traversalPermits.size() == 2)
			{
				auto const& lanes = snapshot.traversalResources.front().crossingLanes;
				if (!lanes[0].owner || !lanes[1].owner || lanes[0].owner == lanes[1].owner
					|| snapshot.traversalResources.front().crossingLeaseCount != 2)
					return false;
				disabledWithTwoCrossings = building.setTraversalResourceEnabled(created.traversalResource, false);
			}
			if (disabledWithTwoCrossings
				&& building.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& building.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}
		building.advanceTicks(3);
		auto snapshot = building.getSimulationSnapshot();
		auto denied = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		return disabledWithTwoCrossings
			&& building.lookupAgent(ids[0]).entity->getSector() == building.getSector(back).get()
			&& building.lookupAgent(ids[1]).entity->getSector() == building.getSector(back).get()
			&& building.lookupAgent(ids[2]).entity->getSector() == building.getSector(fore).get()
			&& denied != snapshot.traversalRequests.end()
			&& denied->state == core::TraversalRequestState::Denied
			&& denied->failureReason == core::TraversalFailureReason::ResourceDisabled
			&& snapshot.traversalResources.front().crossingLeaseCount == 0;
	}

	bool doorLeasesAndSensorObservationsPreventUnsafeClosure()
	{
		core::Building building("Door observation safety", 7, 2);
		auto fore = building.addRoom("Sensor fore", CORE_LAYER_FORE, 0, 0, 6, 1);
		building.addRoom("Sensor back", CORE_LAYER_BACK, 0, 0, 6, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		options.holdOpenSeconds = core::Building::getFixedTimestep() * 2.0f;
		auto created = building.addSectorDoor(0, 3, options);
		building.finishBuild();
		auto sensor = core::DoorSensorId{ 1 };
		if (!building.setDoorSensorObservation(created.traversalResource, sensor,
			core::DoorSensorObservation::Presence)) return false;
		building.advanceTicks(90);
		auto lease = building.acquireDoorOpenLease(created.traversalResource);
		building.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		building.advanceTicks(30);
		auto snapshot = building.getSimulationSnapshot();
		if (!lease || snapshot.traversalResources.front().doorState != core::DoorSnapshotState::Open
			|| snapshot.traversalResources.front().externalOpenLeaseCount != 1) return false;

		auto actor = building.createAgent("Close operator", fore, 0, 3.5f);
		core::DeviceCommand close;
		close.type = core::DeviceCommandType::OpenDoor;
		close.desiredState = false;
		close.traversalResource = created.traversalResource;
		auto point = building.createInteractionPoint("Close door", core::SectorId{ (uint64_t)fore + 1 },
			{ 3.5f, 0.0f }, 1.0f, 0.0f, { { close, core::InteractionBindingRequirement::Required } });
		auto closeRequest = building.requestInteraction(point, actor);
		building.advanceTicks(4);
		if (building.lookupInteractionRequest(closeRequest).entity->getResult() != core::InteractionResult::Rejected
			|| !building.releaseDoorOpenLease(created.traversalResource, lease)) return false;

		building.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		building.advanceTicks(20);
		if (building.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Open) return false;
		building.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Clear);
		for (uint32_t i = 0; i < 10 && building.getSimulationSnapshot().traversalResources.front().doorState
			!= core::DoorSnapshotState::Closing; ++i) building.advanceTick();
		if (building.getSimulationSnapshot().traversalResources.front().doorState != core::DoorSnapshotState::Closing) return false;
		building.setDoorSensorObservation(created.traversalResource, sensor, core::DoorSensorObservation::Obstruction);
		building.advanceTick();
		snapshot = building.getSimulationSnapshot();
		return snapshot.traversalResources.front().obstructionObserved
			&& snapshot.traversalResources.front().doorState == core::DoorSnapshotState::Opening;
	}

	bool finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed()
	{
		core::Building building("Finite ladder", 4, 4);
		auto lower = building.addCorridor(0, 0, 3);
		auto upper = building.addCorridor(2, 0, 3);
		core::Building::CreateLadderOptions options{ 3, false, true };
		options.agentSpacing = 10.0f; // Deliberately derive a single capacity slot.
		auto created = building.addLadder(0, 1, options);
		building.finishBuild();
		if (!created.traversalResource) return false;

		auto const& graph = building.getGraph();
		auto target = graph->getClosestVertexInSector(building.getSector(upper).get(), { 1.5f, 2.0f });
		if (!target) return false;
		std::vector<core::AgentId> ids = {
			building.createAgent("First climber", lower, 0, 1.5f),
			building.createAgent("Second climber", lower, 0, 1.5f),
			building.createAgent("Cancelled climber", lower, 0, 1.5f)
		};
		for (auto id : ids)
		{
			auto agent = building.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedFull = false;
		bool cancelledWaiter = false;
		uint64_t climbStarted = 0;
		uint64_t climbFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isLadder
				|| resource->capacity != 1
				|| resource->occupantCount + resource->admissionReservationCount > resource->capacity
				|| resource->capacityPositions.size() != resource->capacity)
				return false;
			if (resource->occupantCount == 1)
			{
				observedFull = true;
				if (!climbStarted && building.lookupAgent(ids[0]).entity->getState()
					== core::Agent::State::TraversingEdge)
					climbStarted = building.getSimulationTick();
				if (!cancelledWaiter)
				{
					building.lookupAgent(ids[2]).entity->clearPath();
					cancelledWaiter = true;
				}
			}
			if (climbStarted && !climbFinished
				&& building.lookupAgent(ids[0]).entity->getSector() == building.getSector(upper).get())
				climbFinished = building.getSimulationTick();
			if (building.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& building.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}

		auto snapshot = building.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		// Two vertical units at 0.25 units/second require about 480 fixed ticks;
		// this also detects accidentally using walking speed.
		return observedFull && cancelledWaiter && climbStarted && climbFinished
			&& climbFinished - climbStarted >= 470
			&& building.lookupAgent(ids[0]).entity->getSector() == building.getSector(upper).get()
			&& building.lookupAgent(ids[1]).entity->getSector() == building.getSector(upper).get()
			&& building.lookupAgent(ids[2]).entity->getSector() == building.getSector(lower).get()
			&& resource != snapshot.traversalResources.end()
			&& resource->occupantCount == 0 && resource->admissionReservationCount == 0
			&& resource->admissionQueue.empty();
	}

	bool extensibleForceBridgeCompletesThroughPhysicalControl()
	{
		core::Building building("Extensible force bridge", 6, 4);
		auto room = building.addRoom("Bridge room", CORE_LAYER_BACK, 0, 0, 4, 3);
		building.addSectorWalkway(room, 1, 0);
		building.addSectorWalkway(room, 1, 2);
		building.addSectorWalkway(room, 1, 3);
		core::Building::CreateForceBridgeOptions options;
		options.fromSide = CORE_SIDE_LEFT;
		options.extensible = true;
		options.startExtended = false;
		options.controlCount = 1;
		auto bridge = building.addSectorForceBridge(room, 1, 1, options);
		building.finishBuild();

		auto edgeIt = std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::ForceBridge; });
		if (edgeIt == building.getGraph()->getEdges().end()) return false;
		auto edge = *edgeIt;
		auto source = edge->getVertex(0)->getPosition().x < edge->getVertex(1)->getPosition().x
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = building.createAgent("Bridge traveller", room, 1, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		agent->setPath(twoNodePath(source, destination, edge), true);

		bool sawPreparation = false;
		bool sawExtensionLease = false;
		while (agent->getState() != core::Agent::State::Idle
			&& building.getSimulationTick() < MaximumSimulationTicks)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == bridge.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isForceBridge
				|| !resource->isExtensible) return false;
			sawPreparation = sawPreparation || !snapshot.deviceOperations.empty();
			sawExtensionLease = sawExtensionLease || resource->extensionRequestLeaseCount > 0;
		}
		auto final = building.getSimulationSnapshot();
		auto resource = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& value) { return value.id == bridge.traversalResource; });
		return sawPreparation && sawExtensionLease
			&& agent->getState() == core::Agent::State::Idle
			&& agent->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f
			&& resource != final.traversalResources.end() && resource->extended
			&& resource->extensionRequestLeaseCount == 0
			&& resource->extensionOccupantLeaseCount == 0
			&& final.traversalRequests.empty() && final.traversalPermits.empty();
	}

	bool extensibleLadderUsesDesiredStateAndLeases()
	{
		core::Building building("Extensible ladder", 4, 4);
		auto lower = building.addCorridor(0, 0, 3);
		auto upper = building.addCorridor(2, 0, 3);
		core::Building::CreateLadderOptions options{ 3, true, false };
		options.agentSpacing = 10.0f;
		auto created = building.addLadder(0, 1, options);
		building.finishBuild();

		auto target = building.getGraph()->getClosestVertexInSector(
			building.getSector(upper).get(), { 1.5f, 2.0f });
		auto first = building.createAgent("Extension owner", lower, 0, 1.5f);
		auto second = building.createAgent("Shared extension owner", lower, 0, 1.5f);
		for (auto id : { first, second })
		{
			auto agent = building.lookupAgent(id).entity;
			auto path = building.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool sawSharedOperation = false;
		bool sawLease = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isExtensible) return false;
			sawLease = sawLease || resource->extensionRequestLeaseCount > 0
				|| resource->extensionOccupantLeaseCount > 0;
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SetExtendedState
					&& operation.command.desiredState && operation.requesters.size() == 2)
					sawSharedOperation = true;
			if (building.lookupAgent(first).entity->getState() == core::Agent::State::Idle
				&& building.lookupAgent(second).entity->getState() == core::Agent::State::Idle) break;
		}
		auto snapshot = building.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		return sawSharedOperation && sawLease && resource != snapshot.traversalResources.end()
			&& resource->extended && resource->extensionRequestLeaseCount == 0
			&& resource->extensionOccupantLeaseCount == 0
			&& building.lookupAgent(first).entity->getSector() == building.getSector(upper).get()
			&& building.lookupAgent(second).entity->getSector() == building.getSector(upper).get();
	}

	bool directionalLadderBoundsBatchesAndPreventsOpposingAdmission()
	{
		core::Building building("Directional ladder", 4, 4);
		auto lower = building.addCorridor(0, 0, 3);
		auto upper = building.addCorridor(2, 0, 3);
		core::Building::CreateLadderOptions options{ 3, false, true };
		options.agentSpacing = 1.0f;
		options.directionalBatchLimit = 2;
		auto created = building.addLadder(0, 1, options);
		building.finishBuild();

		auto graph = building.getGraph();
		auto upperTarget = graph->getClosestVertexInSector(building.getSector(upper).get(), { 1.5f, 2.0f });
		auto lowerTarget = graph->getClosestVertexInSector(building.getSector(lower).get(), { 1.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;
		std::vector<core::AgentId> ascending = {
			building.createAgent("Ascending one", lower, 0, 1.5f),
			building.createAgent("Ascending two", lower, 0, 1.5f),
			building.createAgent("Ascending next batch", lower, 0, 1.5f)
		};
		auto descending = building.createAgent("Descending waiter", upper, 0, 1.5f);
		for (auto id : ascending)
		{
			auto agent = building.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, upperTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}
		{
			auto agent = building.lookupAgent(descending).entity;
			auto path = graph->calculatePath(agent, lowerTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedOppositeWaiting = false;
		uint64_t descendingFinished = 0;
		uint64_t thirdAscendingFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 4; ++i)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || resource->capacity != 2
				|| resource->occupantCount + resource->admissionReservationCount > 2
				|| resource->directionalBatchLimit != 2) return false;

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
			observedOppositeWaiting = observedOppositeWaiting
				|| (resource->activeDirection == core::TraversalDirection::Ascending
					&& resource->descendingWaitingCount == 1
					&& resource->directionalBatchCount == 2);
			if (!descendingFinished && building.lookupAgent(descending).entity->getState() == core::Agent::State::Idle)
				descendingFinished = building.getSimulationTick();
			if (!thirdAscendingFinished && building.lookupAgent(ascending[2]).entity->getState() == core::Agent::State::Idle)
				thirdAscendingFinished = building.getSimulationTick();
			if (descendingFinished && thirdAscendingFinished) break;
		}
		return observedOppositeWaiting && descendingFinished && thirdAscendingFinished
			&& descendingFinished < thirdAscendingFinished;
	}

	bool staircaseCoordinationIsExplicitlyOptIn()
	{
		core::Building ordinary("Ordinary staircase", 5, 3);
		ordinary.addCorridor(0, 0, 4);
		ordinary.addCorridor(1, 0, 4);
		ordinary.addStaircase(0, 1, 2, CORE_SIDE_LEFT);
		ordinary.finishBuild();
		if (!ordinary.getSimulationSnapshot().traversalResources.empty()) return false;

		core::Building narrow("Narrow staircase", 5, 3);
		narrow.addCorridor(0, 0, 4);
		narrow.addCorridor(1, 0, 4);
		core::Building::CreateStaircaseOptions options{ 2, CORE_SIDE_LEFT };
		options.directionalCapacity = 1;
		options.directionalBatchLimit = 3;
		auto created = narrow.addStaircase(0, 1, options);
		narrow.finishBuild();
		auto snapshot = narrow.getSimulationSnapshot();
		if (!created.traversalResource || snapshot.traversalResources.size() != 1
			|| !snapshot.traversalResources.front().isNarrowStaircase
			|| snapshot.traversalResources.front().capacity != 1
			|| snapshot.traversalResources.front().directionalBatchLimit != 3) return false;
		return std::all_of(narrow.getGraph()->getEdges().begin(), narrow.getGraph()->getEdges().end(),
			[&](auto const& edge)
			{
				return edge->getType() != core::EdgeType::Staircase
					|| edge->getTraversalResourceId() == created.traversalResource;
			});
	}

	bool platformLiftAuthoringReconcilesWalkwayStops()
	{
		{
			core::Building offset("PlatformLift initial floor", 8, 6);
			auto offsetRoom = offset.addRoom("Offset room", CORE_LAYER_FORE, 1, 0, 7, 4);
			offset.addSectorWalkway(offsetRoom, 2, 2);
			offset.addSectorWalkway(offsetRoom, 2, 3);
			core::Building::CreateLiftOptions offsetOptions;
			offsetOptions.stopOffsets = { 0, 2 };
			auto placed = offset.addSectorPlatformLift(offsetRoom, 0, 2, offsetOptions);
			auto object = std::dynamic_pointer_cast<const core::LiftSectorObject>(
				placed.lift.sector->getObject(placed.lift.index));
			offset.finishBuild();
			auto snapshot = offset.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == placed.traversalResource; });
			std::shared_ptr<const core::SectorObject> hitObject;
			auto hit = offset.getObjectAtPosition(CORE_LAYER_FORE, 2.5f, 0.975f, &hitObject);
			if (!object || std::abs(object->getLift()->getPosition().y - 1.0f) > 0.001f
				|| resource == snapshot.traversalResources.end()
				|| std::abs(resource->liftPosition - 1.0f) > 0.001f
				|| hit.get() != object->getLift().get() || hitObject != object) return false;
		}
		core::Building building("PlatformLift authoring", 8, 5);
		auto room = building.addRoom("Lift room", CORE_LAYER_FORE, 0, 0, 7, 4);
		building.addSectorWalkway(room, 1, 2);
		building.addSectorWalkway(room, 1, 3);
		building.addSectorWalkway(room, 3, 2);
		building.addSectorWalkway(room, 3, 3);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1; options.stopOffsets = { 0, 1 };
		auto created = building.addSectorPlatformLift(room, 0, 2, options);
		building.finishBuild(); building.pauseSimulation();
		options.stopOffsets = { 0, 1, 3 };
		auto edit = building.planPlatformLiftEdit(room, created.lift.index, options);
		if (!edit.valid) return false;
		auto liftObject = building.applyPlatformLiftEdit(edit);
		if (!liftObject) return false;

		auto findObject = [&](core::SectorObjectType type, uint32_t x, uint32_t y)
		{
			auto sector = building.getSector(room);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (object && object->getObjectType() == type
					&& object->getCellX() == x && object->getCellY() == y) return i;
			}
			return ~0u;
		};
		auto lower = findObject(core::SectorObjectType::Walkway, 2, 1);
		auto removal = building.planRemoveSectorWalkway(room, lower);
		if (!removal.valid || !removal.consequences.empty() || !building.applyWalkwayEdit(removal)) return false;
		auto liftIndex = findObject(core::SectorObjectType::Lift, 2, 0);
		core::Building::CreateLiftOptions retained;
		if (liftIndex == ~0u || !building.getPlatformLiftOptions(room, liftIndex, retained)
			|| retained.stopOffsets != std::vector<uint32_t>({ 0, 3 })) return false;
		auto upper = findObject(core::SectorObjectType::Walkway, 2, 3);
		removal = building.planRemoveSectorWalkway(room, upper);
		if (!removal.valid || removal.consequences.empty() || !building.applyWalkwayEdit(removal)) return false;
		if (findObject(core::SectorObjectType::Lift, 2, 0) != ~0u) return false;

		core::Building resized("PlatformLift resize", 8, 5);
		auto resizedRoom = resized.addRoom("Lift room", CORE_LAYER_FORE, 0, 0, 7, 4);
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

		core::Building moving("PlatformLift movement", 9, 5);
		auto movingRoom = moving.addRoom("Lift room", CORE_LAYER_FORE, 0, 0, 8, 4);
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
		core::Building building("Open platform lift", 7, 5);
		auto room = building.addRoom("Platform room", CORE_LAYER_FORE, 0, 0, 6, 4);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 1;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		building.addSectorWalkway(room, 2, 2);
		building.addSectorWalkway(room, 2, 3);
		auto created = building.addSectorPlatformLift(room, 0, 2, options);
		building.finishBuild();
		if (!created.traversalResource || !created.interiorSelector || created.buttons.size() != 2)
			return false;

		auto target = building.getGraph()->getClosestVertexInSector(
			building.getSector(room).get(), { 2.5f, 2.0f });
		auto passengerId = building.createAgent("Platform passenger", room, 0, 0.5f);
		auto passenger = building.lookupAgent(passengerId).entity;
		auto path = building.getGraph()->calculatePath(passenger, target);
		if (!path || std::count_if(path->nodes.begin(), path->nodes.end(), [](auto const& node)
			{ return node.edge && node.edge->getType() == core::EdgeType::Lift; }) != 1) return false;
		passenger->setPath(path, true);

		bool sawBoundary = false;
		bool sawOnboard = false;
		bool sawAttachedMotion = false;
		bool sawDestinationConfirmation = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 6
			&& passenger->getState() != core::Agent::State::Idle; ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto platform = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (platform == snapshot.traversalResources.end() || !platform->isOpenPlatformLift
				|| platform->liftCarDoorOpen
				|| platform->occupantCount + platform->admissionReservationCount > options.capacity)
				return false;
			sawBoundary = sawBoundary || platform->virtualBoundaryCrossingCount == 1;
			sawOnboard = sawOnboard || platform->occupantCount == 1;
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
		}
		auto final = building.getSimulationSnapshot();
		auto platform = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawBoundary && sawOnboard && sawAttachedMotion && sawDestinationConfirmation
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == building.getSector(room).get()
			&& passenger->getGlobalPosition().distanceTo(target->getPosition()) < 0.001f
			&& platform != final.traversalResources.end() && platform->occupantCount == 0
			&& platform->virtualBoundaryCrossingCount == 0;
	}

	bool singlePassengerCompletesTwoStopLiftJourney()
	{
		core::Building building("Two-stop lift journey", 6, 4);
		auto lower = building.addCorridor(0, 0, 5);
		auto upper = building.addCorridor(2, 0, 5);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		auto created = building.addLift(0, 2, options);
		building.finishBuild();
		if (!created.traversalResource || created.doors.size() != 2 || !created.interiorSelector)
			return false;
		auto initial = building.getSimulationSnapshot();
		auto initialLift = std::find_if(initial.traversalResources.begin(),
			initial.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		if (initialLift == initial.traversalResources.end() || initialLift->capacity != 2) return false;
		auto target = building.getGraph()->getClosestVertexInSector(
			building.getSector(upper).get(), { 2.5f, 2.0f });
		auto passengerId = building.createAgent("Lift passenger", lower, 0, 0.5f);
		auto passenger = building.lookupAgent(passengerId).entity;
		auto path = building.getGraph()->calculatePath(passenger, target);
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
		bool sawConfirmedDestination = false;
		bool sawMovingAttachedPassenger = false;
		bool sawQueuedDebug = false, sawEnteringDebug = false;
		bool sawInLiftDebug = false, sawExitingDebug = false;
		bool climbedTowardLandingCallButton = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 4
			&& passenger->getState() != core::Agent::State::Idle; ++i)
		{
			building.advanceTick();
			if (passenger->getSector() == building.getSector(lower).get()
				&& passenger->getGlobalPosition().y > 0.001f)
				climbedTowardLandingCallButton = true;
			auto snapshot = building.getSimulationSnapshot();
			auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (lift == snapshot.traversalResources.end() || !lift->isLift) return false;
			for (auto const& debug : lift->liftAgents)
			{
				if (debug.agent != passengerId || debug.targetStop != 1
					|| std::abs(debug.targetFloor - 2.0f) > 0.001f) continue;
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
			sawOnboard = sawOnboard || (lift->liftPassenger == passengerId
				&& passenger->getSector() == building.getSector(created.lift.sector->getIndex()).get());
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
		}
		auto final = building.getSimulationSnapshot();
		auto lift = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawIntentWithoutDispatch && sawReservedCapacity && sawOnboard
			&& sawConfirmedDestination && sawMovingAttachedPassenger
			&& sawQueuedDebug && sawEnteringDebug && sawInLiftDebug && sawExitingDebug
			&& !climbedTowardLandingCallButton
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == building.getSector(upper).get()
			&& lift != final.traversalResources.end() && !lift->liftPassenger
			&& lift->occupantCount == 0;
	}

	bool waitingLiftPassengersFillArrivingCar()
	{
		core::Building building("Arriving lift boards waiting capacity", 7, 4);
		auto lower = building.addCorridor(0, 0, 6);
		auto upper = building.addCorridor(2, 0, 6);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = building.addLift(0, 2, options);
		building.finishBuild();

		auto lowerTarget = building.getGraph()->getClosestVertexInSector(
			building.getSector(lower).get(), { 2.5f, 0.0f });
		auto upperTarget = building.getGraph()->getClosestVertexInSector(
			building.getSector(upper).get(), { 2.5f, 2.0f });
		if (!lowerTarget || !upperTarget) return false;
		auto downId = building.createAgent("Down passenger", upper, 0, 2.0f);
		auto down = building.lookupAgent(downId).entity;
		auto downPath = building.getGraph()->calculatePath(down, lowerTarget);
		if (!downPath) return false;
		down->setPath(downPath, true);

		bool descending = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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
			auto id = building.createAgent("Waiting passenger", lower, 0, 1.7f - i * 0.35f);
			auto agent = building.lookupAgent(id).entity;
			auto path = building.getGraph()->calculatePath(agent, upperTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}
		bool sawBothWaiting = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 5; ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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
		core::Building building("Finite lift", 7, 4);
		auto lower = building.addCorridor(0, 0, 6);
		auto upper = building.addCorridor(2, 0, 6);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = building.addLift(0, 2, options);
		building.finishBuild();
		auto initial = building.getSimulationSnapshot();
		for (auto const& door : created.doors)
		{
			auto resource = std::find_if(initial.traversalResources.begin(),
				initial.traversalResources.end(),
				[&](auto const& candidate) { return candidate.id == door.traversalResource; });
			if (resource == initial.traversalResources.end()) return false;
			bool foundCarLane = false, foundCorridorLane = false;
			for (auto const& lane : resource->queueLanes)
			{
				auto sector = building.getSector((uint32_t)lane.sector.value - 1);
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
		auto target = building.getGraph()->getClosestVertexInSector(
			building.getSector(upper).get(), { 2.5f, 2.0f });
		std::vector<core::AgentId> passengers;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto id = building.createAgent("Capacity passenger", lower, 0, 0.3f + i * 0.15f);
			auto agent = building.lookupAgent(id).entity;
			auto path = building.getGraph()->calculatePath(agent, target);
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
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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
					auto agent = building.lookupAgent(id).entity;
					return agent && agent->getState() == core::Agent::State::Idle
						&& agent->getSector() == building.getSector(upper).get();
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
		core::Building building("LOOK lift", 7, 7);
		auto lower = building.addCorridor(0, 0, 6);
		auto middle = building.addCorridor(2, 0, 6);
		auto upper = building.addCorridor(5, 0, 6);
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2, 5 }; // deliberately non-uniform
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 3.0f;
		auto created = building.addLift(0, 2, options);
		building.finishBuild();

		auto graph = building.getGraph();
		auto lowerTarget = graph->getClosestVertexInSector(building.getSector(lower).get(), { 2.5f, 0.0f });
		auto middleTarget = graph->getClosestVertexInSector(building.getSector(middle).get(), { 2.5f, 2.0f });
		auto upperTarget = graph->getClosestVertexInSector(building.getSector(upper).get(), { 2.5f, 5.0f });
		if (!lowerTarget || !middleTarget || !upperTarget) return false;

		struct Journey { core::AgentId id; std::shared_ptr<const core::Vertex> target; };
		std::vector<Journey> journeys = {
			{ building.createAgent("Up through run", lower, 0, 2.5f), upperTarget },
			{ building.createAgent("Down middle", middle, 0, 2.5f), lowerTarget },
			{ building.createAgent("Down upper", upper, 0, 2.5f), middleTarget }
		};
		for (auto const& journey : journeys)
		{
			auto agent = building.lookupAgent(journey.id).entity;
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
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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
				{ return building.lookupAgent(journey.id).entity->getState() == core::Agent::State::Idle; }))
				break;
		}

		return observedCoalescedMiddleDemand
			&& serviceOrder == std::vector<uint32_t>({ 0, 2, 1, 0 })
			&& building.lookupAgent(journeys[0].id).entity->getSector() == building.getSector(upper).get()
			&& building.lookupAgent(journeys[1].id).entity->getSector() == building.getSector(lower).get()
			&& building.lookupAgent(journeys[2].id).entity->getSector() == building.getSector(middle).get();
	}

	bool singleCarriageShuttleUsesTransportJourneyProtocol()
	{
		core::Building building("Single carriage shuttle", 12, 2);
		auto left = building.addRoom("Left platform", CORE_LAYER_FORE, 0, 0, 3, 1);
		auto right = building.addRoom("Right platform", CORE_LAYER_FORE, 0, 7, 3, 1);
		core::Building::CreateShuttleOptions options{ 1, 3, { 0, 7 }, 0 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = building.addShuttle(0, 0, 11, options);
		building.finishBuild();
		if (!created.traversalResource || !created.interiorSelector || created.doors.size() != 2)
			return false;
		auto initial = building.getSimulationSnapshot();
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
		auto target = building.getGraph()->getClosestVertexInSector(
			building.getSector(right).get(), { 8.5f, 0.0f });
		if (!target) return false;
		std::vector<core::AgentId> passengers;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto id = building.createAgent("Shuttle passenger", left, 0, 1.0f + i * 0.15f);
			auto agent = building.lookupAgent(id).entity;
			auto path = building.getGraph()->calculatePath(agent, target);
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
		bool sawAttachedMotion = false;
		bool sawDisembarkBeforeBoard = false;
		std::map<core::AgentId, float> previousShuttleX;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 14; ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end() || !shuttle->isShuttle
				|| shuttle->occupantCount + shuttle->admissionReservationCount > options.capacity)
				return false;
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::CallShuttle
					&& operation.state == core::DeviceOperationState::Succeeded) sawPhysicalCall = true;
			for (auto passengerId : passengers)
			{
				auto passenger = building.lookupAgent(passengerId).entity;
				if (passenger->getSector() != building.getSector(created.shuttle.sector->getIndex()).get())
				{
					previousShuttleX.erase(passengerId);
					continue;
				}
				auto x = passenger->getGlobalPosition().x;
				if (auto previous = previousShuttleX.find(passengerId); previous != previousShuttleX.end())
				{
					if (std::abs(x - previous->second) > passenger->getWalkSpeed()
						* building.getFixedTimestep() + 0.001f) return false;
				}
				previousShuttleX[passengerId] = x;
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
				auto agent = building.lookupAgent(passenger).entity;
				if (agent->getSector() == building.getSector(left).get()
					&& std::abs(agent->getGlobalPosition().y) > 0.001f) return false;
			}
			if (shuttle->liftMoving)
				for (auto const& passenger : passengers)
				{
					auto agent = building.lookupAgent(passenger).entity;
					if (agent->getSector() == building.getSector(created.shuttle.sector->getIndex()).get()
						&& agent->getGlobalPosition().x >= shuttle->liftPosition)
						sawAttachedMotion = true;
				}
			if (std::all_of(passengers.begin(), passengers.end(), [&](auto id)
				{ auto agent = building.lookupAgent(id).entity; return agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == building.getSector(right).get(); })) break;
		}
		auto final = building.getSimulationSnapshot();
		auto shuttle = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawPhysicalCall && sawFullWithWaiter && sawAttachedMotion && sawDisembarkBeforeBoard
			&& shuttle != final.traversalResources.end() && shuttle->occupantCount == 0
			&& shuttle->admissionReservationCount == 0;
	}

	bool shuttleArrivalFollowsFinalPathNodeWithoutBacktracking()
	{
		core::Building building("Multi-door Shuttle arrival", 48, 6);
		auto left = building.addCorridor(1, 1, 6);
		auto right = building.addCorridor(1, 15, 6);
		core::Building::CreateShuttleOptions options{ 1, 3, { 0, 13 }, 0 };
		options.capacity = 5;
		options.doorMask = 0b101;
		options.minimumDwellSeconds = 0.75f;
		options.maximumBoardingSeconds = 5.0f;
		auto created = building.addShuttle(1, 3, 16, options);
		building.finishBuild();

		auto target = building.getGraph()->getClosestVertexInSector(
			building.getSector(right).get(), { 20.5f, 1.0f });
		if (!target) return false;
		auto passengerId = building.createAgent("Multi-door passenger", left, 0, 0.4f);
		auto passenger = building.lookupAgent(passengerId).entity;
		auto path = building.getGraph()->calculatePath(passenger, target);
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
		bool sawForwardAlignment = false;
		bool sawAllDestinationDoorsOpen = false;
		bool sawBoardingWindowAfterDisembark = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 14; ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()) return false;
			passenger = building.lookupAgent(passengerId).entity;
			if (!shuttle->liftMoving && shuttle->liftCurrentStop == 1)
			{
				if (passenger->getSector()
					== building.getSector(created.shuttle.sector->getIndex()).get())
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
						/ building.getFixedTimestep());
					if (shuttle->liftServiceStartedTick <= *lastOccupiedAtDestination
						|| shuttle->liftBoardingCutoffTick
							!= shuttle->liftServiceStartedTick + boardingTicks) return false;
					sawBoardingWindowAfterDisembark = true;
				}
			}
			if (passenger->getSector() == building.getSector(created.shuttle.sector->getIndex()).get()
				&& !shuttle->liftMoving && shuttle->liftCurrentStop == 1)
			{
				auto x = passenger->getGlobalPosition().x;
				if (previousStoppedX)
				{
					if (x < *previousStoppedX - 0.001f) return false;
					sawForwardAlignment = sawForwardAlignment || x > *previousStoppedX + 0.001f;
				}
				previousStoppedX = x;
			}
			if (sawBoardingWindowAfterDisembark && passenger->getState() == core::Agent::State::Idle
				&& passenger->getSector() == building.getSector(right).get()) break;
		}
		return sawForwardAlignment && sawAllDestinationDoorsOpen && sawBoardingWindowAfterDisembark
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == building.getSector(right).get();
	}

	bool multiCarriageShuttleCoordinatesIndependentCarriagesAndAccessZones()
	{
		core::Building building("Coupled shuttle", 20, 2);
		auto leftA = building.addRoom("Left A", CORE_LAYER_FORE, 0, 0, 3, 1);
		auto leftB = building.addRoom("Left B", CORE_LAYER_FORE, 0, 4, 3, 1);
		auto rightA = building.addRoom("Right A", CORE_LAYER_FORE, 0, 12, 3, 1);
		auto rightB = building.addRoom("Right B", CORE_LAYER_FORE, 0, 16, 3, 1);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 12 }, 0 };
		options.capacity = 1;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 2.0f;
		auto created = building.addShuttle(0, 0, 19, options);
		building.finishBuild();
		if (!created.traversalResource || created.doors.size() != 4) return false;

		struct Journey { core::AgentId agent; uint32_t targetSector; float targetX; };
		std::vector<Journey> journeys = {
			{ building.createAgent("A first", leftA, 0, 1.35f), rightA, 13.5f },
			{ building.createAgent("B", leftB, 0, 1.5f), rightB, 17.5f },
			{ building.createAgent("A overflow", leftA, 0, 1.65f), rightA, 13.5f }
		};
		for (auto const& journey : journeys)
		{
			auto agent = building.lookupAgent(journey.agent).entity;
			auto target = building.getGraph()->getClosestVertexInSector(
				building.getSector(journey.targetSector).get(), { journey.targetX, 0.0f });
			if (!target) return false;
			auto path = building.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(std::move(path), true);
		}

		bool sawIndependentFullCarriages = false;
		std::array<bool, 2> sawCarriageOccupied{};
		bool sawSeparatedAccessZones = false;
		bool sawBoundAssignment = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 24; ++tick)
		{
			building.advanceTick();
			auto snapshot = building.getSimulationSnapshot();
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
				{ auto agent = building.lookupAgent(journey.agent).entity;
					return agent->getState() == core::Agent::State::Idle
						&& agent->getSector() == building.getSector(journey.targetSector).get(); })) break;
		}
		auto complete = std::all_of(journeys.begin(), journeys.end(), [&](auto const& journey)
			{ auto agent = building.lookupAgent(journey.agent).entity;
				return agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == building.getSector(journey.targetSector).get(); });
		return (sawIndependentFullCarriages || std::all_of(sawCarriageOccupied.begin(), sawCarriageOccupied.end(),
			[](bool occupied) { return occupied; }))
			&& sawSeparatedAccessZones && sawBoundAssignment && complete;
	}

	bool liftFailuresCancellationAndDisableDrainSafely()
	{
		// Repeated selector failures keep the landing open and eventually return the
		// passenger to the current stop without leaking lift ownership.
		{
			core::Building building("Failed lift selector", 6, 4);
			auto lower = building.addCorridor(0, 0, 5);
			auto upper = building.addCorridor(2, 0, 5);
			core::Building::CreateLiftOptions options;
			options.stopOffsets = { 0, 2 };
			auto created = building.addLift(0, 2, options);
			building.finishBuild();
			auto target = building.getGraph()->getClosestVertexInSector(
				building.getSector(upper).get(), { 2.5f, 2.0f });
			auto id = building.createAgent("Failed selector passenger", lower, 0, 2.5f);
			auto agent = building.lookupAgent(id).entity;
			agent->setPath(building.getGraph()->calculatePath(agent, target), true);
			std::set<core::DeviceOperationId> failed;
			bool stayedOpen = true;
			for (uint32_t tick = 0; tick < MaximumSimulationTicks * 5; ++tick)
			{
				building.advanceTick();
				auto snapshot = building.getSimulationSnapshot();
				for (auto const& operation : snapshot.deviceOperations)
				{
					if (operation.command.type != core::DeviceCommandType::SelectLiftDestination
						|| failed.contains(operation.id)
						|| (operation.state != core::DeviceOperationState::Pending
							&& operation.state != core::DeviceOperationState::Running)) continue;
					building.lookupDeviceOperation(operation.id).entity->setState(core::DeviceOperationState::Failed);
					failed.insert(operation.id);
				}
				auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
					[&](auto const& resource) { return resource.id == created.traversalResource; });
				if (lift != snapshot.traversalResources.end() && lift->occupantCount > 0
					&& lift->liftStopPhase == core::LiftStopPhase::Closing) stayedOpen = false;
				if (failed.size() == 3 && agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == building.getSector(lower).get()) break;
			}
			auto final = building.getSimulationSnapshot();
			auto lift = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (failed.size() != 3 || !stayedOpen || lift == final.traversalResources.end()
				|| lift->occupantCount != 0 || lift->admissionReservationCount != 0
				|| !lift->admissionQueue.empty() || lift->liftPendingSafeExits != 0) return false;
		}

		// Cancellation while moving and subsequent disable both preserve occupancy
		// until alignment, reject fresh demand, and unload through a landing permit.
		{
			core::Building building("Disabled moving lift", 6, 4);
			auto lower = building.addCorridor(0, 0, 5);
			auto upper = building.addCorridor(2, 0, 5);
			core::Building::CreateLiftOptions options;
			options.stopOffsets = { 0, 2 };
			auto created = building.addLift(0, 2, options);
			building.finishBuild();
			auto target = building.getGraph()->getClosestVertexInSector(
				building.getSector(upper).get(), { 2.5f, 2.0f });
			auto id = building.createAgent("Cancelled onboard passenger", lower, 0, 2.5f);
			auto agent = building.lookupAgent(id).entity;
			agent->setPath(building.getGraph()->calculatePath(agent, target), true);
			bool cancelledMoving = false;
			for (uint32_t tick = 0; tick < MaximumSimulationTicks * 4; ++tick)
			{
				building.advanceTick();
				auto snapshot = building.getSimulationSnapshot();
				auto lift = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
					[&](auto const& resource) { return resource.id == created.traversalResource; });
				if (!cancelledMoving && lift != snapshot.traversalResources.end() && lift->liftMoving)
				{
					agent->clearPath();
					cancelledMoving = building.setTraversalResourceEnabled(created.traversalResource, false);
				}
				if (cancelledMoving && agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == building.getSector(upper).get()) break;
			}
			building.advanceTick(); // publish the terminal unavailable state after unload commit
			auto final = building.getSimulationSnapshot();
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
		core::Building building("Editor lift authoring", 10, 8);
		building.addCorridor(1, 0, 8);
		building.addCorridor(4, 0, 8);
		auto created = building.addLift(0, 2, 2, 6);
		building.finishBuild();
		auto lift = std::dynamic_pointer_cast<const core::LiftTransit>(created.lift.sector);
		if (!lift || lift->getCellsWide() != 2 || lift->getDecksHigh() != 6
			|| lift->getNumStops() != 2 || created.doors.size() != 2) return false;
		for (auto const& door : created.doors)
			if (!building.isLiftOwnedDoor(door.door.sector->getObject(door.door.index))) return false;

		building.pauseSimulation();
		building.addCorridor(3, 0, 8);
		building.finishBuild();
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(
			building.getSectorAtPosition(CORE_LAYER_BACK, 2.0f, 0.0f));
		if (!lift || lift->getNumStops() != 2) return false; // Corridors do not create stops.
		uint32_t landingX = 0, landingWidth = 0;
		if (!building.getLiftLandingGeometry(3, 3, landingX, landingWidth)
			|| landingX != 2 || landingWidth != 2) return false;
		auto added = building.addSectorDoor(3, 3);
		if (!building.isLiftOwnedDoor(added.door.sector->getObject(added.door.index))) return false;
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(
			building.getSectorAtPosition(CORE_LAYER_BACK, 2.0f, 0.0f));
		if (!lift || lift->getNumStops() != 3) return false;

		auto move = building.planResizeLift(lift->getIndex(), 5, 0, 2, 6);
		if (!move.valid || !move.move) return false;
		auto movedIndex = building.applyLiftEdit(move);
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(building.getSector(movedIndex));
		if (!lift || lift->getCellX() != 5 || lift->getDecksHigh() != 6
			|| lift->getNumStops() != 3) return false;
		for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
		{
			auto floor = (uint32_t)((int)lift->getStop(stop).sector->getCellY()
				+ lift->getStop(stop).sectorOffsetY);
			auto const& cell = static_cast<core::Building const&>(building)
				.getLayer(CORE_LAYER_FORE)->getCellDefinition(5, floor);
			auto door = building.getSector(cell.sectorIndex)->getObject(cell.sectorObjectIndex);
			if (!building.isLiftOwnedDoor(door)) return false;
		}
		auto removeStop = building.planRemoveLiftStop(lift->getIndex(), 1);
		if (!removeStop.valid || !removeStop.requiresConfirmation()) return false;
		auto afterStopRemoval = building.applyLiftEdit(removeStop);
		lift = std::dynamic_pointer_cast<const core::LiftTransit>(building.getSector(afterStopRemoval));
		if (!lift || lift->getNumStops() != 2) return false;
		auto remove = building.planRemoveLift(lift->getIndex());
		if (!remove.valid) return false;
		building.applyLiftEdit(remove);
		return !building.getSectorAtPosition(CORE_LAYER_BACK, 5.0f, 0.0f);
	}

	bool editorShuttleAuthoringReconcilesOwnedLandings()
	{
		core::Building building("Editor shuttle authoring", 32, 3);
		building.addCorridor(0, 0, 31);
		building.addCorridor(1, 0, 31);
		core::Building::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.doorMask = 0b101;
		auto candidates = building.getValidShuttleStopOffsets(0, 0, 27, 2, 3, false, 2);
		if (find(candidates.begin(), candidates.end(), 0) == candidates.end()
			|| find(candidates.begin(), candidates.end(), 18) == candidates.end()) return false;
		auto created = building.addShuttle(0, 0, 27, options);
		building.finishBuild();
		auto shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(created.shuttle.sector);
		if (!shuttle || shuttle->getNumStops() != 2 || created.doors.size() != 8) return false;
		for (auto const& door : created.doors)
		{
			uint32_t owner, stop, carriage;
			if (!building.isShuttleOwnedDoor(door.door.sector->getObject(door.door.index),
				&owner, &stop, &carriage) || owner != shuttle->getIndex()
				|| stop >= 2 || carriage >= 2) return false;
		}
		auto doorCandidates = building.getShuttleStopCandidatesForDoor(0, 9);
		if (none_of(doorCandidates.begin(), doorCandidates.end(), [&](auto const& candidate)
			{ return candidate.sectorIndex == shuttle->getIndex() && candidate.stopOffset == 9; })) return false;

		building.pauseSimulation();
		auto move = building.planResizeShuttle(shuttle->getIndex(), 1, 1, 27);
		if (!move.valid || !move.move || move.stopOffsets != std::vector<uint32_t>({ 0, 18 })) return false;
		auto movedIndex = building.applyShuttleEdit(move);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(building.getSector(movedIndex));
		if (!shuttle || shuttle->getCellX() != 1 || shuttle->getCellY() != 1) return false;

		auto resize = building.planResizeShuttle(movedIndex, 1, 1, 26);
		if (!resize.valid || resize.move || resize.stopOffsets != std::vector<uint32_t>({ 0, 18 })) return false;
		movedIndex = building.applyShuttleEdit(resize);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(building.getSector(movedIndex));
		if (!shuttle || shuttle->getCellsWide() != 26) return false;

		auto add = building.planAddShuttleStop(movedIndex, 9);
		if (!add.valid || !add.requiresConfirmation()) return false;
		movedIndex = building.applyShuttleEdit(add);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(building.getSector(movedIndex));
		if (!shuttle || shuttle->getNumStops() != 3) return false;
		auto removeStop = building.planRemoveShuttleStop(movedIndex, 1);
		if (!removeStop.valid || !removeStop.requiresConfirmation()) return false;
		movedIndex = building.applyShuttleEdit(removeStop);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(building.getSector(movedIndex));
		if (!shuttle || shuttle->getNumStops() != 2) return false;
		building.addSectorWindow(CORE_LAYER_FORE, 1, 10, 1, 1);
		auto remove = building.planRemoveShuttle(movedIndex);
		if (!remove.valid) return false;
		building.applyShuttleEdit(remove);
		if (building.getSectorAtPosition(CORE_LAYER_BACK, 1.0f, 1.0f)) return false;

		core::Building manyDoors("Schematic Shuttle doors", 24, 2);
		manyDoors.addCorridor(0, 0, 23);
		core::Building::CreateShuttleOptions manyDoorOptions{ 1, 4, { 0, 10 }, 0 };
		manyDoorOptions.doorMask = 0b1111;
		auto manyDoorResult = manyDoors.addShuttle(0, 0, 20, manyDoorOptions);
		manyDoors.finishBuild();
		if (manyDoorResult.doors.size() != 8
			|| !manyDoors.getValidShuttleStopOffsets(0, 0, 20, 1, 4, false, 1u << 4).empty()) return false;
		for (uint32_t door = 0; door < 4; ++door)
			if (manyDoorResult.doors[door].door.sector->getObject(
				manyDoorResult.doors[door].door.index)->getCellX() != door) return false;

		core::Building partial("Partial Shuttle authoring", 24, 2);
		partial.addCorridor(0, 0, 4);
		partial.addCorridor(0, 10, 4);
		core::Building::CreateShuttleOptions partialOptions{ 2, 3, { 0, 10 }, 0 };
		partialOptions.allowPartialLandings = true;
		partialOptions.doorMask = 0b101;
		auto partialCreated = partial.addShuttle(0, 0, 20, partialOptions);
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
		core::Building building("Unavailable door", 6, 2);
		auto fore = building.addRoom("Fore", CORE_LAYER_FORE, 0, 0, 5, 1);
		building.addRoom("Back", CORE_LAYER_BACK, 0, 0, 5, 1);
		core::Building::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Unavailable;
		building.addSectorDoor(0, 2, options);
		building.finishBuild();
		auto edge = *std::find_if(building.getGraph()->getEdges().begin(), building.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto agentId = building.createAgent("Rejected traveller", fore, 0, 0.5f);
		auto agent = building.lookupAgent(agentId).entity;
		// This is the same two-step path assignment used by the UI for a
		// player-directed agent: preview the route, then explicitly start it.
		agent->setPath(twoNodePath(source, destination, edge), false);
		building.advanceTick();
		if (agent->getState() != core::Agent::State::Idle
			|| !building.getSimulationSnapshot().traversalRequests.empty()) return false;
		agent->startPathing();
		for (uint32_t i = 0; i < MaximumSimulationTicks
			&& building.getSimulationSnapshot().traversalRequests.empty(); ++i)
		{
			building.advanceTick();
		}
		auto snapshot = building.getSimulationSnapshot();
		return agent->getSector() == building.getSector(fore).get()
			&& agent->getState() == core::Agent::State::WaitingForTraversal
			&& snapshot.traversalRequests.size() == 1
			&& snapshot.traversalRequests.front().state == core::TraversalRequestState::Denied
			&& snapshot.deviceOperations.empty() && snapshot.traversalPermits.empty();
	}

	bool interactionBindingAggregationIsMeaningful()
	{
		core::Building building("Binding aggregation", 3, 2);
		auto corridorIndex = building.addCorridor(0, 0, 2);
		building.finishBuild();
		auto sectorId = core::SectorId{ (uint64_t)corridorIndex + 1 };
		auto actor = building.createAgent("Binding operator", corridorIndex, 0, 0.5f);

		core::InteractionBinding required{ { core::DeviceCommandType::SetSectorLights, sectorId, false },
			core::InteractionBindingRequirement::Required };
		core::InteractionBinding bestEffort{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::BestEffort };
		auto point = building.createInteractionPoint("Multi-binding control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { required, bestEffort });
		auto requestId = building.requestInteraction(point, actor);
		auto request = building.lookupInteractionRequest(requestId);
		if (!request || request.entity->getOperations().size() != 2)
		{
			return false;
		}
		auto failedBestEffort = request.entity->getOperations()[1].first;
		building.lookupDeviceOperation(failedBestEffort).entity->setState(core::DeviceOperationState::Failed);
		building.advanceTicks(4);
		request = building.lookupInteractionRequest(requestId);
		if (!request || request.entity->getResult() != core::InteractionResult::SucceededWithBestEffortFailure)
		{
			return false;
		}

		core::InteractionBinding failingRequired{ { core::DeviceCommandType::SetSectorLights, sectorId, true },
			core::InteractionBindingRequirement::Required };
		auto requiredPoint = building.createInteractionPoint("Required control", sectorId,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { failingRequired });
		auto failedRequestId = building.requestInteraction(requiredPoint, actor);
		auto failedRequest = building.lookupInteractionRequest(failedRequestId);
		if (!failedRequest)
		{
			return false;
		}
		building.lookupDeviceOperation(failedRequest.entity->getOperations().front().first).entity->setState(
			core::DeviceOperationState::Failed);
		building.advanceTick();
		return building.lookupInteractionRequest(failedRequestId).entity->getResult() == core::InteractionResult::Failed;
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
		core::Building building("Scale world", 80, 2);
		auto corridor = building.addCorridor(0, 0, 79);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(corridor, 0, 70.5f, &destinationVertexId);
		building.finishBuild();
		for (uint32_t i = 0; i < ResourceCount; ++i)
		{
			building.createTraversalResource("Scale resource " + std::to_string(i));
		}

		auto source = building.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = building.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto edge = std::make_shared<core::SectorEdge>();
		for (uint32_t i = 0; i < agentCount; ++i)
		{
			auto id = building.createAgent("Scale agent " + std::to_string(i), corridor, 0, 0.5f);
			building.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
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
		digestEvents(building.consumeSimulationEvents());
		auto started = std::chrono::steady_clock::now();
		for (uint64_t tick = 0; tick < ticks; ++tick)
		{
			building.advanceTick();
			// Event delivery is intentionally incremental in a long-running host.
			if ((tick + 1) % 10 == 0) digestEvents(building.consumeSimulationEvents());
		}
		digestEvents(building.consumeSimulationEvents());
		result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		result.workingSetBytes = currentWorkingSetBytes();

		auto snapshot = building.getSimulationSnapshot();
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

	ScenarioResult runOrdinaryPathScenario()
	{
		core::Building building("Headless smoke building", 7, 2);
		auto corridor = building.addCorridor(0, 0, 6);

		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		building.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		building.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		building.finishBuild();

		auto agentId = building.createAgent("Headless smoke agent", corridor, 0, 0.5f);
		auto agentLookup = building.lookupAgent(agentId);
		if (!agentLookup)
		{
			return {};
		}
		auto agent = agentLookup.entity;

		auto graph = building.getGraph();
		auto source = graph->getVertexByIdentifier(sourceVertexId);
		auto destination = graph->getVertexByIdentifier(destinationVertexId);
		auto path = graph->calculatePath(agent, source, destination);
		if (!path)
		{
			return {};
		}
		agent->setPath(path, true);

		while (agent->getState() != core::Agent::State::Idle
			&& building.getSimulationTick() < MaximumSimulationTicks)
		{
			building.advanceTick();
		}

		ScenarioResult result;
		result.snapshot = building.getSimulationSnapshot();
		result.events = building.consumeSimulationEvents();
		auto finalPosition = agent->getGlobalPosition();
		result.reachedDestination = agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == building.getSector(corridor).get()
			&& finalPosition.distanceTo(destination->getPosition()) < 0.001f
			&& phasesAreOrdered(result.events, result.snapshot.tick);
		return result;
	}
}

int main()
{
	try
	{
		runSerializationSmokeChecks();

		if (!accumulatedRenderTimeAdvancesWholeTicksOnly())
		{
			std::cerr << "FAIL: render-time accumulation did not advance exactly one whole tick\n";
			return 1;
		}
		if (!buildingOwnsTypedEntitiesAndInvalidatesHandles())
		{
			std::cerr << "FAIL: typed building ownership or handle invalidation failed\n";
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
			std::cerr << "FAIL: Door placement did not enforce corridor/Room or obstruction rules\n";
			return 1;
		}
		if (!objectMoveValidatesAndRebuildsOnceCommitted())
		{
			std::cerr << "FAIL: Object movement did not validate and rebuild atomically\n";
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
		if (!waitingLiftPassengersFillArrivingCar())
		{
			std::cerr << "FAIL: waiting passengers did not fill an arriving lift with available capacity\n";
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
		if (!extensibleForceBridgeCompletesThroughPhysicalControl())
		{
			std::cerr << "FAIL: extensible force bridge preparation or lease cleanup failed\n";
			return 1;
		}
		if (!staircaseCoordinationIsExplicitlyOptIn())
		{
			std::cerr << "FAIL: ordinary/narrow staircase coordination policy was incorrect\n";
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
