#pragma once

// Shared Agent drop-target logic, for ticket #50.
//
// A Facade is occupiable "exactly as a Room" (ADR 0003), but the canvas
// paths that place or move an Agent tested SectorType::Location by identity
// and so refused a Facade. These checks live here as inline functions so the
// headless smoke checks exercise the same code the canvas runs: UI.cpp keeps
// only the screen-space translation around them.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include "core/Agent.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Vector2.h"

#include "UISettings.h"

extern UISettings gUISettings;

struct PegmanTarget
{
	std::shared_ptr<const core::Sector> sector;
	uint32_t deckOffset{ 0 };
	float localX{ 0.0f };
	float feetY{ 0.0f };
	float floorY{ 0.0f };
	std::string diagnostic;
	uint32_t cellX{ 0 };
	uint32_t cellY{ 0 };
	uint32_t cellsWide{ 1 };

	explicit operator bool() const { return sector != nullptr && diagnostic.empty(); }
};

// True when the Sector may hold an Agent: a Location in the occupancy sense -
// a Room, a Corridor, or a Facade (ADR 0003) - with capacity still free.
inline bool locationHasCapacity(std::shared_ptr<const core::Sector> const& sector)
{
	if (!sector || !core::isLocationLike(sector->getType())) return false;
	auto capacity = sector->getCapacity();
	return capacity == ~0u || sector->getAgents().size() < capacity;
}

// The world-coordinate core of the pegman's Agent drop: everything the
// canvas does after the canvas-rectangle test and the screen-to-world
// translation in UI.cpp's getPegmanTarget().
inline PegmanTarget pegmanAgentTargetAtWorld(std::shared_ptr<const core::World> const& world,
	core::Vector2 const& worldPosition)
{
	auto sector = world->getSectorAtPosition(gUISettings.visibleLayer, worldPosition.x, worldPosition.y);
	if (!locationHasCapacity(sector) || !sector->pointInBounds(worldPosition.x, worldPosition.y)) return {};

	auto cellY = (uint32_t)std::floor(worldPosition.y);
	if (cellY < sector->getCellY()) return {};
	auto deckOffset = cellY - sector->getCellY();
	if (deckOffset >= sector->getDecksHigh()) return {};

	float halfAgentWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
	float minimumX = halfAgentWidth;
	float maximumX = sector->getSize().x - halfAgentWidth;
	float localX = worldPosition.x - sector->getPosition().x;
	localX = minimumX <= maximumX
		? std::clamp(localX, minimumX, maximumX)
		: sector->getSize().x * 0.5f;

	return { sector, deckOffset, localX, worldPosition.y,
		(float)sector->getCellY() + deckOffset, {} };
}

// The drag-move target for a selected Agent, in world coordinates. An Agent
// moving within its own Sector retains its capacity; crossing into another
// Location - Room or Facade alike - must find capacity there.
inline PegmanTarget getAgentMoveTarget(std::shared_ptr<const core::World> const& world,
	core::Agent const* agent, core::Vector2 const& worldPosition)
{
	if (worldPosition.x < 0.0f || worldPosition.y < 0.0f
		|| worldPosition.x >= world->getCellsWide() || worldPosition.y >= world->getDecksHigh())
		return { nullptr, 0, 0.0f, worldPosition.y, worldPosition.y, "Drop the Agent inside the world" };
	auto sector = world->getSectorAtPosition(gUISettings.visibleLayer, worldPosition.x, worldPosition.y);
	bool const retainsCapacity = sector && agent && agent->getSector() == sector.get();
	if (!sector || !core::isLocationLike(sector->getType())
		|| (!retainsCapacity && !locationHasCapacity(sector))
		|| !sector->pointInBounds(worldPosition.x, worldPosition.y))
		return { nullptr, 0, 0.0f, worldPosition.y, worldPosition.y,
			"Agents require a viable sector with available capacity" };

	auto cellY = (uint32_t)std::floor(worldPosition.y);
	if (cellY < sector->getCellY() || cellY >= sector->getCellY() + sector->getDecksHigh())
		return { nullptr, 0, 0.0f, worldPosition.y, worldPosition.y, "Agent deck is outside the sector" };
	float halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
	float localX = std::clamp(worldPosition.x - sector->getPosition().x, halfWidth,
		std::max(halfWidth, sector->getSize().x - halfWidth));
	return { sector, cellY - sector->getCellY(), localX, worldPosition.y,
		(float)cellY, {} };
}
