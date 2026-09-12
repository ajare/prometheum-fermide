#pragma once

#include <string>


namespace core
{

	enum struct SensorType
	{
		AgentBlocking
	};

	std::string getSensorTypeString(SensorType type);

} // core
