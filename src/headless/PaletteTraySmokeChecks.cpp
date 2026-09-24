// Palette tray layout and placement checks, for tickets #54 and #41.
//
// The tray geometry lives in PaletteLayout.h and is derived from the slot
// grid itself. These checks pin the compact two-row grid down: every slot
// must fit inside the tray with the tray's own padding to spare, neither row
// may contain a blank column, and no two slots may overlap.
//
// The tray is draggable by any part of itself that is not a button, so
// the checks also pin the placement rules down: a legal drag lands where
// it asked, an illegal one stops at the view window's edge, the tray can
// never be dragged clean out of the window, and the grip is exactly the
// tray minus its buttons.

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
			require(min.y >= TrayTopLeft.y + PaletteTopMargin,
				"palette slot intrudes on the tray's top margin");
			require(max.x <= bottomRight.x - PalettePadding,
				"palette slot intrudes on the tray's right padding");
			require(max.y <= bottomRight.y - PalettePadding,
				"palette slot intrudes on the tray's bottom padding");
		}
	}

	// The final two bottom-row tools must land inside the compact tray.
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

	// Both rows are compact: the bottom row starts with Agent, Marker, Door
	// and contains no blank columns.
	void rowsAreContiguous()
	{
		for (int index = static_cast<int>(PaletteSlot::Agent);
			index < static_cast<int>(PaletteSlot::Count); ++index)
		{
			auto const column = paletteSlotColumn(slotAt(index));
			require(column == index - static_cast<int>(PaletteSlot::Agent),
				"bottom row contains a blank column");
			require(paletteSlotRow(slotAt(index)) == 1,
				"bottom-row tool drifted off the bottom row");
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

	void sectorLabelsFitAndAreCentred()
	{
		char const* labels[]{ "Room", "Facade", "Corridor", "Background", "Ladder",
			"Stairwell", "Lift", "Shuttle", "Staircase" };
		for (int index = 0; index < 9; ++index)
		{
			auto const slot = slotAt(index);
			auto const minimum = paletteSlotMin(TrayTopLeft, slot);
			auto const maximum = paletteSlotMax(TrayTopLeft, slot);
			auto const glyphBounds = paletteLabelVisibleBounds(labels[index]);
			auto const origin = paletteLabelPosition(minimum, maximum, labels[index]);
			auto const visibleLeft = origin.x + glyphBounds.x;
			auto const visibleRight = origin.x + glyphBounds.y;
			require(visibleLeft >= minimum.x + 4.0f && visibleRight <= maximum.x - 4.0f,
				"sector palette label does not fit inside its slot");
			require((visibleLeft + visibleRight) * 0.5f
					== (minimum.x + maximum.x) * 0.5f,
				"sector palette label is not horizontally centred");
			require(origin.y + 8.0f == (minimum.y + maximum.y) * 0.5f,
				"sector palette label is not vertically centred");
		}
	}

	// The grid is compile-time geometry; pin the tray size so a future
	// slot addition that overflows fails loudly.
	static_assert(PaletteSlotWidth >= 96.0f,
		"palette slots are too narrow for their sector labels");
	static_assert(paletteColumnCount() == 9, "palette grid column count changed");
	static_assert(paletteRowCount() == 2, "palette grid row count changed");
	static_assert(paletteTraySize().x == PalettePadding * 2.0f
			+ PaletteSlotWidth * 9.0f + PaletteGap * 8.0f,
		"tray width no longer covers every slot column");
	static_assert(paletteTraySize().y == PaletteTopMargin + PalettePadding
			+ PaletteSlotSize * 2.0f + PaletteGap,
		"tray height no longer covers both slot rows and its top grip");
	static_assert(paletteSlotMax(ImVec2(0.0f, 0.0f), PaletteSlot::PlatformLift).x
			+ PalettePadding == paletteTraySize().x,
		"PlatformLift escapes the tray");

	// Ticket #41: the tray is dragged around the view window by its grip and
	// may never leave that window.

	// A drag that asks for a legal position gets exactly that position.
	void dragInsideTheCanvasKeepsTheRequestedPosition()
	{
		auto const size = paletteTraySize();
		ImVec2 const canvasMin{ 0.0f, 0.0f };
		ImVec2 const canvasSize{ size.x + 200.0f, size.y + 120.0f };
		ImVec2 const requested{ 37.0f, 11.0f };
		auto const clamped = paletteClampTopLeft(canvasMin, canvasSize, requested);
		require(clamped.x == requested.x && clamped.y == requested.y,
			"a tray dragged inside the canvas was moved anyway");
	}

	// A drag that asks for more than the window has stops at the edge.
	void dragPastAnEdgeStopsAtThatEdge()
	{
		auto const size = paletteTraySize();
		ImVec2 const canvasMin{ 10.0f, -30.0f };
		ImVec2 const canvasSize{ size.x + 200.0f, size.y + 120.0f };
		auto const clamped = paletteClampTopLeft(canvasMin, canvasSize,
			ImVec2(canvasMin.x - 500.0f, canvasMin.y + 5000.0f));
		require(clamped.x == canvasMin.x, "the tray was dragged past the left edge");
		require(clamped.y + size.y == canvasMin.y + canvasSize.y,
			"the tray was dragged past the bottom edge");
		auto const other = paletteClampTopLeft(canvasMin, canvasSize,
			ImVec2(canvasMin.x + 5000.0f, canvasMin.y - 5000.0f));
		require(other.x + size.x == canvasMin.x + canvasSize.x,
			"the tray was dragged past the right edge");
		require(other.y == canvasMin.y, "the tray was dragged past the top edge");
	}

	// The safety property: whatever the drag asks for, the tray still covers
	// part of the window, so the palette can always be dragged back.
	void trayAlwaysOverlapsTheCanvas()
	{
		auto const size = paletteTraySize();
		ImVec2 const canvasMin{ -40.0f, 25.0f };
		ImVec2 const canvasSize{ 900.0f, 600.0f };
		for (int step = -40; step <= 40; ++step)
		{
			auto const requested = ImVec2(canvasMin.x + static_cast<float>(step) * 60.0f,
				canvasMin.y + static_cast<float>(step) * 40.0f);
			auto const clamped = paletteClampTopLeft(canvasMin, canvasSize, requested);
			auto const overlaps = clamped.x < canvasMin.x + canvasSize.x
				&& clamped.x + size.x > canvasMin.x
				&& clamped.y < canvasMin.y + canvasSize.y
				&& clamped.y + size.y > canvasMin.y;
			require(overlaps, "the palette left the view window");
		}
	}

	// A window smaller than the tray has nowhere to hold it: the tray pins to
	// the window's top-left corner and the window clips the overflow.
	void aCanvasSmallerThanTheTrayClipsTheTray()
	{
		auto const size = paletteTraySize();
		ImVec2 const canvasMin{ 5.0f, 7.0f };
		ImVec2 const canvasSize{ size.x - 100.0f, size.y - 20.0f };
		auto const clamped = paletteClampTopLeft(canvasMin, canvasSize,
			ImVec2(canvasMin.x + 4000.0f, canvasMin.y + 4000.0f));
		require(clamped.x == canvasMin.x && clamped.y == canvasMin.y,
			"an oversized tray was not pinned to the window's top-left");
		require(clamped.x + size.x > canvasMin.x && clamped.y + size.y > canvasMin.y,
			"an oversized tray is hidden rather than clipped");
	}

	// The grip is the tray minus its buttons: the padding and the gaps
	// between slots. Every button stays a button.
	void paddingAndGapsAreTheGrip()
	{
		auto const size = paletteTraySize();
		require(!paletteButtonAt(TrayTopLeft,
				ImVec2(TrayTopLeft.x + 1.0f, TrayTopLeft.y + 1.0f)),
			"the tray's top-left padding is not a grip");
		require(!paletteButtonAt(TrayTopLeft,
				ImVec2(TrayTopLeft.x + size.x * 0.5f,
					TrayTopLeft.y + PaletteTopMargin * 0.5f)),
			"the tray's top margin is not an exposed drag grip");
		require(!paletteButtonAt(TrayTopLeft,
				ImVec2(TrayTopLeft.x + size.x - 1.0f, TrayTopLeft.y + size.y - 1.0f)),
			"the tray's bottom-right padding is not a grip");
		auto const rowGapY = TrayTopLeft.y + PaletteTopMargin + PaletteSlotSize
			+ PaletteGap * 0.5f;
		require(!paletteButtonAt(TrayTopLeft,
				ImVec2(TrayTopLeft.x + PalettePadding + PaletteSlotWidth * 0.5f, rowGapY)),
			"the gap between the tray rows is not a grip");
		auto const columnGapX = TrayTopLeft.x + PalettePadding + PaletteSlotWidth
			+ PaletteGap * 0.5f;
		require(!paletteButtonAt(TrayTopLeft,
				ImVec2(columnGapX, TrayTopLeft.y + PalettePadding + PaletteSlotSize * 0.5f)),
			"the gap between the tray columns is not a grip");
		for (int index = 0; index < static_cast<int>(PaletteSlot::Count); ++index)
		{
			auto const slot = slotAt(index);
			auto const centre = ImVec2(
				(paletteSlotMin(TrayTopLeft, slot).x + paletteSlotMax(TrayTopLeft, slot).x) * 0.5f,
				(paletteSlotMin(TrayTopLeft, slot).y + paletteSlotMax(TrayTopLeft, slot).y) * 0.5f);
			require(paletteButtonAt(TrayTopLeft, centre),
				"a palette button can be dragged as a grip");
		}
	}
}

void runPaletteTraySmokeChecks()
{
	everySlotFitsInsideTheTray();
	roomLadderAndPlatformLiftFitInsideTheTray();
	rowsAreContiguous();
	noTwoSlotsOverlap();
	sectorLabelsFitAndAreCentred();
	dragInsideTheCanvasKeepsTheRequestedPosition();
	dragPastAnEdgeStopsAtThatEdge();
	trayAlwaysOverlapsTheCanvas();
	aCanvasSmallerThanTheTrayClipsTheTray();
	paddingAndGapsAreTheGrip();
}
