#pragma once


namespace core
{

	enum struct SectorEndType
	{
		None,
		Wall,
		BulkheadDoor
	};

	struct SectorEnd
	{
		// Left, right.  Array rather than names so we can index the other with 1-.
		SectorEndType end[2] = {
			SectorEndType::Wall,
			SectorEndType::Wall
		};
	};


} // core
