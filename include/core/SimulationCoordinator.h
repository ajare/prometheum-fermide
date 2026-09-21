#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/Coordination.h"
#include "core/EntityId.h"
#include "core/Simulation.h"
#include "core/Vector2.h"


namespace core
{
	class Building;
	class Agent;
	class Edge;
	class Vertex;
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

		// Wakes every activated Agent the Building owns. A deactivated Agent is
		// not simulated (#118), so waking must not restart its locomotion.
		void wakeAllAgents();

		// Activation is judged before it is written (#118). The target state is
		// always a legal value; the only refusals are an Agent the Building does
		// not own and a running simulation, since activating or deactivating an
		// Agent mid-run would strand whatever traversal it was in the middle of.
		bool canSetAgentActive(AgentId id, bool active, std::string* diagnostic = nullptr) const;

		// Returns false and changes nothing when canSetAgentActive refuses,
		// reporting the reason through `diagnostic`.
		bool setAgentActive(AgentId id, bool active, std::string* diagnostic = nullptr);

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
		// Lift traversal allocation dispatcher (ADR 0004 stage 3)
		//
		// Resolving the journey resource and the stop a request is made against,
		// the enabled check, the boarding / riding / disembarking classification
		// and the dispatch to the three branches above all live here, together
		// with the open platform lift dispatch which bypasses the journey
		// resource entirely. Building's traversal-request allocation forwards
		// lift and shuttle requests through this entry point; no caller outside
		// Building names the coordinator.
		// ------------------------------------------------------------------

		// Allocate one pending request made against a lift, shuttle or landing
		// resource. `edgeResource` is the resource the request was made on: the
		// journey itself, or the landing door whose coordinator link is followed.
		void allocateLiftTraversal(TraversalRequestId requestId, TraversalResource& edgeResource);

		// ------------------------------------------------------------------
		// Lift boarding branch (ADR 0004 stage 3)
		//
		// What happens when an Agent outside the car asks to enter it: the
		// landing-door queue ticket and the trip intent it registers, the landing
		// call preparation and its completion, the boarding eligibility gates, the
		// FIFO admission-queue selection, the shuttle carriage capacity
		// assignment, the queue-position arrival check, the door open lease and the
		// crossing grant all live here. Building's lift allocation dispatcher
		// forwards boarding requests through this entry point; no caller outside
		// Building names the coordinator.
		// ------------------------------------------------------------------

		// Allocate one pending boarding request. `edgeResource` is the landing the
		// request was made on and the door the passenger crosses to enter the car -
		// for a shuttle, the boarding-door assignment may retarget it to another
		// door of the same access zone without changing the ticket's priority.
		// `coordinator` is the lift or shuttle journey resource which owns the
		// stop, and `stop` the stop index the dispatcher resolved for the request.
		void allocateLiftBoarding(TraversalRequestId requestId, TraversalResource& edgeResource,
			TraversalResource& coordinator, uint32_t stop);

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
		// Lift disembarking branch (ADR 0004 stage 3)
		//
		// What happens when an occupant asks to leave the car at the stop it is
		// standing at: the shuttle disembark-door assignment and the walk within
		// the carriage to the shuttle-side node, the disembark stop phase, the
		// door open lease which holds the landing door open, and the crossing
		// lane grant all live here. Building's lift allocation dispatcher
		// forwards disembarking requests through this entry point; no caller
		// outside Building names the coordinator.
		// ------------------------------------------------------------------

		// Allocate one pending disembarking request. `edgeResource` is the
		// resource the request was made on - the landing a lift passenger crosses,
		// and only the starting point for a shuttle passenger, whose landing is
		// whichever door the disembark assignment selects. `coordinator` is the
		// lift or shuttle journey resource which owns the stop, and `stop` the
		// stop index the dispatcher resolved for the request.
		void allocateLiftDisembarking(TraversalRequestId requestId, TraversalResource& edgeResource,
			TraversalResource& coordinator, uint32_t stop);

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

		// ------------------------------------------------------------------
		// Queue and admission core (ADR 0004 stage 4)
		//
		// Traversal-request creation, queue tickets, queue positions and their
		// refresh, the door queue grant and release, the ladder admission family
		// with its entry-spacing rule, traversal progress and timeouts, permit
		// expiry, and the grant / allocate / deny / commit / cancel / release
		// transaction lifecycle all live here. Creating and configuring a
		// traversal resource stays with Building as entity ownership (ADR 0001);
		// the coordinator operates on the resources it is given. Building forwards
		// the entry points which still have a caller outside itself, and no caller
		// outside Building names the coordinator.
		// ------------------------------------------------------------------

		// Opening a traversal transaction. The request records the edge, the two
		// sectors and endpoints, and where its holder will select a queue position
		// from, then takes whatever waiting ownership that resource admits this
		// way - a door queue ticket, a ladder admission place, an extension lease -
		// before the request is allocated.
		TraversalRequestId createTraversalRequest(Agent const& agent,
			std::shared_ptr<const Edge> const& edge,
			std::shared_ptr<const Vertex> const& source,
			std::shared_ptr<const Vertex> const& destination);

		// Queue tickets. A ticket is the request's logical place in the line,
		// independent of where its Agent stands; it is bound to the approach lane
		// of the request's source sector nearest the endpoint it is arriving at,
		// and the lane's physical positions are refreshed to match.
		void attachQueueTicket(TraversalRequestId requestId, TraversalResource& resource);

		// Whether an Agent walking towards a threshold should stop at the queue
		// instead: the queue is already formed, or admission cannot be immediate,
		// and the nearest free position lies within the step it is about to take.
		// The Agent's early approach direction is set so it is steered there.
		bool stopForAvailableQueuePosition(Agent& agent,
			std::shared_ptr<const Edge> const& edge, Vector2 const& endpoint,
			float movementDistance);

		// Whether an Agent walking towards a threshold enters the traversal flow
		// from where it stands: its next edge crosses a plain Door (no lift
		// landing interlock) and it is within the door's crossing-width band at
		// the threshold row. The same band gates the grant (#97, ADR 0005); this
		// is the request-creation gate (#98).
		bool isAtDoorCrossingArrival(Agent const& agent,
			std::shared_ptr<const Edge> const& edge, Vector2 const& threshold);

		// Re-assign every lane's physical waiting positions. Proximity to the
		// resource endpoint wins, then proximity to the waiting Agent. A request
		// which is preparing, or whose last position timed out, keeps its logical
		// place while releasing the scarce physical one.
		void refreshQueuePositions(TraversalResource& resource);

		// The per-tick progress pass. A permit whose Agent stops closing on its
		// destination expires; a waiting Agent which stops reaching its assigned
		// position loses that position, retries for another, and is denied as
		// locally unreachable once its retries run out.
		void updateTraversalProgressAndTimeouts();

		// Permit expiry. The request returns to Pending behind its own ticket with
		// its crossing authority downgraded to a preparation lease, so the door it
		// was granted stays open only for the wait, not for a crossing it never made.
		void expireTraversalPermit(TraversalPermitId permitId);

		// The door queue grant. Each free crossing lane goes to the queued request
		// with the oldest ticket whose Agent has actually arrived at its assigned
		// position, ties broken by Agent so equal ticks stay deterministic.
		void tryGrantDoorQueue(TraversalResource& resource);

		// Surrender a request's place in every queue lane, its crossing lane, its
		// physical position and its role as the resource's preparation operator.
		void releaseDoorQueueOwnership(TraversalRequestId requestId, TraversalResource& resource);

		// Whether a request against a ladder or stairwell is admission-controlled.
		// Only the edge which claims climbing capacity is; a mount or dismount edge
		// which puts no new climber on the span must stay immediately traversable
		// or one Agent could reserve capacity twice.
		bool isLadderAdmission(TraversalRequest const& request,
			TraversalResource const& resource) const;

		// Put a request into the resource's admission queue, fixing its travel
		// direction on the way in - from the cross-deck edge, or from which side of
		// the physical midpoint the Agent approaches - and taking it a queue ticket
		// where the ladder has one.
		void attachLadderAdmissionRequest(TraversalRequestId requestId,
			TraversalResource& resource);

		// Whether every in-flight climber has cleared the entry altitude by a full
		// spacing. Climbers share one climb speed, so the separation established on
		// mounting persists; admitting before that clears would overlap the span.
		bool ladderEntryHasClearedSpacing(TraversalResource const& resource) const;

		// The ladder admission grant. The active direction keeps the span until its
		// demand is drained or its batch limit is reached, then reverses; every
		// physically available slot is filled, but only with a request in the
		// active direction whose Agent has reached the head of its queue.
		void tryGrantLadderAdmissions(TraversalResource& resource);

		// Surrender every form of waiting ownership - admission queue place, queue
		// ticket, capacity reservation - so cancellation, denial or completion
		// leaves nothing behind to hold the span closed.
		void releaseLadderAdmission(TraversalRequestId requestId,
			TraversalResource& resource);

		// Free an Agent's occupancy of the climbing span. Occupancy lasts until the
		// Agent leaves the span, not until its entry permit commits, and releasing it
		// is what makes room for the next climber.
		void releaseLadderOccupancy(AgentId agentId, TraversalResource& resource);

		// Grant a pending request: issue its permit, and for a door exchange the
		// preparation lease for the crossing lease so the door stays open under the
		// authority which now owns the crossing.
		TraversalPermitId grantTraversalRequest(TraversalRequestId requestId);

		// Allocate one pending request against the resource it was made on: the
		// lift-family dispatch, extensible preparation, the force-bridge and ladder
		// admission paths, window policy, the door preparation-and-queue path, and
		// the ordinary edge fall-through which grants or asks the edge to prepare.
		void allocateTraversalRequest(TraversalRequestId requestId,
			std::shared_ptr<const Edge> const& edge,
			std::shared_ptr<const Vertex> const& destination);

		// Deny a pending request for a typed reason, surrendering the queues,
		// positions, reservations and leases it held on its resource.
		void denyTraversalRequest(TraversalRequestId requestId,
			TraversalFailureReason reason = TraversalFailureReason::None);

		// Commit a granted traversal at its destination endpoint. Sector
		// membership transfers here and only here, a capacity reservation converts
		// into occupancy, and both permit and request end Committed. A request,
		// permit and Agent which do not agree commit nothing and return false.
		bool commitTraversal(Agent& agent, TraversalRequestId requestId, TraversalPermitId permitId,
			std::shared_ptr<const Vertex> const& destination);

		// Cancel a traversal: the rider may be asked for a safe transport exit, the
		// request surrenders its queues, reservations and leases, and the pending
		// interactions its preparation needed go with it.
		void cancelTraversal(TraversalRequestId requestId, TraversalPermitId permitId,
			bool requestSafeTransportExit = true);

		// Release the request and permit records once the Agent is finished with
		// them, surrendering any ownership still held and removing both entities.
		void releaseTraversal(TraversalRequestId requestId, TraversalPermitId permitId);

		// ------------------------------------------------------------------
		// Tick pipeline, snapshots and the edit/simulation boundary
		// (ADR 0004 stage 5)
		//
		// The fixed timestep accumulator, the six simulation phases and their
		// per-phase resource advancement (lifts and shuttles, doors, device
		// operations), tick event publication, every snapshot builder, the
		// simulation clock queries and event consumption all live here. The
		// clock, the phase marker, the event queue and the registries themselves
		// stay in Building (ADR 0001); the coordinator drives them and owns none
		// of them. Building forwards each of these entry points; no caller
		// outside Building names the coordinator.
		//
		// Pause and resume around a topology rebuild are deliberately not
		// coordinator entry points. That protocol is part of Building's
		// structural-edit contract - an edit refuses to run unless the
		// simulation is paused, and a resume refuses over dirty topology - so it
		// stays on the edit side of the boundary. What the coordinator supplies
		// is the simulation-side work that protocol performs: taking every live
		// traversal apart, remembering where each Agent was heading, putting
		// those routes back onto the rebuilt graph, and publishing the boundary
		// events.
		// ------------------------------------------------------------------

		// Driving the clock. Rendering supplies wall-clock time to update(),
		// which accumulates it and runs only whole fixed ticks; advanceTick and
		// advanceTicks are the deterministic headless seam and never read render
		// timing. Both are no-ops while the simulation is paused.
		void update(float elapsedSeconds);

		void advanceTick();

		void advanceTicks(uint64_t count);

		// One tick phase. The phase marker is set before the phase runs and
		// cleared by advanceTick once the tick has published its events, so a
		// paused or unwinding Building reports None.
		void runSimulationPhase(SimulationPhase phase);

		// Per-phase resource advancement. The lift and shuttle resources are
		// advanced first so a vehicle which arrives this tick can be boarded in
		// the phases that follow, then doors react to their sensors and drain
		// requests made against a disabled resource.
		void advanceLiftResources();

		void advanceDoorResources();

		// Tick event publication. Every completed phase is reported, then the
		// before-tick snapshot is merged against the after-tick one to publish
		// the Agents and device operations which changed.
		void publishTickEvents(SimulationSnapshot const& before);

		// Simulation clock and event consumption.
		uint64_t getSimulationTick() const;

		SimulationPhase getCurrentSimulationPhase() const;

		std::vector<SimulationEvent> consumeSimulationEvents();

		// Snapshot builders. Each is a read-only projection of one registry
		// entry; getSimulationSnapshot is the whole-world projection the tick
		// pipeline diffs against and the editor and tests read.
		AgentSnapshot makeAgentSnapshot(Agent const* agent) const;

		InteractionPointSnapshot makeInteractionPointSnapshot(InteractionPointId id,
			InteractionPoint const& point) const;

		InteractionRequestSnapshot makeInteractionRequestSnapshot(InteractionRequestId id,
			InteractionRequest const& request) const;

		DeviceOperationSnapshot makeDeviceOperationSnapshot(DeviceOperationId id,
			DeviceOperation const& operation) const;

		TraversalResourceSnapshot makeTraversalResourceSnapshot(TraversalResourceId id,
			TraversalResource const& resource) const;

		TraversalRequestSnapshot makeTraversalRequestSnapshot(TraversalRequestId id,
			TraversalRequest const& request) const;

		TraversalPermitSnapshot makeTraversalPermitSnapshot(TraversalPermitId id,
			TraversalPermit const& permit) const;

		SimulationSnapshot getSimulationSnapshot() const;

		// The simulation-side half of Building's pause/resume protocol.
		void cancelAllTraversalForTopologyRebuild();

		void restorePausedPathIntents();

		// Boundary event publication, shared with the structural-edit paths
		// which report a rebuild, a failed rebuild, a pause and a resume.
		void publishTopologyEvent(SimulationEventType type, std::string diagnostic = {});

	private:

		// Tear one Agent's traversal down for a topology rebuild: an
		// uncommitted crossing is walked back onto its source boundary before the
		// transaction is cancelled and released, and the Agent is left idle.
		void cancelTraversalForTopologyRebuild(Agent& agent);

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
