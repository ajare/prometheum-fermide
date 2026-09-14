#pragma once

#include <memory>

#include "core/Building.h"
#include "core/Graph.h"
#include "core/Agent.h"

#include "MouseButtonStatus.h"

MouseButtonStatus getMouseButtonStatus();

void handleShortcuts(std::shared_ptr<core::Building>& building);

// Returns true when the application may close immediately. A modified document
// instead opens the existing save/discard/cancel confirmation and returns false.
bool requestApplicationClose(std::shared_ptr<core::Building>& building);

void handleWorldInteraction(std::shared_ptr<core::Building> building,
	std::shared_ptr<const core::Graph> graph, MouseButtonStatus const& mouseStatus);

void handleContinuousKeyboardInput(std::shared_ptr<core::Building> building, uint64_t updateTimeMicros);

void renderUI(std::shared_ptr<core::Building>& building, std::shared_ptr<core::Agent> pathingAgent);


