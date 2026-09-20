#pragma once

#include <string>
#include <array>
#include <vector>
#include <set>
#include <memory>
#include <map>


#include "core/Defines.h"
#include "core/Background.h"
#include "core/Facade.h"
#include "core/Layer.h"
#include "core/Location.h"
#include "core/SectorType.h"
#include "core/Door.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
#include "core/Graph.h"
#include "core/Log.h"
#include "core/Simulation.h"
#include "core/Coordination.h"
#include "core/SimulationCoordinator.h"
#include "core/EntityRegistry.h"
#include "core/Serializable.h"


namespace core
{

	class Building : public Serializable
	{
		friend class Agent;
		friend class Graph;
		// The coordinator owns no entities; it drives the registries below and
		// the private machinery beside them on the Building's behalf (ADR 0004).
		friend class SimulationCoordinator;

	public:

		struct CreateObjectResult
		{
			uint32_t index{ ~0u };
			SectorObjectType type{ SectorObjectType::None };
			std::shared_ptr<Sector> sector;
			InteractionPointId interactionPoint{};
		};

		struct CreateDoorOptions
		{
			uint32_t width{ 1 };
			bool controls[2] = { false, false };
			DoorActivationMode activationMode{ DoorActivationMode::Manual };
			float holdOpenSeconds{ CORE_DOOR_STAY_OPEN_TIME };
			// Zero derives one lane per cell of usable threshold width.
			uint32_t crossingLanes{ 0 };
			Door::OpenStyle openStyle{ Door::OpenStyle::OpenUp };
		};

		struct CreateDoorResult
		{
			CreateObjectResult door;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateBulkheadDoorOptions
		{
			bool controls[2] = { true, true };
			DoorActivationMode activationMode{ DoorActivationMode::RemoteControlled };
			float holdOpenSeconds{ CORE_BULKHEAD_DOOR_STAY_OPEN_TIME };
			uint32_t crossingLanes{ 1 };
		};

		struct CreateBulkheadDoorResult
		{
			CreateObjectResult door;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateWindowOptions
		{
			bool traversable{ false };
			Window::State initialState{ Window::State::Closed };
			Window::Style style{ Window::Style::Clear };
		};

		struct CreateWindowResult
		{
			CreateObjectResult window;
			std::shared_ptr<Window> object;
			TraversalResourceId traversalResource;
		};

		struct CreateForceBridgeOptions
		{
			uint32_t width{ 1 };
			int fromSide{ CORE_SIDE_LEFT };
			bool extensible{ true };  // implies controlled
			bool startExtended{ true };
			uint32_t controlCount{ 1 };
		};

		struct CreateForceBridgeResult
		{
			CreateObjectResult forceBridge;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateLadderOptions
		{
			uint32_t decksHigh;
			bool extensible;  // implies controlled
			bool startExtended;
			uint32_t directionalBatchLimit{ 4 };
		};

		struct CreateLadderResult
		{
			CreateObjectResult ladder;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateStairwellOptions
		{
			uint32_t decksHigh;
			int mountSide;
			// Zero preserves ordinary, unconstrained bidirectional stairs.
			uint32_t directionalCapacity{ 0 };
			uint32_t directionalBatchLimit{ 4 };
		};

		struct CreateStairwellResult
		{
			uint32_t sectorIndex{ ~0u };
			TraversalResourceId traversalResource;
		};

		struct CreateStaircaseOptions
		{
			uint32_t cellsWide{ 2 };
			// CORE_SIDE_RIGHT rises left-to-right; CORE_SIDE_LEFT is mirrored.
			int riseSide{ CORE_SIDE_RIGHT };
			// Zero is stationary; positive moves up and negative moves down.
			float speed{ 0.0f };
		};

		struct CreateLiftOptions
		{
			uint32_t cellsWide{ 1 };
			std::vector<uint32_t> stopOffsets;
			uint32_t capacity{ 2 };
			float minimumDwellSeconds{ CORE_LIFT_DOOR_PAUSE_TIME };
			float maximumBoardingSeconds{ CORE_DOOR_STAY_OPEN_TIME };
			uint32_t initialStop{ 0 };
			// Zero preserves the legacy API behaviour of ending at the highest stop.
			uint32_t decksHigh{ 0 };
			// Used only by open PlatformLifts. Enclosed Lifts retain their separate
			// minimum-dwell and maximum-boarding timings.
			float platformStopDurationSeconds{ CORE_PLATFORM_LIFT_STOP_DURATION };
		};

		struct CreateLiftResult
		{
			CreateObjectResult lift;
			std::vector<CreateDoorResult> doors;
			TraversalResourceId traversalResource;
			InteractionPointId interiorSelector;
		};

		struct CreatePlatformLiftResult
		{
			CreateObjectResult lift;
			std::vector<CreateObjectResult> buttons;
			TraversalResourceId traversalResource;
			InteractionPointId interiorSelector;
		};

		struct CreateShuttleOptions
		{
			uint32_t numCars;
			uint32_t carWidth;
			std::vector<uint32_t> stopOffsets;
			uint32_t initialStop;
			// Passenger capacity of each carriage, not the coupled vehicle total.
			uint32_t capacity{ 1 };
			float minimumDwellSeconds{ CORE_LIFT_DOOR_PAUSE_TIME };
			float maximumBoardingSeconds{ CORE_DOOR_STAY_OPEN_TIME };
			bool allowPartialLandings{ false };
			// Bit N selects carriage cell N as a one-cell-wide door.
			uint32_t doorMask{ 1u << 1 };
		};

		struct CreateShuttleResult
		{
			CreateObjectResult shuttle;
			// Fixed stop/carriage/door grid; unsupported partial landings are empty.
			std::vector<CreateDoorResult> doors;
			TraversalResourceId traversalResource;
			InteractionPointId interiorSelector;
		};

		struct ObjectMovePlan
		{
			bool valid{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t objectIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 };
			// Preview dimensions may differ from the source object (Room Ladders
			// and PlatformLifts recalculate their height at the destination).
			uint32_t previewWidth{ 0 }, previewHeight{ 0 };
			// True when the plan resizes the object instead of only moving it.
			bool resizeRequested{ false };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct PlatformLiftStopCandidate
		{
			uint32_t deckOffset{ 0 };
			bool leftButton{ false };
			bool rightButton{ false };
		};

		struct PlatformLiftEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t objectIndex{ ~0u };
			CreateLiftOptions options;
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct WalkwayEditPlan
		{
			bool valid{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t objectIndex{ ~0u };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		// A rectangular edit to a Sector's footprint: remove it, move it, or resize
		// it in place.  Rooms and Corridors are the primary subject, and a Background
		// shares the shape because it is edited the same way and needs the same
		// consequence list: taking a Background away from the Windows looking into it
		// takes those Windows with it.
		struct LocationEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, cellsWide{ 0 }, decksHigh{ 0 };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const
			{
				return !consequences.empty();
			}
		};

		struct LiftEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, cellsWide{ 0 }, decksHigh{ 0 };
			std::vector<uint32_t> stopOffsets;
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct ShuttleEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, cellsWide{ 0 };
			std::vector<uint32_t> stopOffsets;
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct LadderEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, decksHigh{ 0 };
			CreateLadderOptions options{ 0, false, true };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct StairwellEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 }, decksHigh{ 0 };
			CreateStairwellOptions options{ 0, CORE_SIDE_LEFT };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct StaircaseEditPlan
		{
			bool valid{ false };
			bool remove{ false };
			bool move{ false };
			uint32_t sectorIndex{ ~0u };
			uint32_t x{ 0 }, y{ 0 };
			CreateStaircaseOptions options{};
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

		struct ShuttleStopCandidate
		{
			uint32_t sectorIndex{ ~0u };
			uint32_t stopOffset{ 0 };
		};

		// Deleting a Layer is destructive, so its consequences are listed before the
		// user confirms them.  Every Sector on the deleted Layer is removed, Transits
		// on that Layer and on the Layer directly behind it lose their landings and
		// are removed, thresholds which cross the deleted Layer are removed, and
		// Agents in removed Sectors are removed.  A Window which the compaction would
		// leave on the back-most Layer is removed too, since a Window needs a Layer
		// behind it.  The Windows on the Layer in front which looked into the deleted
		// Layer go with it, and each one is named in the consequence list.  Layers
		// behind the deleted Layer compact forward by one, keeping their names.
		struct LayerDeletePlan
		{
			bool valid{ false };
			uint32_t layerIndex{ 0 };
			std::string layerName;
			uint32_t layerCountBefore{ 0 };
			uint32_t layerCountAfter{ 0 };
			uint32_t locationsRemoved{ 0 };
			uint32_t transitsRemoved{ 0 };
			uint32_t backgroundsRemoved{ 0 };
			uint32_t doorsRemoved{ 0 };
			uint32_t windowsRemoved{ 0 };
			// Windows which never crossed the deleted Layer, but are deleted because the
			// compaction leaves them on the back-most Layer with nothing behind them.
			uint32_t windowsStranded{ 0 };
			uint32_t agentsRemoved{ 0 };
			std::string diagnostic;
			std::vector<std::string> consequences;

			[[nodiscard]] bool requiresConfirmation() const { return !consequences.empty(); }
		};

	public:

		static CreateDoorOptions ManualDoor1Options, RemoteControlledDoor1Options, UnavailableDoor1Options;

		static CreateDoorOptions ManualDoor2Options, RemoteControlledDoor2Options, UnavailableDoor2Options;

	private:

		std::string mName;

		uint32_t mCellsWide, mDecksHigh;

		std::vector<std::shared_ptr<Layer>> mLayers;
		std::vector<std::string> mLayerNames;

		std::vector<std::shared_ptr<Sector>> mSectors;


		std::shared_ptr<Graph> mGraph;

		// Simulation behaviour belongs to the coordinator (ADR 0004). Building
		// owns it and stays the facade (design pattern) through which every
		// caller, Agent included, reaches it; the coordinator owns no entities
		// and reaches the registries below through this Building.
		SimulationCoordinator mSimulationCoordinator;

		EntityRegistry<AgentId, Agent> mAgents;

		// Legacy pointer-facing APIs use this reverse index only to recover an ID;
		// the registry above remains the sole owner.
		std::map<Agent const*, AgentId> mAgentIds;

		EntityRegistry<InteractionPointId, InteractionPoint> mInteractionPoints;

		EntityRegistry<InteractionRequestId, InteractionRequest> mInteractionRequests;

		EntityRegistry<DeviceOperationId, DeviceOperation> mDeviceOperations;

		EntityRegistry<TraversalResourceId, TraversalResource> mTraversalResources;

		EntityRegistry<TraversalRequestId, TraversalRequest> mTraversalRequests;

		EntityRegistry<TraversalPermitId, TraversalPermit> mTraversalPermits;

		uint64_t mSimulationTick{ 0 };

		uint64_t mNextEventSequence{ 1 };

		uint64_t mNextQueueTicketValue{ 1 };

		uint64_t mNextDoorOpenLeaseValue{ 1 };

		double mAccumulatedTime{ 0.0 };

		SimulationPhase mCurrentPhase{ SimulationPhase::None };

		std::vector<SimulationEvent> mEvents;

		TraversalWaitingPolicy mTraversalWaitingPolicy;

		// Structural edits are transactional at the graph boundary. The world may
		// only be changed after an explicit pause; the previous graph remains live
		// until a replacement has built and validated successfully.
		bool mBuildFinished{ false };
		bool mSimulationPaused{ false };
		bool mTopologyDirty{ true };
		bool mTopologyValid{ false };
		uint64_t mTopologyGeneration{ 0 };
		std::string mTopologyDiagnostic;

		struct TopologyPathIntent
		{
			SectorId destinationSector;
			Vector2 destinationPosition;
			bool wasPathing{ false };
		};
		std::map<AgentId, TopologyPathIntent> mPausedPathIntents;

		Log mBuildLog;

		// Authored facade operations are the persistence boundary. Replaying them
		// reconstructs sectors, objects, controls, and traversal resources while
		// finishBuild() regenerates graph and pathing data.
		enum class ConstructionType : uint8_t
		{
			Corridor,
			Room,
			Ladder,
			Stairwell,
			Staircase,
			Lift,
			Shuttle,
			Door,
			Window,
			BulkheadDoor,
			LightSwitch,
			ForceBridge,
			SectorLadder,
			PlatformLift,
			Walkway,
			Marker,
			RemoveWall,
			RemoveMarker,
			ObjectTombstone,
			// Appended last: version 1 stored the record kind numerically, so every
			// earlier value has to keep its number.
			Background,
			// Appended after Background for the same reason: a Facade is its own
			// producing record, replayed with all wall ends open intrinsically
			// (ADR 0003).
			Facade
		};

		// Compact tagged command storage. Field meanings are determined by type and
		// kept private so the public model is not coupled to its YAML representation.
		struct ConstructionRecord
		{
			ConstructionType type{};
			std::string name{};

			// The Layer the record's object is authored on.  For a Transit this is the
			// Layer it sits on; for a threshold it is the front Layer of its pair.
			// Records written before Transits and Doors carried a Layer leave this unset
			// and replay against the front pair, which is where every legacy object lived.
			uint32_t layer{ ~0u };

			uint32_t a{ 0 }, b{ 0 }, c{ 0 }, d{ 0 }, e{ 0 }, f{ 0 }, g{ 0 }, h{ 0 };
			int32_t i{ 0 }, j{ 0 };
			float x{ 0.0f }, y{ 0.0f }, z{ 0.0f };
			bool p{ false }, q{ false };
			std::vector<uint32_t> values{};
		};

		std::vector<ConstructionRecord> mConstructionRecords;
		bool mDeserializingConstruction{ false };

		struct PhysicalControlCandidate
		{
			uint32_t cellX{ 0 };
			int side{ CORE_SIDE_MIDDLE };
		};

		struct PhysicalControlPlacement
		{
			uint32_t layerIndex{ 0 };
			uint32_t sectorIndex{ 0 };
			uint32_t objectIndex{ 0 };
			uint32_t cellY{ 0 };
			std::vector<PhysicalControlCandidate> candidates;
			uint32_t defaultCandidate{ 0 };
			uint32_t currentCandidate{ 0 };
			float edgeInset{ 0.0f };
			Vector2 interactionOffset{};
			bool hasInteractionOffset{ false };
		};

		std::vector<PhysicalControlPlacement> mPhysicalControlPlacements;

	private:

		bool childrenModified() const override;

		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;

		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		void recordConstruction(ConstructionRecord record);

		static std::string constructionTypeName(ConstructionType type);

		static ConstructionType constructionTypeFromName(std::string const& name);

		// A record which creates a Sector. A producing record's position among the
		// producers is its live Sector index, so every record-to-Sector mapping has to
		// agree on exactly this set.
		static bool constructionTypeCreatesSector(ConstructionType type);

		void serializeConstructionRecord(Serializer& serializer, ConstructionRecord const& record) const;

		ConstructionRecord deserializeConstructionRecord(Serializer& serializer, uint32_t version) const;

		void applyConstructionRecord(ConstructionRecord const& record);

		bool prepareLocationEdit(LocationEditPlan const& plan,
			std::vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
			std::string& diagnostic) const;

		bool prepareObjectMove(ObjectMovePlan const& plan,
			std::vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
			uint32_t& newObjectIndex, std::string& diagnostic) const;

		bool normalizeRoomLadderRecords(std::vector<ConstructionRecord>& records,
			std::string& diagnostic) const;

		bool roomLadderIsActive(std::shared_ptr<const Ladder> const& ladder) const;

		bool forceBridgeIsActive(std::shared_ptr<const ForceBridge> const& forceBridge) const;

		bool platformLiftIsActive(std::shared_ptr<const Lift> const& lift) const;

		bool preparePlatformLiftEdit(PlatformLiftEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		bool prepareLiftEdit(LiftEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		bool prepareShuttleEdit(ShuttleEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		bool prepareLadderEdit(LadderEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		bool prepareStairwellEdit(StairwellEditPlan const& plan,
			std::vector<ConstructionRecord>& records, std::string& diagnostic) const;

		std::vector<ConstructionRecord> canonicalConstructionRecords(
			std::vector<ConstructionRecord> records) const;

		void rebuildFromConstructionRecords(std::vector<ConstructionRecord> records,
			uint32_t movedSectorIndex = ~0u, int deltaX = 0, int deltaY = 0);

		static std::string defaultLayerName(uint32_t layer);

		struct LayerDeleteImpact
		{
			std::vector<bool> sectorRemoved;
			uint32_t locationsRemoved{ 0 };
			uint32_t transitsRemoved{ 0 };
			uint32_t backgroundsRemoved{ 0 };
			uint32_t doorsRemoved{ 0 };
			uint32_t windowsRemoved{ 0 };
			// Windows which do not cross the deleted Layer but would compact onto the new
			// back-most Layer, and so lose the Layer behind them.
			uint32_t windowsStranded{ 0 };
		};

		// Layers of the live Sectors which hold the threshold object authored at a
		// cell.  A threshold is shared by the Sectors on both sides of it.
		std::set<uint32_t> thresholdLayers(SectorObjectType type, uint32_t x, uint32_t y) const;

		// Every distinct Window the Building holds, as it is registered in a Sector.
		// A Window carries no cell position of its own - the cell it sits on belongs
		// to its SectorObject - so the two travel together.  A Window's SectorObject
		// is registered in both of the Sectors it joins, so the same Window is seen
		// twice while scanning and is reported once.
		std::vector<std::shared_ptr<const WindowSectorObject>> allWindowObjects() const;

		// The Windows which look into `background` and stop looking at it once the
		// Background occupies the given footprint.  A Background which is removed
		// covers nothing, so every Window looking into it is uncovered; a moved or
		// shrunk one uncovers only the cells it lets go of.  A Window looks straight
		// behind itself, so its back cells are its own rectangle on the Layer behind.
		std::vector<std::shared_ptr<const WindowSectorObject>> windowsUncoveredByBackground(
			std::shared_ptr<const Sector> const& background, bool covered,
			uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const;

		// The consequence lines naming every Window which loses the Background the
		// plan edits, so the confirmation popup spells out the cascade rather than
		// only counting it.
		void addUncoveredWindowConsequences(LocationEditPlan& plan) const;

		// Authored construction records rewritten for a Background removal, move, or
		// resize: the Background record follows the plan, the records of the Windows
		// which lose it are dropped, and every remaining Sector index is re-pointed
		// against the compacted Building.
		bool prepareBackgroundEdit(LocationEditPlan const& plan,
			std::vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
			std::string& diagnostic) const;

		// Authored construction records rewritten for a Layer deletion: casualties
		// are dropped and every remaining Layer index compacts forward by one.
		std::vector<ConstructionRecord> recordsWithoutLayer(uint32_t layerIndex,
			LayerDeleteImpact& impact) const;

		void resetForDeserialization(std::string name, uint32_t cellsWide, uint32_t decksHigh);

		// Constructs a validation candidate with the same dimensions and layer count as this Building.
		std::unique_ptr<Building> makeCandidateBuilding() const;

		// Shared body of planRemoveLocation and planRemoveFacade: the same
		// occupiable-removal cascade, gated on the Sector type the caller
		// allows and refusing anything else with the caller's own diagnostic.
		LocationEditPlan planRemoveOccupiable(uint32_t sectorIndex,
			SectorType requiredType, std::string const& refusal) const;

		void validateCellOccupied(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellUnoccupied(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellIsInSector(std::string const& caller, uint32_t x, uint32_t y, std::shared_ptr<const Sector> sector) const;

		void validateCellHasNoObject(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellHasNoDoor(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		bool validateStaircaseEndpoint(uint32_t layerIndex, uint32_t x, uint32_t y, bool upperEndpoint,
			int riseSide, std::string& diagnostic) const;

		void validateCellHasNoPhysicalControl(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, int side) const;

		void validateCellTraversableOnFoot(std::string const& caller, std::string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateLayer(std::string const& caller, uint32_t layerIndex) const;

		void validateBounds(std::string const& caller, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const;

		void validateLayerSpace(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const;

		void validateObjectAllowedInSector(std::string const& caller, SectorObjectType type, uint32_t sectorIndex) const;

		void validateObjectAllowedInSectorAsLookTarget(std::string const& caller, SectorObjectType type, uint32_t sectorIndex) const;

		// The span must fall inside a single Sector, because crossing a Sector boundary
		// breaks the traversal geometry the caller is about to build. The one
		// relaxation is allowAllBackgroundSpan, used for the Layer behind a Window:
		// there a span of nothing but Backgrounds may cover several Background
		// Sectors, since a Background takes no part in traversal and what lies behind
		// an aperture is read from the cell grid. A span that mixes a Background with
		// any other Sector is still refused.
		void validateSpaceOnlyInOneSector(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, bool allowAllBackgroundSpan = false) const;

		void validateSectorDoorOptions(std::string const& caller, CreateDoorOptions const& options) const;

		void validateSectorForceBridgeOptions(std::string const& caller, CreateForceBridgeOptions const& options) const;

		void validateSectorLadderOptions(std::string const& caller, CreateLadderOptions const& options) const;

		void validateLiftOptions(std::string const& caller, CreateLiftOptions const& options) const;

		void validateShuttleOptions(std::string const& caller, CreateShuttleOptions const& options) const;

		void beginStructuralEdit(std::string const& operation);

		// The simulation-side work of a topology rebuild - taking every live
		// traversal apart, remembering the route each Agent was working to,
		// restoring those routes onto the rebuilt graph, and publishing the
		// boundary events - lives in SimulationCoordinator (ADR 0004 stage 5).
		// pauseSimulation and resumeSimulation stay here, on the structural-edit
		// side of the edit/simulation boundary, and call it.

		void validateTraversalTopology(Graph const& graph) const;

		std::shared_ptr<Sector> _getSector(uint32_t index);

		std::shared_ptr<Layer> getLayer(uint32_t layerIndex);

		uint32_t createLocation(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, bool isCorridor);

		// A Transit is created on layerIndex and lands on the Layer directly in front of
		// it, so every landing cell is read from layerInFront(layerIndex).
		uint32_t createLadder(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options);

		uint32_t createStairwell(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t decksHigh, int mountSide);

		uint32_t createStaircase(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
			int riseSide, float speed);

		CreateObjectResult createLift(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
			uint32_t decksHigh, std::vector<uint32_t> const& stopOffsets);

		CreateObjectResult createShuttle(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth, std::vector<uint32_t> const& stopOffsets);

		// A Door is authored on the front Layer of its pair and opens into the Layer
		// directly behind it.
		CreateObjectResult createDoor(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createWindow(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createBulkheadDoor(uint32_t layerIndex, uint32_t x, uint32_t y, int side);

		CreateObjectResult createPhysicalControl(std::string const& name, uint32_t layerIndex, uint32_t x, uint32_t y, int side, uint32_t flags, uint32_t* vertexIdentifier = nullptr,
			uint32_t alternateX = ~0u, int alternateSide = -1);

		void reflowPhysicalControls(uint32_t layerIndex, uint32_t sectorIndex, uint32_t y);
		void bindPhysicalControl(CreateObjectResult& control, InteractionPointId point);
		InteractionPointId createPhysicalControlInteractionPoint(std::string const& name,
			CreateObjectResult& control, float standingY, float reach,
			float durationSeconds, std::vector<InteractionBinding> bindings);

		CreateObjectResult createWalkway(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createMarker(uint32_t layerIndex, uint32_t x, uint32_t y, float xOffset, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createForceBridge(uint32_t layerIndex, uint32_t x, uint32_t y, CreateForceBridgeOptions const& options);

		CreateObjectResult createLadderSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createPlatformLiftSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLiftOptions const& options, uint32_t* vertexIdentifier = nullptr);

		CreateDoorResult _addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, CreateDoorOptions const& options,
			bool controlsAreExternallyBound = false);

		CreateObjectResult _createSectorButton(std::string const& name, std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t flags, uint32_t* index = nullptr);

		CreateObjectResult _createDoorButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t flags, uint32_t* index = nullptr);

		CreateObjectResult _createBulkheadDoorButton(std::shared_ptr<const Sector> sector, uint32_t y, int side, uint32_t* index = nullptr);

		CreateObjectResult _createForceBridgeButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index = nullptr);

		CreateObjectResult _createLadderButton(std::shared_ptr<const Sector> sector,
			uint32_t x, uint32_t y, int side, uint32_t flags,
			uint32_t* index = nullptr, bool insetWithinCell = false);

		CreateObjectResult _createPlatformLiftButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index = nullptr);

		uint32_t addLocation(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, bool isCorridor);

		void buildGraph();

		// Forwards to SimulationCoordinator, which owns Agent placement (ADR 0004).
		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId, uint32_t deckOffset, float xOffset);

		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId);

		// Snapshot building - every per-entity projection and the whole-world
		// SimulationSnapshot - lives in SimulationCoordinator (ADR 0004 stage 5).
		// Building keeps the whole-world forward in the public section below, plus
		// one private forward: creating and removing a traversal resource is entity
		// ownership which stays with Building (ADR 0001), and the lifecycle events
		// those paths publish carry the resource snapshot the coordinator builds.
		TraversalResourceSnapshot makeTraversalResourceSnapshot(TraversalResourceId id,
			TraversalResource const& resource) const;

		// Interaction and device-operation orchestration lives in
		// SimulationCoordinator (ADR 0004); each entry point below - public or
		// private - forwards to it.
		DeviceOperationId findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester);

		void advanceDeviceOperations();

		void allocateInteractions();

		void moveInteractions(float frameTime);

		void tryPressUpcomingDoorButton(Agent& agent, Vector2 const& movementStart,
			Vector2 const& movementEnd);

		void pressPhysicalControl(InteractionPointId point);

		void updateInteractionResults();

		InteractionRequestId requestInteractionForTraversal(InteractionPointId point, AgentId actor);

		InteractionRequestId requestInteractionWhilePassing(InteractionPointId point, AgentId actor);

		// Remote-door and extensible traversal preparation, and the queue and
		// admission core - traversal-request creation, queue tickets, queue
		// positions and their refresh, the door queue grant and release, the
		// ladder admission family with its entry-spacing rule, traversal progress
		// and timeouts, permit expiry, and the grant / allocate / deny / commit /
		// cancel / release transaction lifecycle - all live in
		// SimulationCoordinator (ADR 0004). Building keeps the entry points which
		// still have a caller outside Building and forwards them; the helpers
		// reached only from inside the coordinator keep no forward. Configuring a
		// traversal resource's queue lanes stays with Building: that is entity
		// ownership (ADR 0001), not coordination.
		void refreshQueuePositions(TraversalResource& resource);

		bool stopForAvailableQueuePosition(Agent& agent,
			std::shared_ptr<const Edge> const& edge, Vector2 const& endpoint,
			float movementDistance);

		void configureLadderQueueLanes(TraversalResourceId resource,
			std::array<SectorId, 2> const& sectors,
			std::array<Vector2, 2> const& endpoints);

		void configureForceBridgeQueueLanes(TraversalResourceId resource,
			SectorId sector, std::array<Vector2, 2> const& endpoints);

		void updateTraversalProgressAndTimeouts();

		// Door open lease acquisition and release live in SimulationCoordinator
		// (ADR 0004); these forward.
		DoorOpenLeaseId acquireDoorOpenLease(TraversalResource& resource,
			DoorOpenLeaseKind kind, TraversalRequestId request = {});

		bool releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease);

		// The lift allocation dispatcher and its branches - the platform lift
		// dispatch, the journey resource and stop resolution, the enabled check
		// and the boarding / riding / disembarking classification - live in
		// SimulationCoordinator (ADR 0004), reached only from the coordinator's
		// own traversal-request allocation, so no forward is left for them.

		uint32_t findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const;

		uint32_t findAgentLiftDestination(Agent const& agent, TraversalResource const& resource) const;

		bool liftHasDisembarkDemand(TraversalResource const& resource, uint32_t stop) const;

		void addLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner);

		void removeLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner);

		uint32_t chooseNextLiftStop(TraversalResource& resource) const;

		bool isLiftBoardingDirectionCompatible(TraversalResource& resource,
			uint32_t originStop, uint32_t destinationStop);

		void releaseLiftAdmission(TraversalRequestId requestId, TraversalResource& resource);

		// Shuttle door assignment - the passenger-carriage lookup, the boarding and
		// disembark door selection, and the retargeting they share - lives in
		// SimulationCoordinator (ADR 0004). Both selections are reached only from
		// inside the coordinator now, so no forward is left for them.

		void requestLiftPassengerSafeExit(AgentId passenger, TraversalFailureReason reason);

		void assignLiftSafeExitPaths(TraversalResource& resource);

		bool replaceOnboardLiftDestination(Agent& agent, std::shared_ptr<Path> const& path,
			uint32_t& sourceNode);

		// The traversal transaction lifecycle below - request creation, allocation,
		// denial, commit, cancel and release - also lives in SimulationCoordinator
		// (ADR 0004); these forward. The grant is reached only from inside the
		// coordinator, so no forward is left for it.
		TraversalRequestId createTraversalRequest(Agent const& agent, std::shared_ptr<const Edge> const& edge,
			std::shared_ptr<const Vertex> const& source, std::shared_ptr<const Vertex> const& destination);

		void allocateTraversalRequest(TraversalRequestId requestId,
			std::shared_ptr<const Edge> const& edge, std::shared_ptr<const Vertex> const& destination);

		void denyTraversalRequest(TraversalRequestId requestId,
			TraversalFailureReason reason = TraversalFailureReason::None);

		bool commitTraversal(Agent& agent, TraversalRequestId requestId, TraversalPermitId permitId,
			std::shared_ptr<const Vertex> const& destination);

		void cancelTraversal(TraversalRequestId requestId, TraversalPermitId permitId,
			bool requestSafeTransportExit = true);

		void releaseTraversal(TraversalRequestId requestId, TraversalPermitId permitId);

		// A handle an Agent owns inside a capacity resource outlives nothing: once the
		// Agent is gone the manifest slot can never be disembarked and the Lift, Shuttle
		// or Ladder is permanently one place short (ticket #57). Deleting an Agent
		// therefore surrenders every such claim before the entity is destroyed. The
		// release lives in SimulationCoordinator (ADR 0004); these forward.
		bool holdsTraversalOwnership(AgentId id) const;
		void releaseAgentFromResource(TraversalResource& resource, AgentId id);
		void releaseTraversalOwnership(AgentId id);

	public:

		Building(std::string const& name, uint32_t cellsWide, uint32_t decksHigh);

		virtual ~Building();

		std::string const& getName() const;

		uint32_t getCellsWide() const;

		uint32_t getDecksHigh() const;

		uint32_t getLayerCount() const;

		std::string const& getLayerName(uint32_t layerIndex) const;
		void setLayerName(uint32_t layerIndex, std::string name);

		// Appends a new back-most Layer with the default name and returns its index.
		// Throws if the Building already has CORE_MAX_LAYERS layers.
		uint32_t addLayer();

		// Plans the destructive deletion of a Layer.  The plan is side-effect free
		// and validates that the compacted Building can be rebuilt before it is
		// offered for confirmation.  A Building must keep at least two Layers.
		LayerDeletePlan planDeleteLayer(uint32_t layerIndex) const;

		// Applies a confirmed Layer deletion by rewriting and replaying the authored
		// construction records.  Leaves the simulation paused.  Throws if the Layer
		// can no longer be deleted.
		bool applyDeleteLayer(LayerDeletePlan const& plan);

		uint32_t getNumSectors() const;

		std::shared_ptr<const Layer> getLayer(uint32_t layerIndex) const;

		std::shared_ptr<const Sector> getSector(uint32_t index) const;

		std::vector<std::shared_ptr<const Sector>> getSectorsInBounds(uint32_t layerIndex, float x, float y, float width, float height) const;

		std::vector<std::shared_ptr<const Sector>> getSectors(uint32_t layerIndex) const;

		std::shared_ptr<const Graph> getGraph() const;

		Log const& getBuildLog() const;

		// Sector types
		// A Corridor is a Location, so it may sit on any Layer.  The Layer-less form
		// keeps the front-most Layer as its default.
		uint32_t addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh = 1);
		uint32_t addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t decksHigh = 1);

		uint32_t addRoom(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight = CORE_ROOM_MAX_HEIGHT);

		// A Background is a non-occupiable Sector: it takes space on its own Layer and
		// nothing else. It may sit on any Layer, front-most and back-most included; a
		// "back layers only" rule would re-introduce the Fore/Back special-casing that
		// ADR 0002 removed. Adjacent Backgrounds are allowed and never merge: each
		// keeps its own colour.
		uint32_t addBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t decksHigh, BackgroundColour const& colour = {});

		bool canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t decksHigh, std::string* diagnostic = nullptr) const;

		// A Facade is an occupiable Location with its perimeter walls all open by
		// construction: it hosts objects and agents exactly as a Room does and is
		// rendered as a solid opaque colour (ADR 0003). Placement follows the
		// Room rule - the same Layer, bounds, free-space, and height validation.
		// The named form is the persistence form: the record carries the Facade's
		// name alongside its footprint and packed colour. The unnamed form keeps
		// the generic "Facade" name.
		uint32_t addFacade(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight = CORE_ROOM_MAX_HEIGHT,
			BackgroundColour const& colour = Facade::defaultColour());

		uint32_t addFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t decksHigh, float topDeckHeight = CORE_ROOM_MAX_HEIGHT,
			BackgroundColour const& colour = Facade::defaultColour());

		bool canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t decksHigh, float topDeckHeight, std::string* diagnostic = nullptr) const;
	
		// A Transit is authored on layerIndex, the Layer it occupies, and lands on the
		// Layer directly in front of it.  The front-most Layer can carry no Transit.
		CreateLadderResult addLadder(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLadderOptions const& options);

		bool canAddLadder(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t decksHigh,
			std::string* diagnostic = nullptr) const;

		bool getLadderOptions(uint32_t sectorIndex, CreateLadderOptions& options) const;

		uint32_t addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t decksHigh, int mountSide);

		CreateStairwellResult addStairwell(uint32_t layerIndex, uint32_t y, uint32_t x,
			CreateStairwellOptions const& options);

		bool canAddStairwell(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t decksHigh,
			std::string* diagnostic = nullptr) const;

		bool getStairwellOptions(uint32_t sectorIndex, CreateStairwellOptions& options) const;

		uint32_t addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, int riseSide,
			float speed = 0.0f);
		uint32_t addStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, CreateStaircaseOptions const& options);
		bool canAddStaircase(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, int riseSide,
			std::string* diagnostic = nullptr) const;
		bool getStaircaseOptions(uint32_t sectorIndex, CreateStaircaseOptions& options) const;

		CreateLiftResult addLift(uint32_t layerIndex, uint32_t y, uint32_t x, CreateLiftOptions const& options);

		// One landing row of an enclosed Lift shaft: the row of cells on the Layer
		// directly in front of the shaft's own Layer that the shaft overlaps at one
		// deck offset.
		struct LiftLandingRow
		{
			uint32_t offset{ 0 };
			// The Location the shaft overlaps on the landing Layer; null over a gap.
			std::shared_ptr<const Location> location;
			// A single Location fills the shaft width and every cell is walkable.
			bool fullyOverlapping{ false };
			// An Object or Marker blocks the row.
			bool obstructed{ false };
			// The shaft leaves Location width beside it for the stop's call control.
			bool callButtonSpace{ true };

			// A row the shaft can serve as a stop.
			bool usableForStop() const
			{
				return fullyOverlapping && !obstructed && callButtonSpace;
			}
		};

		// The landing rows of a cellsWide-by-decksHigh shaft at (y, x) on layerIndex.
		// Rows are always read from the Layer directly in front of layerIndex; the
		// front-most Layer has nothing in front of it and yields no rows.
		std::vector<LiftLandingRow> getLiftLandingRows(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t decksHigh) const;

		// Derives stops from every fully overlapping landing-layer corridor row.
		CreateLiftResult addLift(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh);

		CreateShuttleResult addShuttle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, CreateShuttleOptions const& options);

		// Sector object types
		// Doors may connect any two Room, Corridor, or Facade Locations. The legacy
		// method name remains part of the public API. A Door is authored on the front
		// Layer of the pair it crosses.
		bool canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			std::string* diagnostic = nullptr) const;

		bool canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, CreateDoorOptions const& options,
			std::string* diagnostic = nullptr) const;

		// Resolves a cell over a lift to its complete landing-door footprint.  The
		// Layer searched is the Transit's own Layer, not the landing Layer.
		bool getLiftLandingGeometry(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t& landingX, uint32_t& landingWidth) const;

		bool isLiftOwnedDoor(std::shared_ptr<const SectorObject> const& object,
			uint32_t* liftSectorIndex = nullptr, uint32_t* stopIndex = nullptr) const;

		bool isLiftOwnedControl(std::shared_ptr<const SectorObject> const& object,
			uint32_t* liftSectorIndex = nullptr, uint32_t* stopIndex = nullptr) const;

		bool isShuttleOwnedDoor(std::shared_ptr<const SectorObject> const& object,
			uint32_t* shuttleSectorIndex = nullptr, uint32_t* stopIndex = nullptr,
			uint32_t* carriageIndex = nullptr) const;

		bool isShuttleOwnedControl(std::shared_ptr<const SectorObject> const& object,
			uint32_t* shuttleSectorIndex = nullptr, uint32_t* stopIndex = nullptr) const;

		std::vector<uint32_t> getValidShuttleStopOffsets(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t numCars, uint32_t carWidth,
			bool allowPartialLandings, uint32_t doorMask) const;

		bool getShuttleOptions(Shuttle const* shuttle, CreateShuttleOptions& options) const;

		// Stop alignments a Door dropped at doorX could open onto.  The Layer searched
		// is the Shuttle Transit's own Layer, not the Layer the Door is authored on;
		// a caller placing a Door on Layer L passes layerBehind(L), matching
		// getLiftLandingGeometry.  A Shuttle on any other Layer is never returned.
		std::vector<ShuttleStopCandidate> getShuttleStopCandidatesForDoor(
			uint32_t shuttleLayer, uint32_t y, uint32_t doorX) const;

		bool getSectorDoorOptions(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
			CreateDoorOptions& options) const;

		// Re-authors an ordinary Door's opening style.  The authored construction
		// record is the persistence boundary, so the record and the live Door move
		// together: save/load, clipboard readback, moves, and undo/redo all carry
		// the new style.  Opening style feeds only the Door's rendering - timing,
		// state, obstruction, and traversal are untouched.
		bool setSectorDoorOpenStyle(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t width,
			Door::OpenStyle style, std::string* diagnostic = nullptr);

		CreateDoorResult addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x);

		CreateDoorResult addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, CreateDoorOptions const& options);

		// Adds a physical open control in the selected Door's owning Location.
		// Placement against the left or right edge is derived from the Door and Location geometry.
		CreateObjectResult addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex);

		bool removeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex);

		// A Window needs the Layer directly behind the Layer it is authored on, so the
		// back-most Layer can never take a new one.
		bool canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide = 1, uint32_t decksHigh = 1,
			std::string* diagnostic = nullptr) const;

		uint32_t addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh);

		CreateWindowResult addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t decksHigh, CreateWindowOptions const& options);

		bool getSectorWindowOptions(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t decksHigh, CreateWindowOptions& options) const;

		bool removeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex);

		bool canAddSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x, int side,
			CreateBulkheadDoorOptions const& options,
			std::string* diagnostic = nullptr) const;

		CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			int side);

		CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			int side, CreateBulkheadDoorOptions const& options);

		bool getSectorBulkheadDoorOptions(uint32_t sectorIndex, uint32_t objectIndex,
			CreateBulkheadDoorOptions& options) const;

		std::shared_ptr<const SectorObject> applySectorBulkheadDoorOptions(uint32_t sectorIndex,
			uint32_t objectIndex, CreateBulkheadDoorOptions const& options);

		bool removeSectorBulkheadDoor(uint32_t sectorIndex, uint32_t objectIndex);

		bool isBulkheadDoorOwnedControl(std::shared_ptr<const SectorObject> const& object,
			uint32_t* doorSectorIndex = nullptr, uint32_t* doorObjectIndex = nullptr) const;

		CreateObjectResult addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset);

		CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex, uint32_t deckIndex,
			uint32_t xOffset);

		CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex, uint32_t deckIndex,
			uint32_t xOffset, CreateForceBridgeOptions const& options);

		bool canAddSectorForceBridge(uint32_t sectorIndex, uint32_t deckIndex,
			uint32_t xOffset, CreateForceBridgeOptions const& options,
			std::string* diagnostic = nullptr) const;

		bool calculateSectorForceBridgeWidthToRight(uint32_t sectorIndex,
			uint32_t deckIndex, uint32_t xOffset, uint32_t& width,
			std::string* diagnostic = nullptr) const;

		bool getSectorForceBridgeOptions(uint32_t sectorIndex, uint32_t objectIndex,
			CreateForceBridgeOptions& options) const;

		std::shared_ptr<const SectorObject> applySectorForceBridgeOptions(uint32_t sectorIndex,
			uint32_t objectIndex, CreateForceBridgeOptions const& options);

		bool removeSectorForceBridge(uint32_t sectorIndex, uint32_t objectIndex);

		bool isForceBridgeOwnedControl(std::shared_ptr<const SectorObject> const& object,
			uint32_t* forceBridgeSectorIndex = nullptr,
			uint32_t* forceBridgeObjectIndex = nullptr) const;

		CreateLadderResult addSectorLadder(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateLadderOptions const& options);

		// Room Ladders are point-placed objects. Their height is always derived
		// from the nearest Walkway above their Ground/Walkway base.
		bool canAddRoomLadder(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset,
			uint32_t* decksHigh = nullptr, std::string* diagnostic = nullptr) const;

		CreateLadderResult addRoomLadder(uint32_t sectorIndex, uint32_t deckIndex,
			uint32_t xOffset);

		CreateLadderResult addRoomLadder(uint32_t sectorIndex, uint32_t deckIndex,
			uint32_t xOffset, CreateLadderOptions options);

		bool getRoomLadderOptions(uint32_t sectorIndex, uint32_t objectIndex,
			CreateLadderOptions& options) const;

		std::shared_ptr<const SectorObject> applyRoomLadderOptions(uint32_t sectorIndex,
			uint32_t objectIndex, CreateLadderOptions const& options);

		bool removeRoomLadder(uint32_t sectorIndex, uint32_t objectIndex);

		CreatePlatformLiftResult addSectorPlatformLift(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateLiftOptions const& options);

		std::vector<PlatformLiftStopCandidate> getPlatformLiftStopCandidates(
			uint32_t sectorIndex, uint32_t xOffset) const;

		bool canAddPlatformLift(uint32_t sectorIndex, uint32_t xOffset,
			CreateLiftOptions const& options, std::string* diagnostic = nullptr) const;

		bool getPlatformLiftOptions(uint32_t sectorIndex, uint32_t objectIndex,
			CreateLiftOptions& options) const;

		PlatformLiftEditPlan planPlatformLiftEdit(uint32_t sectorIndex, uint32_t objectIndex,
			CreateLiftOptions const& options) const;

		PlatformLiftEditPlan planRemovePlatformLift(uint32_t sectorIndex,
			uint32_t objectIndex) const;

		std::shared_ptr<const SectorObject> applyPlatformLiftEdit(
			PlatformLiftEditPlan const& plan);

		WalkwayEditPlan planRemoveSectorWalkway(uint32_t sectorIndex,
			uint32_t objectIndex) const;

		bool applyWalkwayEdit(WalkwayEditPlan const& plan);

		bool canAddSectorWalkway(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset,
			std::string* diagnostic = nullptr) const;

		CreateObjectResult addSectorWalkway(uint32_t sectorIndex, uint32_t deckIndex,
			uint32_t xOffset);

		bool removeSectorWalkway(uint32_t sectorIndex, uint32_t objectIndex);

		bool canAddSectorMarker(uint32_t sectorIndex, uint32_t deckIndex, float xOffset,
			std::string* diagnostic = nullptr) const;

		CreateObjectResult addSectorMarker(uint32_t sectorIndex, uint32_t deckIndex, float xOffset,
			uint32_t* vertexIdentifier = nullptr);

		bool removeSectorMarker(uint32_t sectorIndex, uint32_t objectIndex);

		// Plans are side-effect free. Applying a plan reconstructs the authored
		// structure atomically and leaves the simulation paused.
		LocationEditPlan planResizeLocation(uint32_t sectorIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, uint32_t decksHigh) const;

		LocationEditPlan planRemoveLocation(uint32_t sectorIndex) const;

		// A Facade is occupiable, so deleting it goes through the same cascade a
		// Room deletion plays: the plan names the Agents inside and every hosted
		// object which goes with it, and the apply rebuilds the rest of the
		// Building around the removal (ticket #53).  Resizing stays out of
		// scope: planResizeLocation keeps refusing a Facade.
		LocationEditPlan planRemoveFacade(uint32_t sectorIndex) const;

		uint32_t applyLocationEdit(LocationEditPlan const& plan);

		// A Background exists only to be looked into, so taking it away takes the
		// Windows looking into it with it.  Deleting a Background, moving it, or
		// resizing it all uncover whatever back cells the new footprint no longer
		// covers, and every Window which loses what it looks into is named in the
		// plan's consequences before anything is applied.  A plan which uncovers
		// nothing needs no confirmation and applies silently.
		LocationEditPlan planRemoveBackground(uint32_t sectorIndex) const;

		LocationEditPlan planResizeBackground(uint32_t sectorIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, uint32_t decksHigh) const;

		uint32_t applyBackgroundEdit(LocationEditPlan const& plan);

		// Recolour a Background in place. Colour is the only thing a Background owns,
		// so the live Sector and its authored ConstructionType::Background record are
		// patched together: no rebuild, no cascade, and nothing else in the Building
		// reads a Background's colour. Returns false, with a diagnostic when one is
		// asked for, if the Sector is not a Background or has no authored record.
		bool setBackgroundColour(uint32_t sectorIndex, BackgroundColour const& colour,
			std::string* diagnostic = nullptr);

		// Recolour a Facade in place, the same patch shape setBackgroundColour uses:
		// the live Sector and its authored ConstructionType::Facade record are updated
		// together, so a save writes the new colour and a reload replays it. A
		// Facade's colour feeds only its own rendering, so this needs no plan and no
		// cascade. Returns false, with a diagnostic when one is asked for, if the
		// Sector is not a Facade or has no authored record.
		bool setFacadeColour(uint32_t sectorIndex, BackgroundColour const& colour,
			std::string* diagnostic = nullptr);

		LiftEditPlan planResizeLift(uint32_t sectorIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, uint32_t decksHigh) const;

		LiftEditPlan planRemoveLift(uint32_t sectorIndex) const;

		LiftEditPlan planRemoveLiftStop(uint32_t sectorIndex, uint32_t stopIndex) const;

		uint32_t applyLiftEdit(LiftEditPlan const& plan);

		ShuttleEditPlan planResizeShuttle(uint32_t sectorIndex, uint32_t x,
			uint32_t y, uint32_t cellsWide) const;

		ShuttleEditPlan planRemoveShuttle(uint32_t sectorIndex) const;

		ShuttleEditPlan planRemoveShuttleStop(uint32_t sectorIndex, uint32_t stopIndex) const;

		ShuttleEditPlan planAddShuttleStop(uint32_t sectorIndex, uint32_t stopOffset) const;

		uint32_t applyShuttleEdit(ShuttleEditPlan const& plan);

		LadderEditPlan planResizeLadder(uint32_t sectorIndex, uint32_t x,
			uint32_t y, CreateLadderOptions const& options) const;

		LadderEditPlan planRemoveLadder(uint32_t sectorIndex) const;

		uint32_t applyLadderEdit(LadderEditPlan const& plan);

		StairwellEditPlan planResizeStairwell(uint32_t sectorIndex, uint32_t x,
			uint32_t y, CreateStairwellOptions const& options) const;

		StairwellEditPlan planRemoveStairwell(uint32_t sectorIndex) const;

		uint32_t applyStairwellEdit(StairwellEditPlan const& plan);

		StaircaseEditPlan planResizeStaircase(uint32_t sectorIndex, uint32_t x,
			uint32_t y, CreateStaircaseOptions const& options) const;
		StaircaseEditPlan planRemoveStaircase(uint32_t sectorIndex) const;
		uint32_t applyStaircaseEdit(StaircaseEditPlan const& plan);

		ObjectMovePlan planMoveSectorObject(uint32_t sectorIndex, uint32_t objectIndex,
			uint32_t x, uint32_t y) const;

		// Windows resize from any edge. The resulting plan uses the same atomic replay
		// path as movement, preserving authored options while validating the complete
		// new footprint against normal Window placement rules.
		ObjectMovePlan planResizeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex,
			uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const;

		// Regular Doors resize horizontally between one and two cells. The plan uses
		// the same atomic replay path as movement, preserving authored options while
		// validating the new span against normal Door placement rules. Lift and
		// Shuttle landing doors are managed by their transport and refuse to resize.
		ObjectMovePlan planResizeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex,
			uint32_t x, uint32_t y, uint32_t cellsWide) const;

		std::shared_ptr<const SectorObject> applyObjectMove(ObjectMovePlan const& plan);

		bool canRemoveLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side,
			std::string* diagnostic = nullptr) const;

		bool canAddLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side,
			std::string* diagnostic = nullptr) const;

		void removeLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side);

		void addLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side);

		void finishBuild();

		// Runtime structural editing protocol. Pausing deterministically cancels
		// active edge transactions while retaining route destinations for the new
		// graph. A failed rebuild is atomic at the graph boundary and cannot resume.
		// Pause and resume are the edit/simulation boundary and live here, on the
		// editing side of it; the simulation-side work they drive - the teardown,
		// the paused route intents and the boundary events - lives in
		// SimulationCoordinator (ADR 0004 stage 5).
		void pauseSimulation();

		bool rebuildTraversalTopology();

		bool resumeSimulation();

		bool isSimulationPaused() const { return mSimulationPaused; }

		bool isTraversalTopologyDirty() const { return mTopologyDirty; }

		bool isTraversalTopologyValid() const { return mTopologyValid; }

		uint64_t getTopologyGeneration() const { return mTopologyGeneration; }

		std::string const& getTopologyDiagnostic() const { return mTopologyDiagnostic; }

		std::shared_ptr<const Sector> getSectorAtPosition(uint32_t layerIndex, float x, float y) const;

		Agent* getAgentAtPosition(uint32_t layerIndex, float x, float y) const;

		std::shared_ptr<const Object> getObjectAtPosition(uint32_t layerIndex, float x, float y,
			std::shared_ptr<const SectorObject>* sectorObject = nullptr) const;

		// Building-owned replacement APIs. Callers retain typed IDs, not ownership.
		// Agent lifecycle - creation, placement, removal, lookup, id resolution,
		// waking, and traversal-ownership release - lives in SimulationCoordinator
		// (ADR 0004); every Agent entry point below forwards to it, as does every
		// InteractionPoint, InteractionRequest and DeviceOperation entry point.
		AgentId createAgent(std::string const& name, uint32_t sectorId, uint32_t deckOffset, float xOffset);

		AgentId createAgent(std::string const& name, uint32_t sectorId);

		EntityLookup<Agent> lookupAgent(AgentId id);

		EntityLookup<Agent const> lookupAgent(AgentId id) const;

		EntityRemovalResult removeAgent(AgentId id);

		InteractionPointId createInteractionPoint(std::string const& name);

		InteractionPointId createInteractionPoint(std::string const& name, SectorId sector,
			Vector2 position, float reach, float durationSeconds, std::vector<InteractionBinding> bindings);

		EntityLookup<InteractionPoint> lookupInteractionPoint(InteractionPointId id);

		EntityLookup<InteractionPoint const> lookupInteractionPoint(InteractionPointId id) const;

		EntityRemovalResult removeInteractionPoint(InteractionPointId id);

		InteractionRequestId requestInteraction(InteractionPointId point, AgentId actor);

		EntityLookup<InteractionRequest const> lookupInteractionRequest(InteractionRequestId id) const;

		bool cancelInteraction(InteractionRequestId id);

		DeviceOperationId createDeviceOperation(std::string const& name, AgentId requester);

		EntityLookup<DeviceOperation> lookupDeviceOperation(DeviceOperationId id);

		EntityLookup<DeviceOperation const> lookupDeviceOperation(DeviceOperationId id) const;

		bool cancelDeviceOperation(DeviceOperationId id, AgentId requester);

		EntityRemovalResult removeDeviceOperation(DeviceOperationId id);

		TraversalResourceId createTraversalResource(std::string const& name);

		TraversalResourceId createDoorTraversalResource(std::string const& name,
			std::shared_ptr<Door> door, DoorActivationMode mode, float holdOpenSeconds);

		TraversalResourceId createWindowTraversalResource(std::string const& name,
			std::shared_ptr<Window> window);

		TraversalResourceId createLadderTraversalResource(std::string const& name,
			std::shared_ptr<Ladder> ladder, SectorId ladderSector,
			uint32_t directionalBatchLimit);

		TraversalResourceId createLiftTraversalResource(std::string const& name,
			std::shared_ptr<Lift> lift, SectorId liftSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createOpenPlatformLiftTraversalResource(std::string const& name,
			std::shared_ptr<Lift> lift, SectorId locationSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1,
			float stopDurationSeconds = CORE_PLATFORM_LIFT_STOP_DURATION);

		TraversalResourceId createShuttleTraversalResource(std::string const& name,
			std::shared_ptr<Shuttle> shuttle, SectorId shuttleSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createForceBridgeTraversalResource(std::string const& name,
			std::shared_ptr<ForceBridge> forceBridge);

		TraversalResourceId createStairwellTraversalResource(std::string const& name,
			std::shared_ptr<Stairwell> stairwell, SectorId stairwellSector,
			uint32_t capacity, uint32_t directionalBatchLimit);

		// Defines one physical waiting lane. The direction is normalized and
		// positions are generated at agent-safe spacing from origin through extent.
		// A door accepts at most two lanes, one per source sector.
		bool configureDoorQueueLane(TraversalResourceId resource, SectorId sector,
			Vector2 origin, Vector2 direction, float extent);

		// Configures independent threshold slots. Reconfiguration is rejected
		// while a crossing owns a lane.
		bool configureDoorCrossingLanes(TraversalResourceId resource, uint32_t laneCount);

		// External systems hold doors open through the same scoped safety
		// protocol. The lease protocol lives in SimulationCoordinator (ADR 0004);
		// these forward.
		DoorOpenLeaseId acquireDoorOpenLease(TraversalResourceId resource,
			DoorOpenLeaseKind kind = DoorOpenLeaseKind::ExternalHoldOpen);

		bool releaseDoorOpenLease(TraversalResourceId resource, DoorOpenLeaseId lease);

		// Sensors report facts; only the traversal coordinator issues door actions.
		bool setDoorSensorObservation(TraversalResourceId resource, DoorSensorId sensor,
			DoorSensorObservation observation);

		// Disabling rejects future admission but never revokes active crossings.
		bool setTraversalResourceEnabled(TraversalResourceId resource, bool enabled);

		// Registers a physical control as applicable from its interaction point's sector.
		// Controlled traversal never falls back to operating the resource directly.
		bool addTraversalControl(TraversalResourceId resource, InteractionPointId control);

		EntityLookup<TraversalResource> lookupTraversalResource(TraversalResourceId id);

		EntityLookup<TraversalResource const> lookupTraversalResource(TraversalResourceId id) const;

		// Resolves physical geometry to the resource that owns its agent queue.
		// Transport landing doors resolve to their vehicle coordinator.
		TraversalResourceId getTraversalResourceId(Object const* object) const;

		EntityRemovalResult removeTraversalResource(TraversalResourceId id);

		EntityLookup<TraversalRequest const> lookupTraversalRequest(TraversalRequestId id) const;

		TraversalWaitingPolicy const& getTraversalWaitingPolicy() const;

		void setTraversalWaitingPolicy(TraversalWaitingPolicy policy);

		// Pure route-cost query: it creates no ticket, operation, reservation, or permit.
		float estimateTraversalDelay(TraversalResourceId resource, SectorId sourceSector) const;

		// Wakes every Agent the Building owns. Forwards to SimulationCoordinator,
		// where the Agent lifecycle lives (ADR 0004).
		void wakeAllAgents();

		// Restore authored Agent routes/positions and reconstruct all simulated
		// objects in their configured initial state.
		void resetSimulation();

		// Clear the modified state of the Building and every Agent it owns, so
		// isModified() reports clean.  Only call this once a save has fully
		// succeeded; a save that fails must leave the dirty state intact.
		void markSaved();

		// Persist the Building to filepath.  The clean-state transition happens
		// only after the file write has completely succeeded; any open, write,
		// flush, close, or replacement error throws and leaves the Building and
		// its Agents exactly as dirty as they were before the attempt.
		void saveTo(std::string const& filepath);

		// Rendering supplies elapsed wall time here.  It is accumulated and only
		// whole fixed simulation ticks are executed.
		//
		// The tick pipeline itself - the accumulator, the six simulation phases,
		// the per-phase lift, shuttle and door advancement, tick event
		// publication, the simulation clock and event consumption, and every
		// snapshot builder - lives in SimulationCoordinator (ADR 0004 stage 5);
		// every entry point below forwards to it.
		void update(float elapsedSeconds);

		// Headless deterministic seam.  These methods never use render timing.
		void advanceTick();

		void advanceTicks(uint64_t count);

		static constexpr float getFixedTimestep()
		{
			return 1.0f / 60.0f;
		}

		uint64_t getSimulationTick() const;

		SimulationPhase getCurrentSimulationPhase() const;

		AgentId getAgentId(Agent const* agent) const;

		SimulationSnapshot getSimulationSnapshot() const;

		std::vector<SimulationEvent> consumeSimulationEvents();
	};

} // core
