// Background paint-tool checks, for ticket #38.
//
// The editor gains PaintTool::Background: click-drag a rectangle on the Layer
// canvas and release. Unlike a Room, a Background does not shrink to the largest
// free block - the full dragged rectangle is the request, and any occupied cell
// inside it refuses the paint, routed through canAddBackground()'s diagnostic.
//
// The editor itself is not reachable headlessly, so these checks mirror the
// tool's rectangle derivation (clamp to the World bounds, take the whole
// dragged block, validate on the drag's Layer) against the core contracts the
// release path calls: create, reject-on-occupied, min/max sizing, and correct
// Layer targeting.

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/Background.h"
#include "core/World.h"
#include "core/CellDefinition.h"
#include "core/Sector.h"
#include "core/SectorType.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool throws(std::function<void()> action)
	{
		try
		{
			action();
		}
		catch (std::exception const&)
		{
			return true;
		}
		return false;
	}

	// Mirrors the editor's Background paint preview: the dragged block is clamped
	// to the World bounds and taken whole - no shrink-to-free sweep - and
	// canAddBackground() on the drag's Layer decides whether it lands.
	struct PaintPreview
	{
		bool valid{ false };
		uint32_t x{ 0 };
		uint32_t y{ 0 };
		uint32_t cellsWide{ 0 };
		uint32_t levelsHigh{ 0 };
		std::string diagnostic;
	};

	PaintPreview previewBackgroundPaint(core::World const& world, uint32_t layerIndex,
		int anchorX, int anchorY, int endX, int endY)
	{
		PaintPreview preview;
		auto const clampedEndX = std::clamp(endX, 0, (int)world.getCellsWide() - 1);
		auto const clampedEndY = std::clamp(endY, 0, (int)world.getLevelsHigh() - 1);
		preview.x = (uint32_t)std::min(anchorX, clampedEndX);
		preview.y = (uint32_t)std::min(anchorY, clampedEndY);
		preview.cellsWide = (uint32_t)(std::abs(clampedEndX - anchorX) + 1);
		preview.levelsHigh = (uint32_t)(std::abs(clampedEndY - anchorY) + 1);
		preview.valid = world.canAddBackground(layerIndex, preview.y, preview.x,
			preview.cellsWide, preview.levelsHigh, &preview.diagnostic);
		return preview;
	}

	// Mirrors the editor's release path: preview, then create on the same Layer
	// with the same footprint.
	uint32_t releaseBackgroundPaint(core::World& world, uint32_t layerIndex,
		int anchorX, int anchorY, int endX, int endY)
	{
		auto const preview = previewBackgroundPaint(world, layerIndex, anchorX, anchorY,
			endX, endY);
		if (!preview.valid)
			throw std::runtime_error("Release path ran on an invalid preview: " + preview.diagnostic);
		return world.addBackground(layerIndex, preview.y, preview.x, preview.cellsWide,
			preview.levelsHigh);
	}

	std::shared_ptr<const core::Background> backgroundIn(core::World const& world,
		uint32_t sectorIndex)
	{
		auto sector = world.getSector(sectorIndex);
		require(sector != nullptr, "World reported a null Sector");
		require(sector->getType() == core::SectorType::Background,
			("Sector " + std::to_string(sectorIndex) + " is not a Background").c_str());
		auto background = std::dynamic_pointer_cast<const core::Background>(sector);
		require(background != nullptr, "A Background Sector is not a core::Background");
		return background;
	}

	void footprintIsStamped(core::World const& world, uint32_t layerIndex,
		uint32_t sectorIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
	{
		auto const layer = world.getLayer(layerIndex);
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cell = layer->getCellDefinition(ix, iy);
				require(cell.sectorIndex == sectorIndex,
					std::format("Painted Background does not own cell {},{} on Layer {}",
						ix, iy, layerIndex).c_str());
				require(!cell.hasObject(),
					std::format("Painted Background cell {},{} carries a SectorObject", ix, iy).c_str());
			}
		}
	}
}

// A drag over empty canvas creates the Background with exactly the dragged
// footprint on the dragged Layer.
void paintingAnEmptyBlockCreatesABackground()
{
	core::World world("Paint empty", 12, 4);
	world.addRoom("Front room", 0, 0, 0, 12, 4);
	world.finishBuild();
	// The editor pauses the simulation when a paint tool is armed; structural
	// edits after the initial build require it.
	world.pauseSimulation();

	// The 6x3 from the ticket's manual test, dragged backwards to forwards.
	auto const index = releaseBackgroundPaint(world, 1, 5, 2, 0, 0);
	auto const background = backgroundIn(world, index);
	require(background->getLayerIndex() == 1, "The painted Background landed on the wrong Layer");
	require(background->getCellX() == 0 && background->getCellY() == 0,
		"The painted Background did not anchor at the dragged corner");
	require(background->getCellsWide() == 6 && background->getLevelsHigh() == 3,
		"The painted Background did not keep the dragged footprint");
	footprintIsStamped(world, 1, index, 0, 0, 6, 3);
}

// Any occupied cell inside the dragged block refuses the paint, with a
// diagnostic, and nothing is created.
void paintingOverAnOccupiedCellIsRejected()
{
	core::World world("Paint over", 12, 4);
	world.addLayer();
	world.addRoom("Obstacle", 1, 1, 4, 2, 2);
	world.finishBuild();
	world.pauseSimulation();

	// A 6x3 that swallows the Room's cells on the same Layer.
	auto const preview = previewBackgroundPaint(world, 1, 0, 0, 5, 2);
	require(!preview.valid, "A Background overlapping a Location was previewed as valid");
	require(!preview.diagnostic.empty(), "The rejected paint gave no diagnostic");
	require(throws([&] { world.addBackground(1, 0, 0, 6, 3); }),
		"A Background overlapping a Location was created anyway");

	// The obstacle is untouched and the cells around it are still paintable.
	require(world.getSector(0)->getType() == core::SectorType::Location,
		"The blocking Location changed identity");
	require(previewBackgroundPaint(world, 1, 0, 3, 3, 3).valid,
		"A run clear of the obstacle was refused");

	// The same dragged block on a Layer with nothing on it is accepted: the
	// refusal reads occupancy on the Background's own Layer only.
	require(previewBackgroundPaint(world, 2, 0, 0, 5, 2).valid,
		"A free Layer refused the same dragged block");
}

// The minimum paint is 1x1; the maximum is capped by the World bounds,
// which the preview clamps to rather than running past.
void thePaintIsSizedBetweenOneByOneAndTheWorldBounds()
{
	core::World world("Paint sizing", 12, 4);
	world.addLayer();
	world.finishBuild();
	world.pauseSimulation();

	auto const single = previewBackgroundPaint(world, 1, 3, 2, 3, 2);
	require(single.valid, "The minimum 1x1 paint was refused");
	require(single.cellsWide == 1 && single.levelsHigh == 1,
		"The 1x1 paint did not report a one-cell footprint");
	auto const singleIndex = releaseBackgroundPaint(world, 1, 3, 2, 3, 2);
	require(backgroundIn(world, singleIndex)->getCellsWide() == 1
		&& backgroundIn(world, singleIndex)->getLevelsHigh() == 1,
		"The 1x1 Background was not created one cell to a block");

	// Dragging past the edge clamps into the World instead of refusing.
	auto const clamped = previewBackgroundPaint(world, 2, 0, 0, 99, 99);
	require(clamped.valid, "A drag past the World bounds was refused instead of clamped");
	require(clamped.cellsWide == 12 && clamped.levelsHigh == 4,
		"The clamped drag did not fill the World bounds");

	// A zero-sized block is not a paintable Background.
	std::string diagnostic;
	require(!world.canAddBackground(1, 0, 0, 0, 1, &diagnostic),
		"A zero-width Background was accepted");
	require(!world.canAddBackground(1, 0, 0, 1, 0, &diagnostic),
		"A zero-height Background was accepted");
}

// The paint lands on the Layer the drag started on, never on a neighbouring one,
// and every Layer - front-most and back-most included - accepts it.
void thePaintTargetsTheDragLayer()
{
	core::World world("Paint layer targeting", 12, 3);
	world.addLayer();
	world.addRoom("Front room", 0, 0, 0, 6, 3);
	world.addRoom("Back room", 2, 0, 0, 6, 3);
	world.finishBuild();
	world.pauseSimulation();

	// Occupancy on the Layers in front of and behind Layer 1 must not refuse a
	// Background painted on Layer 1 over the same cells.
	auto const middle = previewBackgroundPaint(world, 1, 0, 0, 5, 2);
	require(middle.valid,
		("Occupancy on neighbouring Layers refused a Background on the drag's Layer: "
			+ middle.diagnostic).c_str());
	auto const middleIndex = releaseBackgroundPaint(world, 1, 0, 0, 5, 2);
	require(backgroundIn(world, middleIndex)->getLayerIndex() == 1,
		"The Background did not land on the drag's Layer");
	footprintIsStamped(world, 1, middleIndex, 0, 0, 6, 3);
	auto const& constWorld = std::as_const(world);
	require(constWorld.getLayer(0)->getCellDefinition(0, 0).sectorIndex != middleIndex,
		"The Background leaked onto the Layer in front");
	require(constWorld.getLayer(2)->getCellDefinition(0, 0).sectorIndex != middleIndex,
		"The Background leaked onto the Layer behind");

	// A Background is not a Transit: the front-most Layer takes it with no gate.
	require(previewBackgroundPaint(world, 0, 6, 0, 11, 2).valid,
		"The front-most Layer refused a Background paint");
	releaseBackgroundPaint(world, 0, 6, 0, 11, 2);

	// And so does the back-most Layer.
	require(previewBackgroundPaint(world, 2, 6, 0, 11, 2).valid,
		"The back-most Layer refused a Background paint");
	releaseBackgroundPaint(world, 2, 6, 0, 11, 2);

	world.finishBuild();
	require(world.getNumSectors() == 5,
		"The paint path did not create exactly the Backgrounds it was asked for");
}

void runBackgroundPaintSmokeChecks()
{
	paintingAnEmptyBlockCreatesABackground();
	paintingOverAnOccupiedCellIsRejected();
	thePaintIsSizedBetweenOneByOneAndTheWorldBounds();
	thePaintTargetsTheDragLayer();
}
