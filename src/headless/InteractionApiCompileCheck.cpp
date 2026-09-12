#include <type_traits>

#include "core/BulkheadDoorButton.h"
#include "core/DoorButton.h"
#include "core/ForceBridgeButton.h"
#include "core/LadderButton.h"
#include "core/LiftButton.h"
#include "core/Coordination.h"
#include "core/EntityId.h"

static_assert(std::is_same_v<core::BulkheadDoorButton, core::Button>);
static_assert(std::is_same_v<core::DoorButton, core::Button>);
static_assert(std::is_same_v<core::ForceBridgeButton, core::Button>);
static_assert(std::is_same_v<core::LadderButton, core::Button>);
static_assert(std::is_same_v<core::LiftButton, core::Button>);

static_assert(!std::is_convertible_v<core::AgentId, core::InteractionPointId>);
static_assert(!std::is_convertible_v<core::InteractionRequestId, core::DeviceOperationId>);
static_assert(!std::is_convertible_v<core::DeviceOperationId, core::TraversalResourceId>);
static_assert(!std::is_convertible_v<core::TraversalRequestId, core::TraversalPermitId>);
static_assert(!std::is_convertible_v<core::AgentId, core::TraversalRequestId>);
static_assert(!std::is_convertible_v<core::QueueTicketId, core::TraversalRequestId>);
static_assert(!std::is_constructible_v<core::InteractionPoint, std::string>);
static_assert(!std::is_constructible_v<core::TraversalResource, std::string>);
static_assert(!std::is_default_constructible_v<core::TraversalRequest>);
static_assert(!std::is_default_constructible_v<core::TraversalPermit>);
