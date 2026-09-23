// Vertical viewport culling, for ticket #58.
//
// The bounds query that feeds every render pass converted its upper Y bound
// with CORE_CELL_WIDTH_PIXELS instead of CORE_DECK_HEIGHT_PIXELS, and the
// renderer passed a hard-coded Y origin of 0 instead of the scrollbar's
// -yOffset. The over-wide initial window masked the missing origin for the
// first few rows; scroll far enough up a tall World and the Sectors that
// moved into view were culled - grid and blank space where Rooms should be.
//
// The checks pin down:
//
//   * a sub-deck-height bounds query at the origin does not reach deck 2 -
//     the ticket's probe, which the wrong divisor answered "2 sectors";
//   * viewportSectors() tracks a non-zero vertical offset: the Sectors above
//     the initial viewport come back, the scrolled-out ones below do not;
//   * the full scrollbar range works - the topmost deck of a tall World
//     is visible when scrolled to the bottom of the scrollbar;
//   * the real render pass paints the scrolled-in Sector and paints nothing
//     of the culled Sector below the viewport.
//
// Everything runs headless: ImGui is created without a renderer, so no
// window, dialog, or GPU is ever touched.

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "Render.h"
#include "UISettings.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Sector.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// The Solid-pass Layer fill renderSectors() passes down as ForeLocationColour.
	ImU32 const kForeLocationFill = ImU32(ImColor(192, 192, 255));

	// The scene: one Layer holding Rooms on the ground, on deck 2 (just above
	// a three-deck viewport's initial range), and on the topmost deck of an
	// eight-deck World.
	struct CullingScene
	{
		std::shared_ptr<core::World> world{ std::make_shared<core::World>(
			"Viewport culling", 12, 8) };
		uint32_t groundRoom{ 0 };
		uint32_t upperRoom{ 0 };
		uint32_t topRoom{ 0 };

		CullingScene()
		{
			while (world->getLayerCount() < 2) world->addLayer();

			groundRoom = world->addRoom("Ground", 0, 0, 0, 4, 1);
			upperRoom = world->addRoom("Upper", 0, 2, 0, 4, 1);
			topRoom = world->addRoom("Top", 0, 7, 0, 4, 1);

			world->finishBuild();
			world->pauseSimulation();
		}
	};

	// ImGui without a renderer: contexts are CPU-side only, nothing reaches a
	// window or the GPU.
	struct ImGuiGuard
	{
		ImGuiGuard() { ImGui::CreateContext(); }
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	// A three-deck-tall viewport at the origin, scrolled vertically by
	// scrollY pixels. The visible world band is [scrollY, scrollY + height].
	void setViewport(float scrollX, float scrollY, float width, float height)
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = width;
		gUISettings.worldViewportHeight = height;
		gUISettings.xOffset = -scrollX;
		gUISettings.yOffset = -scrollY;
	}

	// The screen-space rectangle a Sector's bounds land in under
	// transformPosition(), for the current gUISettings.
	struct ScreenRect
	{
		float minX, minY, maxX, maxY;
	};

	ScreenRect sectorScreenRect(core::World const& world, uint32_t sectorIndex)
	{
		core::Vector2 b0, b1;
		world.getSector(sectorIndex)->getBounds(b0, b1);

		auto const tx = [](float x)
		{
			return x * CORE_CELL_WIDTH_PIXELS + gUISettings.worldViewportX + gUISettings.xOffset;
		};
		auto const ty = [](float y)
		{
			return gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- y * CORE_DECK_HEIGHT_PIXELS - gUISettings.yOffset;
		};

		return {
			std::min(tx(b0.x), tx(b1.x)), std::min(ty(b0.y), ty(b1.y)),
			std::max(tx(b0.x), tx(b1.x)), std::max(ty(b0.y), ty(b1.y))
		};
	}

	// How many vertices of the given colour fall inside the rectangle.
	int verticesIn(ImDrawList const* drawList, ImU32 colour, ScreenRect const& rect)
	{
		int count = 0;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
		{
			auto const& v = drawList->VtxBuffer[i];
			if (v.col != colour) continue;
			if (v.pos.x >= rect.minX - 0.01f && v.pos.x <= rect.maxX + 0.01f
				&& v.pos.y >= rect.minY - 0.01f && v.pos.y <= rect.maxY + 0.01f)
			{
				++count;
			}
		}
		return count;
	}
}

// The ticket's probe: a 159-pixel-high query - less than one 160-pixel
// deck - at the origin must see deck 0 only. With the upper bound divided by
// CORE_CELL_WIDTH_PIXELS it reached deck 2 and returned the Upper Room too.
void subDeckHeightViewportExcludesDeckTwo()
{
	CullingScene scene;

	auto const sectors = scene.world->getSectorsInBounds(0, 0.0f, 0.0f,
		4 * CORE_CELL_WIDTH_PIXELS, CORE_DECK_HEIGHT_PIXELS - 1.0f);

	require(sectors.size() == 1,
		std::format("a sub-deck-height viewport at the origin returned {} sectors, expected 1",
			sectors.size()));
	require(sectors[0] == scene.world->getSector(scene.groundRoom),
		"the sub-deck-height viewport did not return the deck-0 Room it covers");
}

// With the scrollbar moved down (yOffset negative), the query must follow:
// the deck-2 Room scrolled into view comes back, the deck-0 Room scrolled
// out below the viewport does not.
void verticalOffsetTracksTheVisibleOrigin()
{
	CullingScene scene;

	// Three decks visible, scrolled so the band is [320, 800] world pixels:
	// deck 2 is in, deck 0 is out.
	setViewport(0.0f, 320.0f, 640.0f, 480.0f);

	auto const sectors = viewportSectors(scene.world, 0);

	bool upperIn = false, groundIn = false, topIn = false;
	for (auto const& sector : sectors)
	{
		if (sector == scene.world->getSector(scene.upperRoom)) upperIn = true;
		if (sector == scene.world->getSector(scene.groundRoom)) groundIn = true;
		if (sector == scene.world->getSector(scene.topRoom)) topIn = true;
	}

	require(upperIn, "the Room scrolled into view on deck 2 was culled (#58 regression)");
	require(!groundIn, "the deck-0 Room below the viewport was not culled");
	require(!topIn, "the deck-7 Room far above the viewport was not culled");
}

// The full scrollbar range: at the bottom of an eight-deck World's
// scroll, the topmost deck must be visible. A World "substantially
// taller than the World panel" is exactly what the ticket reproduced with.
void fullyScrolledTopDeckIsVisible()
{
	CullingScene scene;

	// scrollMax = 8 * 160 - 480 = 800: the band is [800, 1280], decks 5-7.
	setViewport(0.0f, 8.0f * CORE_DECK_HEIGHT_PIXELS - 480.0f, 640.0f, 480.0f);

	auto const sectors = viewportSectors(scene.world, 0);

	bool topIn = false, groundIn = false;
	for (auto const& sector : sectors)
	{
		if (sector == scene.world->getSector(scene.topRoom)) topIn = true;
		if (sector == scene.world->getSector(scene.groundRoom)) groundIn = true;
	}

	require(topIn, "the topmost deck was culled at the end of the scrollbar range");
	require(!groundIn, "the ground deck was still submitted at the end of the scrollbar range");
}

// The real render pass, not just the query: with a non-zero vertical offset
// the Solid pass paints the scrolled-in Sector's fill quad on screen and
// paints nothing of the culled Sector below the viewport.
void renderPassPaintsScrolledInSectorOnly()
{
	ImGuiGuard imgui;
	ImDrawList* drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());

	CullingScene scene;

	setViewport(0.0f, 320.0f, 640.0f, 480.0f);

	renderSectors(scene.world, 0, LayerRenderStyle::Solid, drawList);

	auto const upperRect = sectorScreenRect(*scene.world, scene.upperRoom);
	auto const groundRect = sectorScreenRect(*scene.world, scene.groundRoom);

	// The deck-2 Room fills as one quad (4 vertices) inside its on-screen rect.
	require(verticesIn(drawList, kForeLocationFill, upperRect) == 4,
		"the Solid pass painted no fill quad for the Room scrolled into view (#58 regression)");

	// The deck-0 Room sits below the viewport; none of its fill may be drawn.
	require(verticesIn(drawList, kForeLocationFill, groundRect) == 0,
		"the Solid pass painted the culled deck-0 Room below the viewport");
}

void runViewportCullingSmokeChecks()
{
	subDeckHeightViewportExcludesDeckTwo();
	verticalOffsetTracksTheVisibleOrigin();
	fullyScrolledTopDeckIsVisible();
	renderPassPaintsScrolledInSectorOnly();
}
