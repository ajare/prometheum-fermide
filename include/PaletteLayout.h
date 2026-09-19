#pragma once

#include "imgui/imgui.h"

// Geometry of the editor's object-palette tray (ticket #54).
//
// The tray is a two-row grid of slots. The top row holds the space and
// transit tools; the bottom row holds the Agent, the Marker, and the
// objects that attach to already-placed Sectors. The bottom row keeps the
// top row's column grid: Agent sits under Room, Marker under Corridor,
// and Door under Ladder, with the remaining tools continuing to the
// right. Because the bottom row's tail (RoomLadder, PlatformLift)
// extends past the top row's last column, the tray width is derived
// from the grid's column count rather than from the top row alone.

enum class PaletteSlot
{
	// Top row: spaces and transits.
	Room,
	Facade,
	Corridor,
	Background,
	Ladder,
	Stairwell,
	Lift,
	Shuttle,
	Staircase,
	// Bottom row: agents, markers, and attached objects.
	Agent,
	Marker,
	Door,
	BulkheadDoor,
	Window,
	Walkway,
	ForceBridge,
	RoomLadder,
	PlatformLift,
	Count
};

inline constexpr float PaletteSlotSize{ 36.0f };	// slot height
inline constexpr float PaletteSlotWidth{ 64.0f };
inline constexpr float PaletteGap{ 6.0f };
inline constexpr float PalettePadding{ 6.0f };

inline constexpr int paletteSlotRow(PaletteSlot slot)
{
	return slot >= PaletteSlot::Agent ? 1 : 0;
}

inline constexpr int paletteSlotColumn(PaletteSlot slot)
{
	switch (slot)
	{
	case PaletteSlot::Room: return 0;
	case PaletteSlot::Facade: return 1;
	case PaletteSlot::Corridor: return 2;
	case PaletteSlot::Background: return 3;
	case PaletteSlot::Ladder: return 4;
	case PaletteSlot::Stairwell: return 5;
	case PaletteSlot::Lift: return 6;
	case PaletteSlot::Shuttle: return 7;
	case PaletteSlot::Staircase: return 8;
	case PaletteSlot::Agent: return 0;
	case PaletteSlot::Marker: return 2;
	case PaletteSlot::Door: return 4;
	case PaletteSlot::BulkheadDoor: return 5;
	case PaletteSlot::Window: return 6;
	case PaletteSlot::Walkway: return 7;
	case PaletteSlot::ForceBridge: return 8;
	case PaletteSlot::RoomLadder: return 9;
	case PaletteSlot::PlatformLift: return 10;
	case PaletteSlot::Count: break;
	}
	return 0;
}

inline constexpr int paletteColumnCount()
{
	int widest = 0;
	for (int index = 0; index < static_cast<int>(PaletteSlot::Count); ++index)
	{
		auto column = paletteSlotColumn(static_cast<PaletteSlot>(index));
		if (column + 1 > widest) widest = column + 1;
	}
	return widest;
}

inline constexpr int paletteRowCount()
{
	int tallest = 0;
	for (int index = 0; index < static_cast<int>(PaletteSlot::Count); ++index)
	{
		auto row = paletteSlotRow(static_cast<PaletteSlot>(index));
		if (row + 1 > tallest) tallest = row + 1;
	}
	return tallest;
}

inline constexpr ImVec2 paletteTraySize()
{
	return ImVec2(PalettePadding * 2.0f
			+ PaletteSlotWidth * static_cast<float>(paletteColumnCount())
			+ PaletteGap * static_cast<float>(paletteColumnCount() - 1),
		PalettePadding * 2.0f
			+ PaletteSlotSize * static_cast<float>(paletteRowCount())
			+ PaletteGap * static_cast<float>(paletteRowCount() - 1));
}

inline constexpr ImVec2 paletteSlotMin(ImVec2 trayTopLeft, PaletteSlot slot)
{
	return ImVec2(trayTopLeft.x + PalettePadding
			+ static_cast<float>(paletteSlotColumn(slot)) * (PaletteSlotWidth + PaletteGap),
		trayTopLeft.y + PalettePadding
			+ static_cast<float>(paletteSlotRow(slot)) * (PaletteSlotSize + PaletteGap));
}

inline constexpr ImVec2 paletteSlotMax(ImVec2 trayTopLeft, PaletteSlot slot)
{
	return ImVec2(paletteSlotMin(trayTopLeft, slot).x + PaletteSlotWidth,
		paletteSlotMin(trayTopLeft, slot).y + PaletteSlotSize);
}
