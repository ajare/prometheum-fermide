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

		case SectorType::Background:
			return "Background";

		case SectorType::Facade:
			return "Facade";

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

	bool isLocationLike(SectorType type)
	{
		// A Room, a Corridor (a Location), or a Facade: every Sector that is a
		// Location in the traversal sense.  See the header note for which checks
		// may use this and which must stay Location-only.
		return type == SectorType::Location || type == SectorType::Facade;
	}

} // core