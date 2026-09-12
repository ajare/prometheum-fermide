#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include "core/EdgeType.h"
#include "core/EntityId.h"
#include "core/Vector2.h"


namespace core
{
	class Building;

	// Replacement coordination entities intentionally contain values and typed
	// handles only. Their lifetime is owned by Building's registries.
	class InteractionPoint
	{
		friend class Building;

		std::string mName;

		explicit InteractionPoint(std::string name)
			: mName(std::move(name))
		{
		}

	public:
		InteractionPoint(InteractionPoint const&) = delete;
		InteractionPoint& operator=(InteractionPoint const&) = delete;

		std::string const& getName() const
		{
			return mName;
		}
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
		DeviceOperationState mState{ DeviceOperationState::Pending };

		DeviceOperation(std::string name, AgentId requester)
			: mName(std::move(name))
			, mRequester(requester)
		{
		}

	public:
		DeviceOperation(DeviceOperation const&) = delete;
		DeviceOperation& operator=(DeviceOperation const&) = delete;

		std::string const& getName() const
		{
			return mName;
		}

		AgentId getRequester() const
		{
			return mRequester;
		}

		DeviceOperationState getState() const
		{
			return mState;
		}

		void setState(DeviceOperationState state)
		{
			mState = state;
		}
	};

	class TraversalResource
	{
		friend class Building;

		std::string mName;

		explicit TraversalResource(std::string name)
			: mName(std::move(name))
		{
		}

	public:
		TraversalResource(TraversalResource const&) = delete;
		TraversalResource& operator=(TraversalResource const&) = delete;

		std::string const& getName() const
		{
			return mName;
		}
	};

	enum struct TraversalRequestState
	{
		Pending,
		Granted,
		Denied,
		Cancelled,
		Committed
	};

	// Requests and permits are Building-owned transaction records. They contain
	// no owning pointers: the path keeps topology alive while the transaction is
	// active, and snapshots expose only stable IDs and endpoint values.
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
		TraversalPermitId mPermit;

		TraversalRequest(AgentId owner, EdgeType edgeType, SectorId sourceSector,
			SectorId destinationSector, Vector2 sourceEndpoint, Vector2 destinationEndpoint)
			: mOwner(owner)
			, mEdgeType(edgeType)
			, mSourceSector(sourceSector)
			, mDestinationSector(destinationSector)
			, mSourceEndpoint(sourceEndpoint)
			, mDestinationEndpoint(destinationEndpoint)
		{
		}

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
		TraversalPermitId getPermit() const { return mPermit; }
	};

	enum struct TraversalPermitState
	{
		Active,
		Committed,
		Cancelled
	};

	class TraversalPermit
	{
		friend class Building;

		TraversalRequestId mRequest;
		AgentId mOwner;
		TraversalPermitState mState{ TraversalPermitState::Active };

		TraversalPermit(TraversalRequestId request, AgentId owner)
			: mRequest(request)
			, mOwner(owner)
		{
		}

	public:
		TraversalPermit(TraversalPermit const&) = delete;
		TraversalPermit& operator=(TraversalPermit const&) = delete;

		TraversalRequestId getRequest() const { return mRequest; }
		AgentId getOwner() const { return mOwner; }
		TraversalPermitState getState() const { return mState; }
	};

	// A lookup result keeps failure handling explicit. The returned pointer is a
	// temporary non-owning view and must not be retained in another entity.
	template<typename Entity>
	struct EntityLookup
	{
		Entity* entity{ nullptr };
		std::string diagnostic;

		explicit operator bool() const
		{
			return entity != nullptr;
		}
	};

	struct EntityRemovalResult
	{
		bool removed{ false };
		std::string diagnostic;

		explicit operator bool() const
		{
			return removed;
		}
	};

} // core
