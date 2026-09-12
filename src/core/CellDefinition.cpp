#include "core/CellDefinition.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	string getCellFloorTypeString(CellFloorType type)
	{
		switch (type)
		{
		case CellFloorType::None:
			return "None";

		case CellFloorType::Ground:
			return "Ground";

		case CellFloorType::Walkway:
			return "Walkway";

		case CellFloorType::ForceBridge:
			return "ForceBridge";

		default:
			throw UnhandledException(type, "CellFloorType");
		}
	}

} // core