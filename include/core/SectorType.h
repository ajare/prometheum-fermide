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

	// A Facade is a Location in every horizontal-adjacency respect: whatever may
	// sit next to a Location and share a walkable boundary with it must accept a
	// Facade too, and vice versa.  Adjacency checks - graph cross-sector Vertex
	// creation across open ends, wall-removal validation, floor-level matching -
	// admit Facade through this helper so the widening lives in one place
	// (ADR 0003, ticket #45).
	//
	// Checks that are about plain Location identity rather than horizontal
	// adjacency - resize and delete affordances, transit landing validation -
	// must keep testing SectorType::Location directly.
	bool isLocationLike(SectorType type);

} // core
