#pragma once

#include <string>


namespace core
{

	enum struct SectorType
	{
		Location,
		Ladder,
		Lift,
		Shuttle,
		Staircase
	};

	std::string getSectorTypeString(SectorType type);

} // core
