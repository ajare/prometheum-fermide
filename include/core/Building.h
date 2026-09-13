#pragma once

#include <string>
#include <array>
#include <vector>
#include <memory>
#include <map>


#include "core/Defines.h"
#include "core/Layer.h"
#include "core/Location.h"
#include "core/SectorType.h"
#include "core/Door.h"
#include "core/Window.h"
#include "core/Button.h"
#include "core/Graph.h"
#include "core/Log.h"
#include "core/Simulation.h"
#include "core/Coordination.h"
#include "core/EntityRegistry.h"
#include "core/Serializable.h"


namespace core
{

	class Building : public Serializable
	{
		friend class Agent;
		friend class Graph;

	public:

		struct CreateObjectResult
		{
			uint32_t index{ ~0u };
			SectorObjectType type{ SectorObjectType::None };
			std::shared_ptr<Sector> sector;
			InteractionPointId interactionPoint;
		};

		struct CreateDoorOptions
		{
			uint32_t width{ 1 };
			bool controls[2] = { false, false };
			DoorActivationMode activationMode{ DoorActivationMode::Manual };
			float holdOpenSeconds{ CORE_DOOR_STAY_OPEN_TIME };
			// Zero derives one lane per cell of usable threshold width.
			uint32_t crossingLanes{ 0 };
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
			uint32_t controlCount{ 0 };
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
			float agentSpacing{ CORE_AGENT_MAX_HEIGHT };
			uint32_t directionalBatchLimit{ 4 };
		};

		struct CreateLadderResult
		{
			CreateObjectResult ladder;
			CreateObjectResult controls[2];
			TraversalResourceId traversalResource;
		};

		struct CreateStaircaseOptions
		{
			uint32_t decksHigh;
			int mountSide;
			// Zero preserves ordinary, unconstrained bidirectional stairs.
			uint32_t directionalCapacity{ 0 };
			uint32_t directionalBatchLimit{ 4 };
		};

		struct CreateStaircaseResult
		{
			uint32_t sectorIndex{ ~0u };
			TraversalResourceId traversalResource;
		};

		struct CreateLiftOptions
		{
			uint32_t cellsWide{ 1 };
			std::vector<uint32_t> stopOffsets;
			uint32_t capacity{ 1 };
			float minimumDwellSeconds{ CORE_LIFT_DOOR_PAUSE_TIME };
			float maximumBoardingSeconds{ CORE_DOOR_STAY_OPEN_TIME };
			uint32_t initialStop{ 0 };
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
		};

		struct CreateShuttleResult
		{
			CreateObjectResult shuttle;
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
			std::string diagnostic;
		};

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

	public:

		static CreateDoorOptions ManualDoor1Options, RemoteControlledDoor1Options, UnavailableDoor1Options;

		static CreateDoorOptions ManualDoor2Options, RemoteControlledDoor2Options, UnavailableDoor2Options;

	private:

		std::string mName;

		uint32_t mCellsWide, mDecksHigh;

		std::array<std::shared_ptr<Layer>, CORE_NUM_LAYERS> mLayers;

		std::vector<std::shared_ptr<Sector>> mSectors;


		std::shared_ptr<Graph> mGraph;

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
			ObjectTombstone
		};

		// Compact tagged command storage. Field meanings are determined by type and
		// kept private so the public model is not coupled to its YAML representation.
		struct ConstructionRecord
		{
			ConstructionType type{};
			std::string name;
			uint32_t a{ 0 }, b{ 0 }, c{ 0 }, d{ 0 }, e{ 0 }, f{ 0 }, g{ 0 };
			int32_t i{ 0 }, j{ 0 };
			float x{ 0.0f }, y{ 0.0f };
			bool p{ false }, q{ false };
			std::vector<uint32_t> values;
		};

		std::vector<ConstructionRecord> mConstructionRecords;
		bool mDeserializingConstruction{ false };

	private:

		bool childrenModified() const override;

		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;

		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		void recordConstruction(ConstructionRecord record);

		void applyConstructionRecord(ConstructionRecord const& record);

		bool prepareLocationEdit(LocationEditPlan const& plan,
			std::vector<ConstructionRecord>& records, uint32_t& newSectorIndex,
			std::string& diagnostic) const;

		bool prepareObjectMove(ObjectMovePlan const& plan,
			std::vector<ConstructionRecord>& records, uint32_t& newObjectIndex,
			std::string& diagnostic) const;

		void resetForDeserialization(std::string name, uint32_t cellsWide, uint32_t decksHigh);

		void validateCellOccupied(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellUnoccupied(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellIsInSector(std::string const& caller, uint32_t x, uint32_t y, std::shared_ptr<const Sector> sector) const;

		void validateCellHasObject(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellHasNoObject(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellIsType(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, SectorType sectorType) const;

		void validateCellHasDoor(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellHasNoDoor(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellHasPhysicalControl(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, int side) const;

		void validateCellHasNoPhysicalControl(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, int side) const;

		void validateCellHasNoFloorType(std::string const& caller, std::string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellTraversableOnFoot(std::string const& caller, std::string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateLayer(std::string const& caller, uint32_t layerIndex) const;

		void validateBounds(std::string const& caller, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const;

		void validateLayerSpace(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const;

		void validateObjectAllowedInSector(std::string const& caller, SectorObjectType type, uint32_t sectorIndex) const;

		void validateSpaceOnlyInOneSector(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh) const;

		void validateSectorDoorOptions(std::string const& caller, CreateDoorOptions const& options) const;

		void validateSectorForceBridgeOptions(std::string const& caller, CreateForceBridgeOptions const& options) const;

		void validateSectorLadderOptions(std::string const& caller, CreateLadderOptions const& options) const;

		void validateLiftOptions(std::string const& caller, CreateLiftOptions const& options) const;

		void validateShuttleOptions(std::string const& caller, CreateShuttleOptions const& options) const;

		void beginStructuralEdit(std::string const& operation);

		void cancelTraversalForTopologyRebuild(Agent& agent);

		void validateTraversalTopology(Graph const& graph) const;

		void restorePausedPathIntents();

		void publishTopologyEvent(SimulationEventType type, std::string diagnostic = {});

		std::shared_ptr<Sector> _getSector(uint32_t index);

		std::shared_ptr<Layer> getLayer(uint32_t layerIndex);

		uint32_t createLocation(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, bool isCorridor);

		uint32_t createLadder(uint32_t x, uint32_t y, CreateLadderOptions const& options);

		uint32_t createStaircase(uint32_t x, uint32_t y, uint32_t decksHigh, int mountSide);

		CreateObjectResult createLift(uint32_t x, uint32_t y, uint32_t cellsWide, std::vector<uint32_t> const& stopOffsets);

		CreateObjectResult createShuttle(uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t numCars, uint32_t carWidth, std::vector<uint32_t> const& stopOffsets);

		CreateObjectResult createDoor(uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createWindow(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createBulkheadDoor(uint32_t layerIndex, uint32_t x, uint32_t y, int side);

		CreateObjectResult createPhysicalControl(std::string const& name, uint32_t layerIndex, uint32_t x, uint32_t y, int side, uint32_t flags, uint32_t* vertexIdentifier = nullptr);
		void bindPhysicalControl(CreateObjectResult& control, InteractionPointId point);

		CreateObjectResult createWalkway(uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createMarker(uint32_t layerIndex, uint32_t x, uint32_t y, float xOffset, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createForceBridge(uint32_t layerIndex, uint32_t x, uint32_t y, CreateForceBridgeOptions const& options);

		CreateObjectResult createLadderSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLadderOptions const& options, uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createPlatformLiftSectorObject(uint32_t layerIndex, uint32_t x, uint32_t y, CreateLiftOptions const& options, uint32_t* vertexIdentifier = nullptr);

		CreateDoorResult _addSectorDoor(uint32_t y, uint32_t x, CreateDoorOptions const& options,
			bool controlsAreExternallyBound = false);

		CreateObjectResult _createSectorButton(std::string const& name, std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t flags, uint32_t* index = nullptr);

		CreateObjectResult _createDoorButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t flags, uint32_t* index = nullptr);

		CreateObjectResult _createBulkheadDoorButton(std::shared_ptr<const Sector> sector, uint32_t y, int side, uint32_t* index = nullptr);

		CreateObjectResult _createForceBridgeButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index = nullptr);

		CreateObjectResult _createLadderButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, int side, uint32_t flags, uint32_t* index = nullptr);

		CreateObjectResult _createPlatformLiftButton(std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags, uint32_t* index = nullptr);

		uint32_t addLocation(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight, bool isCorridor);

		void buildGraph();

		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId, uint32_t deckOffset, float xOffset);

		AgentId addOwnedAgentToSector(std::unique_ptr<Agent> agent, uint32_t sectorId);

		AgentSnapshot makeAgentSnapshot(Agent const* agent) const;

		InteractionPointSnapshot makeInteractionPointSnapshot(InteractionPointId id, InteractionPoint const& point) const;

		InteractionRequestSnapshot makeInteractionRequestSnapshot(InteractionRequestId id, InteractionRequest const& request) const;

		DeviceOperationSnapshot makeDeviceOperationSnapshot(DeviceOperationId id, DeviceOperation const& operation) const;

		DeviceOperationId findOrCreateDeviceOperation(DeviceCommand const& command, AgentId requester);

		void advanceDeviceOperations();

		void allocateInteractions();

		void moveInteractions(float frameTime);

		void tryPressUpcomingDoorButton(Agent& agent, Vector2 const& movementStart,
			Vector2 const& movementEnd);

		void pressPhysicalControl(InteractionPointId point);

		void updateInteractionResults();

		void detachInteractionRequester(InteractionRequest& request);

		InteractionRequestId requestInteractionForTraversal(InteractionPointId point, AgentId actor);

		InteractionRequestId requestInteractionWhilePassing(InteractionPointId point, AgentId actor);

		void allocateRemoteDoorPreparation(TraversalRequestId requestId, TraversalResource& resource);

		void allocateExtensiblePreparation(TraversalRequestId requestId, TraversalResource& resource);

		void attachDoorQueueTicket(TraversalRequestId requestId, TraversalResource& resource);

		void refreshDoorQueuePositions(TraversalResource& resource);

		void updateTraversalProgressAndTimeouts();

		void expireTraversalPermit(TraversalPermitId permitId);

		void tryGrantDoorQueue(TraversalResource& resource);

		bool isLadderAdmission(TraversalRequest const& request, TraversalResource const& resource) const;

		void attachLadderAdmissionRequest(TraversalRequestId requestId, TraversalResource& resource);

		void tryGrantLadderAdmissions(TraversalResource& resource);

		void releaseLadderAdmission(TraversalRequestId requestId, TraversalResource& resource);

		void releaseLadderOccupancy(AgentId agentId, TraversalResource& resource);

		DoorOpenLeaseId acquireDoorOpenLease(TraversalResource& resource,
			DoorOpenLeaseKind kind, TraversalRequestId request = {});

		bool releaseDoorOpenLease(TraversalResource& resource, DoorOpenLeaseId lease);

		void advanceDoorResources();

		void advanceLiftResources();

		void allocateLiftTraversal(TraversalRequestId requestId, TraversalResource& resource);

		void allocateOpenPlatformLiftTraversal(TraversalRequestId requestId, TraversalResource& resource);

		uint32_t findLiftStop(TraversalResource const& resource, Vector2 const& endpoint) const;

		uint32_t findAgentLiftDestination(Agent const& agent, TraversalResource const& resource) const;

		bool liftHasDisembarkDemand(TraversalResource const& resource, uint32_t stop) const;

		void addLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner);

		void removeLiftStopRequest(TraversalResource& resource, uint32_t stop, AgentId owner);

		uint32_t chooseNextLiftStop(TraversalResource& resource) const;

		bool isLiftBoardingDirectionCompatible(TraversalResource& resource,
			uint32_t originStop, uint32_t destinationStop);

		void releaseLiftAdmission(TraversalRequestId requestId, TraversalResource& resource);

		bool retargetShuttleDoorTraversal(TraversalRequestId requestId,
			TraversalResource& coordinator, ShuttleDoor const& door);

		bool assignShuttleBoardingDoor(TraversalRequestId requestId,
			TraversalResource& coordinator, uint32_t stop);

		bool assignShuttleDisembarkDoor(TraversalRequestId requestId,
			TraversalResource& coordinator, uint32_t stop);

		uint32_t findShuttlePassengerCarriage(TraversalResource const& resource,
			AgentId passenger) const;

		void requestLiftPassengerSafeExit(AgentId passenger, TraversalFailureReason reason);

		void assignLiftSafeExitPaths(TraversalResource& resource);

		bool replaceOnboardLiftDestination(Agent& agent, std::shared_ptr<Path> const& path,
			uint32_t& sourceNode);

		void releaseDoorQueueOwnership(TraversalRequestId requestId, TraversalResource& resource);

		TraversalResourceSnapshot makeTraversalResourceSnapshot(TraversalResourceId id, TraversalResource const& resource) const;

		TraversalRequestSnapshot makeTraversalRequestSnapshot(TraversalRequestId id, TraversalRequest const& request) const;

		TraversalPermitSnapshot makeTraversalPermitSnapshot(TraversalPermitId id, TraversalPermit const& permit) const;

		TraversalRequestId createTraversalRequest(Agent const& agent, std::shared_ptr<const Edge> const& edge,
			std::shared_ptr<const Vertex> const& source, std::shared_ptr<const Vertex> const& destination);

		TraversalPermitId grantTraversalRequest(TraversalRequestId requestId);

		void allocateTraversalRequest(TraversalRequestId requestId,
			std::shared_ptr<const Edge> const& edge, std::shared_ptr<const Vertex> const& destination);

		void denyTraversalRequest(TraversalRequestId requestId,
			TraversalFailureReason reason = TraversalFailureReason::None);

		bool commitTraversal(Agent& agent, TraversalRequestId requestId, TraversalPermitId permitId,
			std::shared_ptr<const Vertex> const& destination);

		void cancelTraversal(TraversalRequestId requestId, TraversalPermitId permitId,
			bool requestSafeTransportExit = true);

		void releaseTraversal(TraversalRequestId requestId, TraversalPermitId permitId);

		void runSimulationPhase(SimulationPhase phase);

		void publishTickEvents(SimulationSnapshot const& before);

	public:

		Building(std::string const& name, uint32_t cellsWide, uint32_t decksHigh);

		virtual ~Building();

		std::string const& getName() const;

		uint32_t getCellsWide() const;

		uint32_t getDecksHigh() const;

		uint32_t getNumSectors() const;

		std::shared_ptr<const Layer> getLayer(uint32_t layerIndex) const;

		std::shared_ptr<const Sector> getSector(uint32_t index) const;

		std::vector<std::shared_ptr<const Sector>> getSectorsInBounds(uint32_t layerIndex, float x, float y, float width, float height) const;

		std::vector<std::shared_ptr<const Sector>> getSectors(uint32_t layerIndex) const;

		std::shared_ptr<const Graph> getGraph() const;

		Log const& getBuildLog() const;

		// Sector types
		uint32_t addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh = 1);

		uint32_t addRoom(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh, float topDeckHeight = CORE_ROOM_MAX_HEIGHT);
	
		CreateLadderResult addLadder(uint32_t y, uint32_t x, CreateLadderOptions const& options);

		uint32_t addStaircase(uint32_t y, uint32_t x, uint32_t decksHigh, int mountSide);

		CreateStaircaseResult addStaircase(uint32_t y, uint32_t x,
			CreateStaircaseOptions const& options);

		CreateLiftResult addLift(uint32_t y, uint32_t x, CreateLiftOptions const& options);

		CreateShuttleResult addShuttle(uint32_t y, uint32_t x, uint32_t cellsWide, CreateShuttleOptions const& options);

		// Sector object types
		// Corridor doors are the constrained authoring form exposed by the object palette.
		bool canAddCorridorDoor(uint32_t y, uint32_t x,
			std::string* diagnostic = nullptr) const;

		CreateDoorResult addSectorDoor(uint32_t y, uint32_t x, CreateDoorOptions const& options = {});

		// Adds a physical open control in the selected Door's owning Location.
		// Placement against the left or right edge is derived from the Door and Location geometry.
		CreateObjectResult addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex);

		bool removeSectorDoor(uint32_t sectorIndex, uint32_t objectIndex);

		bool canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide = 1, uint32_t decksHigh = 1,
			std::string* diagnostic = nullptr) const;

		uint32_t addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t decksHigh);

		CreateWindowResult addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t decksHigh, CreateWindowOptions const& options);

		bool removeSectorWindow(uint32_t sectorIndex, uint32_t objectIndex);

		CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			int side, CreateBulkheadDoorOptions const& options = {});

		CreateObjectResult addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset);

		CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateForceBridgeOptions const& options = {});

		CreateLadderResult addSectorLadder(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateLadderOptions const& options);

		CreatePlatformLiftResult addSectorPlatformLift(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset, CreateLiftOptions const& options);

		void addSectorWalkway(uint32_t sectorIndex, uint32_t deckIndex, uint32_t xOffset);

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

		uint32_t applyLocationEdit(LocationEditPlan const& plan);

		ObjectMovePlan planMoveSectorObject(uint32_t sectorIndex, uint32_t objectIndex,
			uint32_t x, uint32_t y) const;

		std::shared_ptr<const SectorObject> applyObjectMove(ObjectMovePlan const& plan);

		void removeLocationWall(uint32_t sectorIndex, uint32_t deckIndex, int side);

		void finishBuild();

		// Runtime structural editing protocol. Pausing deterministically cancels
		// active edge transactions while retaining route destinations for the new
		// graph. A failed rebuild is atomic at the graph boundary and cannot resume.
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
			std::shared_ptr<Ladder> ladder, SectorId ladderSector, float agentSpacing,
			uint32_t directionalBatchLimit);

		TraversalResourceId createLiftTraversalResource(std::string const& name,
			std::shared_ptr<Lift> lift, SectorId liftSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createOpenPlatformLiftTraversalResource(std::string const& name,
			std::shared_ptr<Lift> lift, SectorId locationSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createShuttleTraversalResource(std::string const& name,
			std::shared_ptr<Shuttle> shuttle, SectorId shuttleSector, std::vector<LiftStop> stops,
			uint32_t capacity = 1, float minimumDwellSeconds = CORE_LIFT_DOOR_PAUSE_TIME,
			float maximumBoardingSeconds = CORE_DOOR_STAY_OPEN_TIME);

		TraversalResourceId createForceBridgeTraversalResource(std::string const& name,
			std::shared_ptr<ForceBridge> forceBridge);

		TraversalResourceId createStaircaseTraversalResource(std::string const& name,
			std::shared_ptr<Staircase> staircase, SectorId staircaseSector,
			uint32_t capacity, uint32_t directionalBatchLimit);

		// Defines one physical waiting lane. The direction is normalized and
		// positions are generated at agent-safe spacing from origin through extent.
		// A door accepts at most two lanes, one per source sector.
		bool configureDoorQueueLane(TraversalResourceId resource, SectorId sector,
			Vector2 origin, Vector2 direction, float extent);

		// Configures independent threshold slots. Reconfiguration is rejected
		// while a crossing owns a lane.
		bool configureDoorCrossingLanes(TraversalResourceId resource, uint32_t laneCount);

		// External systems hold doors open through the same scoped safety protocol.
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

		EntityLookup<TraversalPermit const> lookupTraversalPermit(TraversalPermitId id) const;

		TraversalWaitingPolicy const& getTraversalWaitingPolicy() const;

		void setTraversalWaitingPolicy(TraversalWaitingPolicy policy);

		// Pure route-cost query: it creates no ticket, operation, reservation, or permit.
		float estimateTraversalDelay(TraversalResourceId resource, SectorId sourceSector) const;

		void wakeAllAgents();

		// Rendering supplies elapsed wall time here.  It is accumulated and only
		// whole fixed simulation ticks are executed.
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
