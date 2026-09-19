#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/Coordination.h"
#include "core/EntityId.h"
#include "core/Vector2.h"


namespace core
{
	class Building;
	class Agent;

	// Runs the simulation on behalf of the Building that owns it.
	//
	// Building keeps the world structure and owns every entity registry
	// (ADR 0001), and remains the facade (design pattern, not the Facade sector
	// type of ADR 0003) through which every caller - Agent included - reaches
	// simulation behaviour. The coordinator is where that behaviour lives:
	// traversal request and permit lifecycle, queue tickets and queue positions,
	// door/lift/shuttle/platform-lift/extensible allocation, interactions and
	// device operations, agent lifecycle, and the tick pipeline.
	//
	// ADR 0004 moves that behaviour in staged, leaf-first increments so each
	// stage builds green. The coordinator owns no state: it works on Building's
	// registries through the Building it was given, and calls back through the
	// Building facade for machinery which has not moved out of Building yet.
	//
	// The name deliberately avoids "controller", which CONTEXT.md bans, and
	// echoes ADR 0001's traversal-coordination vocabulary while covering the
	// wider simulation scope.
	class SimulationCoordinator
	{
	public:

		explicit SimulationCoordinator(Building& building);

		SimulationCoordinator(SimulationCoordinator const&) = delete;
		SimulationCoordinator& operator=(SimulationCoordinator const&) = delete;

		virtual ~SimulationCoordinator() = default;

		// ------------------------------------------------------------------
		// Agent lifecycle (ADR 0004 stage 1)
		//
		// Creating an Agent, placing it in a Sector, removing it, waking every
		// Agent, resolving Agents between handles and pointers, and releasing an
		// Agent's traversal ownership all live here. Building forwards each of
		// these entry points; no caller outside Building names the coordinator.
		// ------------------------------------------------------------------

		// Creates an Agent named `name` and places it in the Sector `sectorId`,
		// either at a specific deck and lateral offset or at the Sector default.
		AgentId createAgent(std::string const& name, uint32_t sectorId, uint32_t deckOffset, float xOffset);

		AgentId createAgent(std::string const& name, uint32_t sectorId);

		// Takes an already-constructed Agent into the Building's ownership and
		// places it in the Sector `sectorId`. The Agent is attached to the
		// Building, never to the coordinator: the Building remains the owner the
		// Agent reports to.
		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId, uint32_t deckOffset, float xOffset);

		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId);

		// Handle resolution. A lookup reports the Agent behind a handle plus a
		// diagnostic when the handle is dead; `getAgentId` is its inverse and
		// yields an empty handle for an Agent this Building does not own.
		EntityLookup<Agent> lookupAgent(AgentId id);

		EntityLookup<Agent const> lookupAgent(AgentId id) const;

		AgentId getAgentId(Agent const* agent) const;

		// Removes the Agent behind `id`, releasing it from every traversal
		// resource, interaction request, and device operation that names it
		// before the entity itself goes. A refusal leaves the Agent untouched.
		EntityRemovalResult removeAgent(AgentId id);

		// Wakes every Agent the Building owns.
		void wakeAllAgents();

		// Traversal-ownership release. An Agent's claims on a traversal
		// resource - a manifest slot, a stop request, an occupant lease -
		// outlive nothing, so they are surrendered before the Agent is destroyed
		// (ticket #57).
		bool holdsTraversalOwnership(AgentId id) const;

		void releaseAgentFromResource(TraversalResource& resource, AgentId id);

		void releaseTraversalOwnership(AgentId id);

		// ------------------------------------------------------------------
		// Interactions and device operations (ADR 0004 stage 2)
		//
		// The InteractionPoint and InteractionRequest lifecycles, the
		// DeviceOperation lifecycle, and the per-tick phases which allocate,
		// move and resolve interactions all live here. Building forwards each of
		// these entry points; no caller outside Building names the coordinator.
		// ------------------------------------------------------------------

		// Interaction point lifecycle. A point is created bare or fully bound to
		// device commands; removing one cancels its pending requests first, and
		// refuses to be silent when a traversal resource still names it as a
		// control, which is a structural change.
		InteractionPointId createInteractionPoint(std::string const& name);

		InteractionPointId createInteractionPoint(std::string const& name, SectorId sector,
			Vector2 position, float reach, float durationSeconds,
			std::vector<InteractionBinding> bindings);

		EntityLookup<InteractionPoint> lookupInteractionPoint(InteractionPointId id);

		EntityLookup<InteractionPoint const> lookupInteractionPoint(InteractionPointId id) const;

		EntityRemovalResult removeInteractionPoint(InteractionPointId id);

		// Requesting an interaction. A traversal request queues behind whatever
		// the point is already doing; a request made while passing does not wait -
		// it activates its operations and presses the control immediately.
		InteractionRequestId requestInteraction(InteractionPointId point, AgentId actor);

		InteractionRequestId requestInteractionForTraversal(InteractionPointId point, AgentId actor);

		InteractionRequestId requestInteractionWhilePassing(InteractionPointId point, AgentId actor);

		EntityLookup<InteractionRequest const> lookupInteractionRequest(InteractionRequestId id) const;

		bool cancelInteraction(InteractionRequestId id);

		// Device-operation lifecycle. Commands coalesce: one accepted command is
		// shared by every requester, and the operation outlives no requester.
		DeviceOperationId createDeviceOperation(std::string const& name, AgentId requester);

		DeviceOperationId findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester);

		EntityLookup<DeviceOperation> lookupDeviceOperation(DeviceOperationId id);

		EntityLookup<DeviceOperation const> lookupDeviceOperation(DeviceOperationId id) const;

		bool cancelDeviceOperation(DeviceOperationId id, AgentId requester);

		EntityRemovalResult removeDeviceOperation(DeviceOperationId id);

		// Per-tick interaction phases. Device operations advance first, then the
		// point queues allocate, the actor walks to and presses the control, and
		// results are resolved from the operations each request depends on.
		void advanceDeviceOperations();

		void allocateInteractions();

		void moveInteractions(float frameTime);

		void updateInteractionResults();

		// Pressing a physical control. The press is what the rendered Button
		// reports, not a separate simulation of the device state.
		void pressPhysicalControl(InteractionPointId point);

		// An Agent walking towards a RemoteControlled Door presses its button
		// early - while still moving, and only once per door - so the door is open
		// by the time the threshold arrives.
		void tryPressUpcomingDoorButton(Agent& agent, Vector2 const& movementStart,
			Vector2 const& movementEnd);

		// ------------------------------------------------------------------
		// Door and extensible traversal preparation (ADR 0004 stage 3)
		//
		// Remote-door preparation, extensible preparation for force bridges and
		// extensible ladders, and the door open lease protocol all live here.
		// Building forwards each of these entry points; no caller outside
		// Building names the coordinator.
		// ------------------------------------------------------------------

		// A RemoteControlled Door cannot be crossed until some Agent has reached
		// and pressed a control applicable from the requester's sector. One
		// preparation is shared by every pending request for the door, and a
		// failed preparation retries on a fixed tick delay before it is refused.
		void allocateRemoteDoorPreparation(TraversalRequestId requestId, TraversalResource& resource);

		// A force bridge or extensible ladder must be extended before its
		// threshold may be crossed. The extension is requested through a control
		// applicable from the requester's sector; once extended, the request is
		// handed to the ladder admission queue or the door crossing queue.
		void allocateExtensiblePreparation(TraversalRequestId requestId, TraversalResource& resource);

		// Door open leases. Every live lease holds the door open; a lease taken
		// while the door is closing re-opens it, except a remote-controlled
		// preparation lease, which must reach its physical control first.
		DoorOpenLeaseId acquireDoorOpenLease(TraversalResource& resource,
			DoorOpenLeaseKind kind, TraversalRequestId request = {});

		bool releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease);

		// The resource-handle form is the entry point external systems use to hold
		// a door open through the same scoped safety protocol.
		DoorOpenLeaseId acquireDoorOpenLease(TraversalResourceId resource,
			DoorOpenLeaseKind kind = DoorOpenLeaseKind::ExternalHoldOpen);

		bool releaseDoorOpenLease(TraversalResourceId resource, DoorOpenLeaseId lease);

	private:

		// Releases a cancelling Actor's claim on each operation its request
		// needed; an operation nobody still wants is cancelled with it.
		void detachInteractionRequester(InteractionRequest& request);

		// The coordinator owns no state. It reaches the registries it drives
		// through the Building that owns it.
		Building& mBuilding;
	};

} // core
