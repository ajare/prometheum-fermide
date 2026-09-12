#pragma once

#include "core/LiftVertex.h"


namespace core
{
	class PlatformLiftVertex : public LiftVertex
	{
	public:

		// This is meant to be called internally to make a copy.  Why must it be public?
		PlatformLiftVertex(uint32_t id, std::shared_ptr<Sector> sector, std::shared_ptr<Lift> lift, float xLocationOffset, float yLocationOffset, uint32_t stopOffset);

		PlatformLiftVertex(std::shared_ptr<Sector> sector, std::shared_ptr<Lift> lift, float xLocationOffset, float yLocationOffset, uint32_t stopOffset);
	};

} // core
