#include "core/ControllableActionType.h"


namespace core
{

	using namespace std;

	string getControllableActionTypeString(ControllableActionType type)
	{
		switch (type)
		{
		case ControllableActionType::None:
			return "None";

		case ControllableActionType::Open:
			return "Open";

		case ControllableActionType::Close:
			return "Close";

		case ControllableActionType::Toggle:
			return "Toggle";

		case ControllableActionType::ToggleLights:
			return "ToggleLights";

		case ControllableActionType::CallToStop:
			return "CallToStop";

		default:
			return "???";
		}
	}

} // core