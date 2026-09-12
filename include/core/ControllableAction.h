#pragma once

#include <format>

#include "core/ControllableActionType.h"
#include "core/ControllableActionStatus.h"
#include "core/ControllableActionData.h"
#include "core/ControllableActionCallback.h"


namespace core
{

	struct ControllableAction
	{
		ControllableActionType type;
		ControllableActionStatus status;
		ControllableActionCallback callback;
		uint32_t id;
		ControllableActionData data;
		bool autoTrigger{ true };

		bool running() const
		{
			return status == ControllableActionStatus::InProgress;
		}

		bool finished() const
		{
			return status == ControllableActionStatus::CompletedFailure ||
				status == ControllableActionStatus::CompletedInterrupted ||
				status == ControllableActionStatus::CompletedSuccess;
		}

		std::string toString() const
		{
			return std::format("'{}': id={} status={} i={} f={}",
				getControllableActionTypeString(type),
				id,
				getControllableActionStatusString(status),
				data.i,
				data.f
			);
		}
	};

} // core
