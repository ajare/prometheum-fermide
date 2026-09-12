#pragma once

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/EdgeType.h"
#include "core/EntityId.h"
#include "core/Vector2.h"


namespace core
{
	class Building;
	class Door;

	enum struct DoorActivationMode
	{
		Automatic,
		Manual,
		RemoteControlled,
		Unavailable
	};

	enum struct DeviceCommandType
	{
		SetSectorLights,
		OpenDoor
	};

	// A command says which state is desired. It is intentionally not a toggle:
	// retries and equivalent requests are therefore idempotent and coalescible.
	struct DeviceCommand
	{
		DeviceCommandType type{ DeviceCommandType::SetSectorLights };
		SectorId target;
		bool desiredState{ false };
		TraversalResourceId traversalResource;

		friend bool operator==(DeviceCommand const&, DeviceCommand const&) = default;
	};

	enum struct InteractionBindingRequirement
	{
		Required,
		BestEffort
	};

	struct InteractionBinding
	{
		DeviceCommand command;
		InteractionBindingRequirement requirement{ InteractionBindingRequirement::Required };
	};

	enum struct InteractionResult
	{
		Pending,
		Succeeded,
		SucceededWithBestEffortFailure,
		Failed,
		Cancelled
	};

	class InteractionPoint
	{
		friend class Building;

		std::string mName;
		SectorId mSector;
		Vector2 mPosition;
		float mReach{ 0.25f };
		uint64_t mDurationTicks{ 1 };
		std::vector<InteractionBinding> mBindings;
		std::vector<InteractionRequestId> mQueue;
		InteractionRequestId mActiveRequest;
		uint64_t mInteractionTicksRemaining{ 0 };

		explicit InteractionPoint(std::string name)
			: mName(std::move(name))
		{
		}

		InteractionPoint(std::string name, SectorId sector, Vector2 position, float reach,
			uint64_t durationTicks, std::vector<InteractionBinding> bindings)
			: mName(std::move(name))
			, mSector(sector)
			, mPosition(position)
			, mReach(reach)
			, mDurationTicks(durationTicks)
			, mBindings(std::move(bindings))
		{
		}

	public:
		InteractionPoint(InteractionPoint const&) = delete;
		InteractionPoint& operator=(InteractionPoint const&) = delete;

		std::string const& getName() const { return mName; }
		SectorId getSector() const { return mSector; }
		Vector2 const& getPosition() const { return mPosition; }
		float getReach() const { return mReach; }
		uint64_t getDurationTicks() const { return mDurationTicks; }
		InteractionRequestId getActiveRequest() const { return mActiveRequest; }
	};

	enum struct DeviceOperationState
	{
		Pending,
		Running,
		Succeeded,
		Failed,
		Cancelled
	};

	class DeviceOperation
	{
		friend class Building;

		std::string mName;
		AgentId mRequester;
		std::set<AgentId> mRequesters;
		DeviceCommand mCommand;
		bool mHasCommand{ false };
		bool mActivated{ false };
		DeviceOperationState mState{ DeviceOperationState::Pending };

		DeviceOperation(std::string name, AgentId requester)
			: mName(std::move(name)), mRequester(requester), mRequesters{ requester }
		{
		}

		DeviceOperation(std::string name, AgentId requester, DeviceCommand command)
			: mName(std::move(name)), mRequester(requester), mRequesters{ requester },
			  mCommand(command), mHasCommand(true)
		{
		}

	public:
		DeviceOperation(DeviceOperation const&) = delete;
		DeviceOperation& operator=(DeviceOperation const&) = delete;

		std::string const& getName() const { return mName; }
		AgentId getRequester() const { return mRequester; }
		std::set<AgentId> const& getRequesters() const { return mRequesters; }
		DeviceCommand const& getCommand() const { return mCommand; }
		bool hasCommand() const { return mHasCommand; }
		DeviceOperationState getState() const { return mState; }
		void setState(DeviceOperationState state) { mState = state; }
	};

	class InteractionRequest
	{
		friend class Building;

		InteractionPointId mPoint;
		AgentId mActor;
		InteractionResult mResult{ InteractionResult::Pending };
		std::vector<std::pair<DeviceOperationId, InteractionBindingRequirement>> mOperations;

		InteractionRequest(InteractionPointId point, AgentId actor)
			: mPoint(point), mActor(actor)
		{
		}

	public:
		InteractionRequest(InteractionRequest const&) = delete;
		InteractionRequest& operator=(InteractionRequest const&) = delete;
		InteractionPointId getPoint() const { return mPoint; }
		AgentId getActor() const { return mActor; }
		InteractionResult getResult() const { return mResult; }
		std::vector<std::pair<DeviceOperationId, InteractionBindingRequirement>> const& getOperations() const { return mOperations; }
	};

	class TraversalResource
	{
		friend class Building;
		std::string mName;
		std::shared_ptr<Door> mDoor;
		DoorActivationMode mDoorActivationMode{ DoorActivationMode::Unavailable };
		uint64_t mHoldOpenTicks{ 0 };
		std::set<TraversalRequestId> mOpenLeases;
		explicit TraversalResource(std::string name) : mName(std::move(name)) {}
		TraversalResource(std::string name, std::shared_ptr<Door> door,
			DoorActivationMode mode, uint64_t holdOpenTicks)
			: mName(std::move(name)), mDoor(std::move(door)),
			  mDoorActivationMode(mode), mHoldOpenTicks(holdOpenTicks) {}
	public:
		TraversalResource(TraversalResource const&) = delete;
		TraversalResource& operator=(TraversalResource const&) = delete;
		std::string const& getName() const { return mName; }
		bool isDoor() const { return mDoor != nullptr; }
		DoorActivationMode getDoorActivationMode() const { return mDoorActivationMode; }
	};

	enum struct TraversalRequestState { Pending, Granted, Denied, Cancelled, Committed };

	class TraversalRequest
	{
		friend class Building;
		AgentId mOwner;
		EdgeType mEdgeType;
		SectorId mSourceSector;
		SectorId mDestinationSector;
		Vector2 mSourceEndpoint;
		Vector2 mDestinationEndpoint;
		TraversalRequestState mState{ TraversalRequestState::Pending };
		bool mPreparationRequested{ false };
		TraversalResourceId mResource;
		DeviceOperationId mPreparationOperation;
		TraversalPermitId mPermit;
		TraversalRequest(AgentId owner, EdgeType edgeType, SectorId sourceSector,
			SectorId destinationSector, Vector2 sourceEndpoint, Vector2 destinationEndpoint)
			: mOwner(owner), mEdgeType(edgeType), mSourceSector(sourceSector),
			  mDestinationSector(destinationSector), mSourceEndpoint(sourceEndpoint),
			  mDestinationEndpoint(destinationEndpoint) {}
	public:
		TraversalRequest(TraversalRequest const&) = delete;
		TraversalRequest& operator=(TraversalRequest const&) = delete;
		AgentId getOwner() const { return mOwner; }
		EdgeType getEdgeType() const { return mEdgeType; }
		SectorId getSourceSector() const { return mSourceSector; }
		SectorId getDestinationSector() const { return mDestinationSector; }
		Vector2 const& getSourceEndpoint() const { return mSourceEndpoint; }
		Vector2 const& getDestinationEndpoint() const { return mDestinationEndpoint; }
		TraversalRequestState getState() const { return mState; }
		bool wasPreparationRequested() const { return mPreparationRequested; }
		TraversalResourceId getResource() const { return mResource; }
		DeviceOperationId getPreparationOperation() const { return mPreparationOperation; }
		TraversalPermitId getPermit() const { return mPermit; }
	};

	enum struct TraversalPermitState { Active, Committed, Cancelled };

	class TraversalPermit
	{
		friend class Building;
		TraversalRequestId mRequest;
		AgentId mOwner;
		TraversalPermitState mState{ TraversalPermitState::Active };
		TraversalPermit(TraversalRequestId request, AgentId owner) : mRequest(request), mOwner(owner) {}
	public:
		TraversalPermit(TraversalPermit const&) = delete;
		TraversalPermit& operator=(TraversalPermit const&) = delete;
		TraversalRequestId getRequest() const { return mRequest; }
		AgentId getOwner() const { return mOwner; }
		TraversalPermitState getState() const { return mState; }
	};

	template<typename Entity>
	struct EntityLookup
	{
		Entity* entity{ nullptr };
		std::string diagnostic;
		explicit operator bool() const { return entity != nullptr; }
	};

	struct EntityRemovalResult
	{
		bool removed{ false };
		std::string diagnostic;
		explicit operator bool() const { return removed; }
	};

} // core
