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
	struct Path;

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

		// ------------------------------------------------------------------
		// Lift scheduling and passenger safe exits (ADR 0004 stage 3)
		//
		// The lift scheduling queries which drive car dispatch, and the passenger
		// safe-exit protocol which gets an Agent out of a car it can no longer
		// ride, all live here. Building forwards each of these entry points; no
		// caller outside Building names the coordinator.
		// ------------------------------------------------------------------

		// Stop lookup. A stop is whichever declared stop sits nearest the endpoint
		// along the axis the resource travels - horizontal for a shuttle, vertical
		// for a lift.
		uint32_t findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const;

		// The stop an Agent wants next, read from the first ride edge of the
		// journey ahead of it on its current path.
		uint32_t findAgentLiftDestination(Agent const& agent,
			TraversalResource const& resource) const;

		// Whether any occupant currently riding to `stop` needs to leave there.
		bool liftHasDisembarkDemand(TraversalResource const& resource, uint32_t stop) const;

		// Stop requests. Each request is attributed to its owning Agent so the
		// car can drop a demand that dies with the Agent who made it, and records
		// the tick of the oldest interest for deterministic dispatch.
		void addLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner);

		void removeLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner);

		// Choosing where the car goes next: keep the current direction while
		// compatible demand lies ahead, reverse only when it does not, and break
		// idle ties by oldest interest, then distance, then stop id.
		uint32_t chooseNextLiftStop(TraversalResource& resource) const;

		// Whether a boarding request can be served without turning the car around
		// while it still has demand ahead of it in the current direction.
		bool isLiftBoardingDirectionCompatible(TraversalResource& resource,
			uint32_t originStop, uint32_t destinationStop);

		// Lift admission release. A request which will never board surrenders its
		// admission-queue and confirmation-queue place, its reservations, and -
		// for an open platform lift - its physical queue position as well.
		void releaseLiftAdmission(TraversalRequestId requestId, TraversalResource& resource);

		// Passenger safe exits. An Agent which cannot complete its journey while
		// riding is asked to leave at the next stop the car can reach safely, and
		// remembers why, so the reason survives until the exit is resolved.
		void requestLiftPassengerSafeExit(AgentId passenger, TraversalFailureReason reason);

		// Once the car is stopped and its doors are open, each pending passenger is
		// given its landing path; a passenger which has already gone gives up its
		// manifest slot instead of leaving it standing (#57).
		void assignLiftSafeExitPaths(TraversalResource& resource);

		// Re-riding with a new onboard destination: the Agent keeps its manifest
		// slot, the stale destination and its obsolete selections are dropped, and
		// the caller is told which path node the new ride starts from.
		bool replaceOnboardLiftDestination(Agent& agent, std::shared_ptr<Path> const& path,
			uint32_t& sourceNode);

		// ------------------------------------------------------------------
		// Lift onboard destination selection - the riding branch (ADR 0004 stage 3)
		//
		// What happens when an Agent which already occupies the car asks to travel
		// to another stop inside it: scheduled-destination grants (including the
		// shuttle contiguous-ride alignment), journey-stop resolution, the
		// shared-destination shortcut, confirmation-queue serialisation at the
		// interior selector control, and the retry-then-safe-exit policy all live
		// here. Building's lift allocation dispatcher forwards riding requests
		// through this entry point; no caller outside Building names the
		// coordinator.
		// ------------------------------------------------------------------

		// Allocate one pending riding request. `coordinator` is the lift or
		// shuttle journey resource the request was made against; the request's
		// own landing edge is not consulted, since the passenger is already aboard.
		void allocateLiftRiding(TraversalRequestId requestId, TraversalResource& coordinator);

		// ------------------------------------------------------------------
		// Open platform lift traversal allocation (ADR 0004 stage 3)
		//
		// Allocating an Agent's use of an open platform lift all lives here:
		// calling the platform at its landing control, boarding from the
		// reserved queue position before the boarding cutoff, selecting an
		// onboard destination, and disembarking through the platform's virtual
		// crossing boundary. It sits with the lift admission release above,
		// which undoes this allocation. Building forwards this entry point; no
		// caller outside Building names the coordinator.
		// ------------------------------------------------------------------

		// Allocate one pending request against the platform lift resource it was
		// made on. Co-located legacy mount edges are not admission points and
		// grant immediately; only the journey edge drives the platform.
		void allocateOpenPlatformLiftTraversal(TraversalRequestId requestId,
			TraversalResource& resource);

		// ------------------------------------------------------------------
		// Shuttle door assignment (ADR 0004 stage 3)
		//
		// Which carriage door a shuttle passenger boards and disembarks through,
		// and the retargeting of the request and the Agent's traversal task onto
		// that door's landing resource, all live here. Building forwards the
		// boarding and disembark entry points its lift allocation branches still
		// call; no caller outside Building names the coordinator.
		// ------------------------------------------------------------------

		// The cells a carriage's door mask opens onto, in carriage order. This is
		// the indexing the shuttle's doors, carriages and capacity positions are
		// built from, so it travels with the assignment family; Building's shuttle
		// authoring path calls it through here.
		static std::vector<uint32_t> shuttleDoorOffsets(uint32_t carriageWidth, uint32_t doorMask);

		// Which carriage of a shuttle an occupying passenger rides in; ~0u when
		// the resource is not a shuttle or the passenger holds no capacity slot.
		uint32_t findShuttlePassengerCarriage(TraversalResource const& resource,
			AgentId passenger) const;

		// Pick and apply the door a waiting passenger boards at `stop`: nearest to
		// the passenger, with capacity left, and on a carriage which also owns a
		// door into the sector the journey leaves the shuttle through.
		bool assignShuttleBoardingDoor(TraversalRequestId requestId,
			TraversalResource& coordinator, uint32_t stop);

		// Pick and apply the door an occupant leaves by at `stop`, keeping the
		// door the remaining path already selected whenever it serves the
		// passenger's assigned carriage.
		bool assignShuttleDisembarkDoor(TraversalRequestId requestId,
			TraversalResource& coordinator, uint32_t stop);

	private:

		// Move a request - and the Agent's traversal task with it - onto the
		// selected landing Door, surrendering any queue ownership the request held
		// on the door it was pointed at before.
		bool retargetShuttleDoorTraversal(TraversalRequestId requestId,
			TraversalResource& coordinator, ShuttleDoor const& door);

		// Releases a cancelling Actor's claim on each operation its request
		// needed; an operation nobody still wants is cancelled with it.
		void detachInteractionRequester(InteractionRequest& request);

		// The coordinator owns no state. It reaches the registries it drives
		// through the Building that owns it.
		Building& mBuilding;
	};

} // core
