#pragma once

#include <string>


namespace core
{

	enum struct SectorObjectType
	{
		None,
		BulkheadDoor,
		Door,
		ForceBridge,
		Controller,
		Ladder,
		Lift,
		Marker,
		Shuttle,
		Walkway,
		Window
	};

	std::string getSectorObjectTypeString(SectorObjectType type);

} // core
