#pragma once

#include <string>

namespace core
{

	enum struct ControllableActionType
	{
		None,
		Open,
		Close,
		Extend,
		Retract,
		Toggle,
		ToggleLights,
		CallToStop,
		COUNT
	};

	std::string getControllableActionTypeString(ControllableActionType type);

} // core
