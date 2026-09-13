#include "core/SectorObjectType.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	string getSectorObjectTypeString(SectorObjectType type)
	{
		switch (type)
		{
		case SectorObjectType::BulkheadDoor:
			return "BulkheadDoor";

		case SectorObjectType::Door:
			return "Door";

		case SectorObjectType::ForceBridge:
			return "ForceBridge";

		case SectorObjectType::InteractionPoint:
			return "InteractionPoint";

		case SectorObjectType::Ladder:
			return "Ladder";

		case SectorObjectType::Lift:
			return "Lift";

		case SectorObjectType::Walkway:
			return "Walkway";

		case SectorObjectType::Window:
			return "Window";

		default:
			throw UnhandledException(type, "SectorObjectType");
		}
	}

} // core