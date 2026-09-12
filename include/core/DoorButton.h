#pragma once

#include "core/Button.h"

namespace core
{
	// Door buttons use the common Button implementation. Door-specific behaviour is
	// supplied by ButtonDoorOrchestratedSystem rather than a partial subclass.
	using DoorButton = Button;
}
