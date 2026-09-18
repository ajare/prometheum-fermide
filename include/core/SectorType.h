#pragma once

#include <string>


namespace core
{

	enum struct SectorType
	{
		Location,
		Background,
		Facade,
		Ladder,
		Lift,
		Shuttle,
		Stairwell,
		Staircase
	};

	std::string getSectorTypeString(SectorType type);

} // core
