#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/Building.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Vertex.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Regression for the door-test-1 editor crash: an Agent standing in a
	// Location the Graph serves no vertices for (a Corridor with no traversable
	// threshold anywhere) used to make calculatePath throw GraphException
	// ("Sector not found in Sector->Vertex lookup"), which escaped the Ctrl+P
	// path-set flow and terminated the editor. An isolated Location offers no
	// route at all; that is an ordinary "no path" outcome and must come back
	// as no Path, the same as any unreachable target.
	void pathingFromAnIsolatedLocationReturnsNoPath()
	{
		core::Building building("Isolated corridor", 12, 1);

		// The stranded Corridor is split from the connected part of the
		// building by a Room, with no Door or traversable Window anywhere, so
		// the Graph builds no vertices for it.
		auto const stranded = building.addCorridor(0, 0, 3);
		building.addRoom("Blocker", 0, 0, 3, 2, 1);
		auto const connected = building.addCorridor(0, 5, 4);
		uint32_t markerIdentifier = 0;
		building.addSectorMarker(connected, 0, 1.5f, &markerIdentifier);
		building.finishBuild();

		auto const destination = building.getGraph()->getVertexByIdentifier(markerIdentifier);
		require(destination != nullptr,
			"The connected Corridor's Marker has no Graph vertex; the scenario is not wired as intended");

		// Control: an Agent in the connected Corridor reaches the Marker, so
		// the Building's graph really is traversable.
		auto const settledAgentId = building.createAgent("Settled agent", connected, 0, 2.5f);
		auto const settledAgent = building.lookupAgent(settledAgentId).entity;
		require(settledAgent != nullptr, "The settled agent was not created");
		auto const settledPath = building.getGraph()->calculatePath(settledAgent, destination);
		require(settledPath && !settledPath->nodes.empty(),
			"No path exists inside the connected Corridor; the scenario is not wired as intended");

		// The regression: the route request out of the stranded Corridor must
		// not throw.
		auto const strandedAgentId = building.createAgent("Stranded agent", stranded, 0, 1.0f);
		auto const strandedAgent = building.lookupAgent(strandedAgentId).entity;
		require(strandedAgent != nullptr, "The stranded agent was not created");

		auto const path = building.getGraph()->calculatePath(strandedAgent, destination);
		require(!path || path->nodes.empty(),
			"A route was found out of an isolated Corridor with no traversable threshold");
	}
}

void runIsolatedSectorPathingSmokeChecks()
{
	pathingFromAnIsolatedLocationReturnsNoPath();
}
