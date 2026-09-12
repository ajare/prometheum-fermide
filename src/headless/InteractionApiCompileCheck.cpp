#include <type_traits>

#include "core/BulkheadDoorButton.h"
#include "core/DoorButton.h"
#include "core/ForceBridgeButton.h"
#include "core/LadderButton.h"
#include "core/LiftButton.h"

static_assert(std::is_same_v<core::BulkheadDoorButton, core::Button>);
static_assert(std::is_same_v<core::DoorButton, core::Button>);
static_assert(std::is_same_v<core::ForceBridgeButton, core::Button>);
static_assert(std::is_same_v<core::LadderButton, core::Button>);
static_assert(std::is_same_v<core::LiftButton, core::Button>);
