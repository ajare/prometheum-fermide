#pragma once

#include "core/Button.h"

namespace core
{
	// Lift stop data is bound by LiftOrchestratedSystem, so the physical control
	// uses the common Button implementation.
	using LiftButton = Button;
}
