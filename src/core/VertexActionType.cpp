#include "core/VertexActionType.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	string getVertexActionTypeString(VertexActionType type)
	{
		switch (type)
		{
		case VertexActionType::None:
			return "No action";

		case VertexActionType::UseController:
			return "Use Controller";

		case VertexActionType::UnblockForExit:
			return "Unblock for exit";

		case VertexActionType::LookOutOfWindow:
			return "Look out of Window";

		default:
			throw UnhandledException(type, "VertexActionType");
		}
	}

} // core