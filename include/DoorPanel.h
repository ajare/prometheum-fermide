#pragma once

// The Selection panel's Door branch (ticket #99). Lives in its own translation
// unit - rather than inside UI.cpp - so the headless smoke checks can compile
// the real panel and render it inside a CPU-side ImGui context to pin its
// disabled-scope balance down; UI.cpp's spdlog/nfd dependencies never link
// headlessly.

#include <memory>

namespace core
{
	class Building;
	class SectorObject;
}

void renderDoorPanel(std::shared_ptr<core::Building> const& building,
	std::shared_ptr<const core::SectorObject> object);
