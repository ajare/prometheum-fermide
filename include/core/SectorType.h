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

	// A Facade is a Location in every traversal respect: whatever may sit next to
	// a Location and share a walkable boundary with it must accept a Facade too,
	// and vice versa, and whatever may land on a Location may land on a Facade.
	// Adjacency checks - graph cross-sector Vertex creation across open ends,
	// wall-removal validation, floor-level matching - admit Facade through this
	// helper so the widening lives in one place (ADR 0003, ticket #45), and so
	// does transit landing validation: a Lift, Staircase, Ladder, Stairwell or
	// Shuttle lands on a Facade exactly as it lands on a Room, and the landing
	// Vertex of a Facade is a VertexType::Location (ticket #52).
	//
	// Checks that are about plain Location identity rather than traversal -
	// the resize affordance, and the type-specific rendering rules - keep
	// testing SectorType::Location directly.  Deletion is not one of them:
	// a Facade is occupiable and deletes through its own planRemoveFacade()
	// with the same Agent-and-object cascade a Room deletion plays, while
	// Facade resize stays refused (ticket #53).
	bool isLocationLike(SectorType type);

} // core
