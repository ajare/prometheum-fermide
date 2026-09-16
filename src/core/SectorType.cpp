#include "core/SectorType.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	string getSectorTypeString(SectorType type)
	{
		switch (type)
		{
		case SectorType::Location:
			return "Location";

		case SectorType::Ladder:
			return "Ladder";

		case SectorType::Lift:
			return "Lift";

		case SectorType::Shuttle:
			return "Shuttle";

		case SectorType::Stairwell:
			return "Stairwell";

		case SectorType::Staircase:
			return "Staircase";

		default:
			throw UnhandledException(type, "SectorType");
		}
	}

} // core