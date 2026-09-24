#pragma once

#include "imgui/imgui.h"

// Geometry of the editor's object-palette tray (ticket #54).
//
// The tray is a compact two-row grid. The top row holds the nine space and
// transit tools; the bottom row holds the nine Agents, Markers, and objects
// that attach to already-placed Sectors. Neither row contains blank slots.

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
inline constexpr float PaletteSlotWidth{ 96.0f };
inline constexpr float PaletteGap{ 6.0f };
inline constexpr float PalettePadding{ 6.0f };
inline constexpr float PaletteTopMargin{ 14.0f }; // exposed grip above the buttons

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
	case PaletteSlot::Marker: return 1;
	case PaletteSlot::Door: return 2;
	case PaletteSlot::BulkheadDoor: return 3;
	case PaletteSlot::Window: return 4;
	case PaletteSlot::Walkway: return 5;
	case PaletteSlot::ForceBridge: return 6;
	case PaletteSlot::RoomLadder: return 7;
	case PaletteSlot::PlatformLift: return 8;
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
		PaletteTopMargin + PalettePadding
			+ PaletteSlotSize * static_cast<float>(paletteRowCount())
			+ PaletteGap * static_cast<float>(paletteRowCount() - 1));
}

inline constexpr ImVec2 paletteSlotMin(ImVec2 trayTopLeft, PaletteSlot slot)
{
	return ImVec2(trayTopLeft.x + PalettePadding
			+ static_cast<float>(paletteSlotColumn(slot)) * (PaletteSlotWidth + PaletteGap),
		trayTopLeft.y + PaletteTopMargin
			+ static_cast<float>(paletteSlotRow(slot)) * (PaletteSlotSize + PaletteGap));
}

inline constexpr ImVec2 paletteSlotMax(ImVec2 trayTopLeft, PaletteSlot slot)
{
	return ImVec2(paletteSlotMin(trayTopLeft, slot).x + PaletteSlotWidth,
		paletteSlotMin(trayTopLeft, slot).y + PaletteSlotSize);
}

// MPP's canvas text renderer uses a fixed 16-pixel font. Its renderText()
// origin includes an eight-pixel lead-in and half of each glyph's negative
// kerning, so ImGui::CalcTextSize cannot centre these labels correctly.
inline constexpr float paletteLabelKern(char character)
{
	return character == 'f' ? -9.0f
		: character == 'i' || character == 'j' || character == 'r' ? -10.0f
		: -8.0f;
}

inline constexpr ImVec2 paletteLabelVisibleBounds(char const* label)
{
	float cursor = 8.0f;
	float left = 0.0f;
	float right = 0.0f;
	for (int index = 0; label[index]; ++index)
	{
		auto const halfKern = paletteLabelKern(label[index]) * 0.5f;
		cursor += halfKern;
		if (index == 0) left = cursor;
		right = cursor + 16.0f;
		cursor += 16.0f + halfKern;
	}
	return { left, right };
}

inline constexpr ImVec2 paletteLabelPosition(ImVec2 slotMin, ImVec2 slotMax,
	char const* label)
{
	auto const bounds = paletteLabelVisibleBounds(label);
	auto const visibleWidth = bounds.y - bounds.x;
	return {
		slotMin.x + ((slotMax.x - slotMin.x) - visibleWidth) * 0.5f - bounds.x,
		slotMin.y + ((slotMax.y - slotMin.y) - 16.0f) * 0.5f
	};
}

// Where a dragged tray ends up, kept inside the view canvas (ticket #41).
//
// The tray is clamped to the canvas so it can never be dragged out of reach.
// A canvas smaller than the tray has no room to hold it, so the clamp
// collapses onto the canvas' top-left corner: the tray stays anchored there
// and the canvas clips its overflow instead of the tray sliding away.
inline constexpr ImVec2 paletteClampTopLeft(ImVec2 canvasMin, ImVec2 canvasSize,
	ImVec2 trayTopLeft)
{
	auto const traySize = paletteTraySize();
	auto const clampAxis = [](float value, float limitMin, float limitMax)
	{
		return value < limitMin ? limitMin : (value > limitMax ? limitMax : value);
	};
	auto const slackX = canvasSize.x - traySize.x;
	auto const slackY = canvasSize.y - traySize.y;
	return ImVec2(clampAxis(trayTopLeft.x, canvasMin.x,
			canvasMin.x + (slackX > 0.0f ? slackX : 0.0f)),
		clampAxis(trayTopLeft.y, canvasMin.y,
			canvasMin.y + (slackY > 0.0f ? slackY : 0.0f)));
}

// True when a button slot covers the point. Everything else inside the tray -
// its padding and the gaps between slots - is the grip the user drags the
// palette by (ticket #41).
inline constexpr bool paletteButtonAt(ImVec2 trayTopLeft, ImVec2 point)
{
	for (int index = 0; index < static_cast<int>(PaletteSlot::Count); ++index)
	{
		auto const slot = static_cast<PaletteSlot>(index);
		auto const min = paletteSlotMin(trayTopLeft, slot);
		auto const max = paletteSlotMax(trayTopLeft, slot);
		if (point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y)
			return true;
	}
	return false;
}
