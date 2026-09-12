#pragma once

#include <string>

namespace core
{

	enum struct ControllableActionType
	{
		None,
		Open,
		Close,
		Toggle,
		ToggleLights,
		CallToStop,
		COUNT
	};

	std::string getControllableActionTypeString(ControllableActionType type);

} // core
