// Palette tray layout checks, for ticket #54.
//
// The palette tray was previously sized for exactly nine slots while the
// bottom row - anchored to the Ladder column so that Door sits under
// Ladder - carried its tail (RoomLadder, PlatformLift) past the tray's
// right edge, so those buttons drew outside the tray background. The
// tray geometry now lives in PaletteLayout.h and is derived from the
// slot grid itself. These checks pin the grid down: every slot must fit
// inside the tray with the tray's own padding to spare, the bottom row
// must keep its column alignment under the top row, and no two slots
// may overlap.

#include <stdexcept>

#include "PaletteLayout.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	ImVec2 const TrayTopLeft{ 120.0f, 80.0f };

	ImVec2 trayBottomRight()
	{
		auto const size = paletteTraySize();
		return ImVec2(TrayTopLeft.x + size.x, TrayTopLeft.y + size.y);
	}

	PaletteSlot slotAt(int index)
	{
		return static_cast<PaletteSlot>(index);
	}

	// The core #54 invariant: no slot draws outside the tray background.
	void everySlotFitsInsideTheTray()
	{
		auto const bottomRight = trayBottomRight();
		for (int index = 0; index < static_cast<int>(PaletteSlot::Count); ++index)
		{
			auto const slot = slotAt(index);
			auto const min = paletteSlotMin(TrayTopLeft, slot);
			auto const max = paletteSlotMax(TrayTopLeft, slot);
			require(min.x >= TrayTopLeft.x + PalettePadding,
				"palette slot intrudes on the tray's left padding");
			require(min.y >= TrayTopLeft.y + PalettePadding,
				"palette slot intrudes on the tray's top padding");
			require(max.x <= bottomRight.x - PalettePadding,
				"palette slot intrudes on the tray's right padding");
			require(max.y <= bottomRight.y - PalettePadding,
				"palette slot intrudes on the tray's bottom padding");
		}
	}

	// The regression itself: the bottom row's tail must land inside the tray.
	void roomLadderAndPlatformLiftFitInsideTheTray()
	{
		auto const bottomRight = trayBottomRight();
		auto const roomLadderMax = paletteSlotMax(TrayTopLeft, PaletteSlot::RoomLadder);
		auto const platformLiftMax = paletteSlotMax(TrayTopLeft, PaletteSlot::PlatformLift);
		require(roomLadderMax.x <= bottomRight.x - PalettePadding,
			"RoomLadder still draws outside the tray");
		require(platformLiftMax.x <= bottomRight.x - PalettePadding,
			"PlatformLift still draws outside the tray");
		// The tray is sized to the grid, so the tail slot fills the last column.
		require(platformLiftMax.x + PalettePadding == bottomRight.x,
			"tray width no longer matches the slot grid's widest row");
	}

	// The bottom row keeps the top row's column grid: Agent under Room,
	// Marker under Corridor, Door under Ladder.
	void bottomRowKeepsTopRowColumnAlignment()
	{
		require(paletteSlotColumn(PaletteSlot::Agent) == paletteSlotColumn(PaletteSlot::Room),
			"Agent no longer sits under Room");
		require(paletteSlotColumn(PaletteSlot::Marker) == paletteSlotColumn(PaletteSlot::Corridor),
			"Marker no longer sits under Corridor");
		require(paletteSlotColumn(PaletteSlot::Door) == paletteSlotColumn(PaletteSlot::Ladder),
			"Door no longer sits under Ladder");
		require(paletteSlotRow(PaletteSlot::Agent) == 1
				&& paletteSlotRow(PaletteSlot::Marker) == 1
				&& paletteSlotRow(PaletteSlot::Door) == 1,
			"Agent, Marker, and Door belong to the bottom row");
	}

	// From Door rightward the bottom row is contiguous, and the top row
	// fills columns 0..8 without gaps.
	void rowsAreContiguous()
	{
		int previousColumn = paletteSlotColumn(PaletteSlot::Door);
		for (int index = static_cast<int>(PaletteSlot::BulkheadDoor);
			index < static_cast<int>(PaletteSlot::Count); ++index)
		{
			auto const column = paletteSlotColumn(slotAt(index));
			require(column == previousColumn + 1,
				"bottom row is no longer contiguous from Door rightward");
			previousColumn = column;
		}
		for (int index = 0; index <= static_cast<int>(PaletteSlot::Staircase); ++index)
		{
			require(paletteSlotColumn(slotAt(index)) == index,
				"top row is no longer contiguous from Room rightward");
			require(paletteSlotRow(slotAt(index)) == 0,
				"top-row tool drifted off the top row");
		}
	}

	void noTwoSlotsOverlap()
	{
		for (int left = 0; left < static_cast<int>(PaletteSlot::Count); ++left)
		{
			for (int right = left + 1; right < static_cast<int>(PaletteSlot::Count); ++right)
			{
				auto const leftMin = paletteSlotMin(TrayTopLeft, slotAt(left));
				auto const leftMax = paletteSlotMax(TrayTopLeft, slotAt(left));
				auto const rightMin = paletteSlotMin(TrayTopLeft, slotAt(right));
				auto const rightMax = paletteSlotMax(TrayTopLeft, slotAt(right));
				auto const disjoint = leftMax.x <= rightMin.x || rightMax.x <= leftMin.x
					|| leftMax.y <= rightMin.y || rightMax.y <= leftMin.y;
				require(disjoint, "two palette slots overlap");
			}
		}
	}

	// The grid is compile-time geometry; pin the tray size so a future
	// slot addition that overflows fails loudly.
	static_assert(paletteColumnCount() == 11, "palette grid column count changed");
	static_assert(paletteRowCount() == 2, "palette grid row count changed");
	static_assert(paletteTraySize().x == PalettePadding * 2.0f
			+ PaletteSlotWidth * 11.0f + PaletteGap * 10.0f,
		"tray width no longer covers every slot column");
	static_assert(paletteTraySize().y == PalettePadding * 2.0f
			+ PaletteSlotSize * 2.0f + PaletteGap,
		"tray height no longer covers both slot rows");
	static_assert(paletteSlotMax(ImVec2(0.0f, 0.0f), PaletteSlot::PlatformLift).x
			+ PalettePadding == paletteTraySize().x,
		"PlatformLift escapes the tray");
}

void runPaletteTraySmokeChecks()
{
	everySlotFitsInsideTheTray();
	roomLadderAndPlatformLiftFitInsideTheTray();
	bottomRowKeepsTopRowColumnAlignment();
	rowsAreContiguous();
	noTwoSlotsOverlap();
}
