#pragma once

#include <cstdint>


namespace core
{
	// Every entity category has its own ID type. Zero is always the null handle;
	// live IDs are monotonically allocated by the owning Building and never reused.
	#define CORE_DEFINE_ENTITY_ID(TypeName) \
		struct TypeName \
		{ \
			uint64_t value{ 0 }; \
			explicit operator bool() const { return value != 0; } \
			friend bool operator==(TypeName const&, TypeName const&) = default; \
			friend bool operator<(TypeName const& lhs, TypeName const& rhs) \
			{ \
				return lhs.value < rhs.value; \
			} \
		}

	CORE_DEFINE_ENTITY_ID(AgentId);
	CORE_DEFINE_ENTITY_ID(AgentGroupId);
	CORE_DEFINE_ENTITY_ID(MarkerId);
	CORE_DEFINE_ENTITY_ID(AgentTagId);
	CORE_DEFINE_ENTITY_ID(AgentBehaviourId);
	CORE_DEFINE_ENTITY_ID(SectorId);
	CORE_DEFINE_ENTITY_ID(InteractionPointId);
	CORE_DEFINE_ENTITY_ID(InteractionRequestId);
	CORE_DEFINE_ENTITY_ID(DeviceOperationId);
	CORE_DEFINE_ENTITY_ID(TraversalResourceId);
	CORE_DEFINE_ENTITY_ID(DoorOpenLeaseId);
	CORE_DEFINE_ENTITY_ID(DoorSensorId);
	CORE_DEFINE_ENTITY_ID(QueueTicketId);
	CORE_DEFINE_ENTITY_ID(TraversalRequestId);
	CORE_DEFINE_ENTITY_ID(TraversalPermitId);

	#undef CORE_DEFINE_ENTITY_ID

} // core
