// Threshold placement must only treat objects on the authored Layer as blockers.
// The Layer behind supplies the destination Sector and floor, but its objects and
// Markers coexist with a Door or Window authored in front of it.

#include <stdexcept>
#include <string>

#include "core/World.h"
#include "core/CellDefinition.h"
#include "core/Edge.h"
#include "core/EdgeType.h"
#include "core/Graph.h"
#include "core/Layer.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct ThreeLayerRooms
	{
		core::World world{ "Threshold overlap", 12, 3 };
		uint32_t sectors[3]{};

		ThreeLayerRooms()
		{
			world.addLayer();
			for (uint32_t layer = 0; layer < 3; ++layer)
				sectors[layer] = world.addRoom("Room " + std::to_string(layer),
					layer, 0, 0, 12, 1);
		}
	};

	core::CellDefinition const& cell(core::World const& world,
		uint32_t layer, uint32_t x, uint32_t y)
	{
		return world.getLayer(layer)->getCellDefinition(x, y);
	}

	uint32_t edgeCount(core::World const& world, core::EdgeType type)
	{
		uint32_t count{ 0 };
		for (auto const& edge : world.getGraph()->getEdges())
			if (edge && edge->getType() == type) ++count;
		return count;
	}

	void aBackLayerMarkerDoesNotBlockAWindow()
	{
		ThreeLayerRooms layout;
		layout.world.addSectorMarker(layout.sectors[1], 0, 3.5f);

		std::string diagnostic;
		auto const allowed = layout.world.canAddSectorWindow(0, 0, 3, 1, 1, &diagnostic);
		require(allowed, ("A back-Layer Marker blocked a Window: " + diagnostic).c_str());
		layout.world.addSectorWindow(0, 0, 3, 1, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		require(cell(layout.world, 1, 3, 0).markers.size() == 1,
			"Adding a Window disturbed the Marker on the Layer behind");
	}

	void aBackLayerMarkerDoesNotBlockADoor()
	{
		ThreeLayerRooms layout;
		layout.world.addSectorMarker(layout.sectors[1], 0, 3.5f);

		std::string diagnostic;
		auto const allowed = layout.world.canAddCorridorDoor(0, 0, 3, &diagnostic);
		require(allowed, ("A back-Layer Marker blocked a Door: " + diagnostic).c_str());
		layout.world.addSectorDoor(0, 0, 3);
		require(cell(layout.world, 1, 3, 0).markers.size() == 1,
			"Adding a Door disturbed the Marker on the Layer behind");
	}

	void thresholdsOnAdjacentPairsCanShareACell()
	{
		{
			ThreeLayerRooms layout;
			layout.world.addSectorWindow(1, 0, 6, 1, 1, { true });
			auto const backAuthoredObject = cell(layout.world, 1, 6, 0).sectorObjectIndex;

			std::string diagnostic;
			auto const allowed = layout.world.canAddSectorWindow(0, 0, 6, 1, 1, &diagnostic);
			require(allowed, ("A Window on the Layer behind blocked a Window: " + diagnostic).c_str());
			layout.world.addSectorWindow(0, 0, 6, 1, 1, { true });
			require(cell(layout.world, 1, 6, 0).sectorObjectType == core::SectorObjectType::Window
				&& cell(layout.world, 1, 6, 0).sectorObjectIndex == backAuthoredObject,
				"A front Window replaced the Window authored on the Layer behind");
			layout.world.finishBuild();
			require(layout.world.isTraversalTopologyValid(),
				"Stacked traversable Windows produced invalid topology");
			require(edgeCount(layout.world, core::EdgeType::Window) == 2,
				"Stacked traversable Windows did not both reach the Graph");
		}
		{
			ThreeLayerRooms layout;
			layout.world.addSectorDoor(1, 0, 6);
			auto const backAuthoredObject = cell(layout.world, 1, 6, 0).sectorObjectIndex;

			std::string diagnostic;
			auto const allowed = layout.world.canAddCorridorDoor(0, 0, 6, &diagnostic);
			require(allowed, ("A Door on the Layer behind blocked a Door: " + diagnostic).c_str());
			layout.world.addSectorDoor(0, 0, 6);
			require(cell(layout.world, 1, 6, 0).sectorObjectType == core::SectorObjectType::Door
				&& cell(layout.world, 1, 6, 0).sectorObjectIndex == backAuthoredObject,
				"A front Door replaced the Door authored on the Layer behind");
			layout.world.finishBuild();
			require(layout.world.isTraversalTopologyValid(),
				"Stacked Doors produced invalid topology");
			require(edgeCount(layout.world, core::EdgeType::Door) == 2,
				"Stacked Doors did not both reach the Graph");
		}
	}
}

void runThresholdLayerOverlapSmokeChecks()
{
	aBackLayerMarkerDoesNotBlockAWindow();
	aBackLayerMarkerDoesNotBlockADoor();
	thresholdsOnAdjacentPairsCanShareACell();
}
