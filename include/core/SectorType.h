#pragma once

#include <string>


namespace core
{

	enum struct SectorType
	{
		Location,
		Background,
		Ladder,
		Lift,
		Shuttle,
		Stairwell,
		Staircase
	};

	std::string getSectorTypeString(SectorType type);

} // core
