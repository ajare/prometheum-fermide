#pragma once

#include <string>


namespace core
{
	enum struct VertexType
	{
		// Used to denote Corridor and Room-type Locations
		Location,
		// For vertices which are on a Ladder
		Ladder,
		// For vertices which are part of a Lift shaft
		Lift,
		// For vertices which are part of a Shuttle
		Shuttle,
		// For vertices which are in a Staircase
		Staircase
	};

	enum struct VertexSubType
	{
		// Placed manually to create a waypoint, for a particular reason.
		// Connected by LocationEdges or a relevant Transit Edge
		Marker,
		// Placed in Locations on either side of a Door.  Connected between Layers by DoorEdges.  
		Door,
		// Placed at an Interactable, for Agents to path to.  Connected by LocationEdges.
		Interactable,
		// Placed at a Window: Agents may wish to stop to look outside.
		// Connected by LocationEdges.
		Window,
		// Placed between two Locations.  Conencted by BulkheadDoorEdges.
		BulkheadDoor,
		// Placed between a gap in the air.  Connected by GapEdges.
		Gap,
		// Placed on either side of a ForceBridge.  Connected by ForceBridgeEdges
		ForceBridge,
		// Placed at a Ladder end, for either type of Ladder (SectorObject or Transit)
		// Connects to LadderVertices via LadderMountPoints
		Ladder,
		// Placed at a Lift entrance, for either type of Lift (SectorObject or Transit)
		// Connects to LiftVertices via LiftMountPoints
		Lift,
		// Placed at a Shuttle entrance, for either type of Lift (SectorObject or Transit)
		// Connects to ShuttleVertices via ShuttleMountPoints
		Shuttle,
		// Placed at a point within a Staircase
		Staircase
	};

	std::string getVertexTypeString(VertexType type);

	std::string getSubVertexTypeString(VertexSubType type);

} // core
