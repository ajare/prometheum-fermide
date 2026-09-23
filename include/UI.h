#pragma once

#include <filesystem>
#include <memory>

#include "core/World.h"
#include "core/Graph.h"
#include "core/Agent.h"

#include "MouseButtonStatus.h"

inline bool isCanvasSelectableSectorType(core::SectorType type)
{
	return type == core::SectorType::Location
		|| type == core::SectorType::Lift
		|| type == core::SectorType::Shuttle
		|| type == core::SectorType::Ladder
		|| type == core::SectorType::Stairwell
		|| type == core::SectorType::Staircase
		// A Background is selected to be inspected and recoloured, and to be deleted
		// through its cascade. It is never entered, so selection asks nothing of it.
		|| type == core::SectorType::Background
		// A Facade is selected to be inspected and recoloured from the Selection
		// panel; entering it works exactly as entering a Room does.
		|| type == core::SectorType::Facade;
}

inline bool shouldDrawCanvasSectorEditOverlay(uint32_t sectorLayer,
	uint32_t visibleLayer)
{
	return sectorLayer == visibleLayer;
}

MouseButtonStatus getMouseButtonStatus();

// Loads the recent-file list, creating its storage file when needed.
void initializeRecentFiles(std::filesystem::path const& filepath);

void handleShortcuts(std::shared_ptr<core::World>& world);

// Returns true when the application may close immediately. A modified document
// instead opens the existing save/discard/cancel confirmation and returns false.
bool requestApplicationClose(std::shared_ptr<core::World>& world);

void handleWorldInteraction(std::shared_ptr<core::World> world,
	std::shared_ptr<const core::Graph> graph, MouseButtonStatus const& mouseStatus);

void handleContinuousKeyboardInput(std::shared_ptr<core::World> world, uint64_t updateTimeMicros);

void renderUI(std::shared_ptr<core::World>& world, std::shared_ptr<core::Agent> pathingAgent);


