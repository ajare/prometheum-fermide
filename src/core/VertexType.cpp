#include "core/VertexType.h"
#include "core/ControllerVertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	string getVertexTypeString(VertexType type)
	{
		switch (type)
		{
		case VertexType::Location:
			return "Location";

		case VertexType::Ladder:
			return "Ladder";

		case VertexType::Lift:
			return "Lift";

		case VertexType::Shuttle:
			return "Shuttle";

		case VertexType::Staircase:
			return "Staircase";

		default:
			throw UnhandledException(type, "VertexType");
		}
	}

	string getSubVertexTypeString(VertexSubType type)
	{
		switch (type)
		{
		case VertexSubType::BulkheadDoor:
			return "BulkheadDoor";

		case VertexSubType::Door:
			return "Door";

		case VertexSubType::Interactable:
			return "Interactable";

		case VertexSubType::ForceBridge:
			return "ForceBridge";

		case VertexSubType::Gap:
			return "Gap";

		case VertexSubType::Ladder:
			return "Ladder";

		case VertexSubType::Lift:
			return "Lift";

		case VertexSubType::Marker:
			return "Marker";

		case VertexSubType::Shuttle:
			return "Shuttle";

		case VertexSubType::Staircase:
			return "Staircase";

		case VertexSubType::Window:
			return "Window";

		default:
			throw UnhandledException(type, "VertexSubType");
		}

	}

} // core