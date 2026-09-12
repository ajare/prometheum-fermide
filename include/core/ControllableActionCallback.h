#pragma once

#include <functional>

#include "core/ControllableActionType.h"
#include "core/ControllableActionStatus.h"
#include "core/ControllableActionData.h"


namespace core
{
	class Controllable;

	typedef std::function<void(Controllable const*, ControllableActionType, ControllableActionStatus, ControllableActionData const&)> ControllableActionCallback;

} // core
