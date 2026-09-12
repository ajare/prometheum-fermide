#pragma once

#include <string>
#include <utility>

#include "core/EntityId.h"


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
