#pragma once

#include "core/SensorType.h"

namespace core
{

	class SensorSource
	{
	public:

		SensorSource() = default;

		virtual ~SensorSource() = default;
	
		virtual bool canSense(SensorType sensor) const = 0;
	};

} // core
