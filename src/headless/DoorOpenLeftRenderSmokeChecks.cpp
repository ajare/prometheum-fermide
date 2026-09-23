// The real renderer's OpenLeft Door passes, for ticket #82.
//
// An OpenLeft Door's leaf slides toward decreasing world X: its right edge
// tracks the open percentage, so the aperture is vacated from the right edge
// and the back Sector is revealed right-to-left. This check drives the real
// renderSector() - which reaches renderDoor() and the OpenLeft branch - against
// a live ImDrawList at closed, partial, and fully open percentages, and reads
// the vertex buffer and draw-command clip rects back:
//
//   * the Solid leaf quad spans the aperture's full height and shrinks from
//     the right, hugging the left jamb, so it stays clipped to the full Door
//     aperture at every percentage;
//   * the back Sector's aperture fill is clipped from the leaf's right edge to
//     the aperture's right edge - the visible back region grows right-to-left
//     and fills the whole aperture at 100% open;
//   * the wireframe pass outlines only the remaining leaf geometry and adds
//     no solid aperture fill: no leaf-coloured or back-coloured vertices reach
//     the draw list at all.
//
// Everything runs headless: ImGui is created without a renderer, so no window,
// dialog, or GPU is ever touched.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "imgui/imgui.h"

#include "Render.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/Vector2.h"
#include "UISettings.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// The colours Render.cpp paints the OpenLeft Door with.
	ImU32 const kDoorLeafColour = ImU32(ImColor(64, 192, 255));      // renderDoor*'s leaf
	ImU32 const kApertureFillColour = ImU32(ImColor(224, 224, 255)); // BackLocationColour
	ImU32 const kThresholdOutlineColour = ImU32(ImColor(0, 0, 0));    // wireframe thresholds
	// The front Room's own surface fill. Deliberately not black, so the
	// wireframe threshold outlines stay distinguishable from it.
	ImColor const kFrontRoomColour(192, 192, 255);

	// The Door under test: a two-cell ordinary Door on Layer 0 at cell (3, 0),
	// with a Room directly behind it on Layer 1.
	struct DoorScene
	{
		std::unique_ptr<core::World> world;
		uint32_t frontSectorIndex{ 0 };
		std::shared_ptr<core::Door> door;
		// The Door aperture in world (cell) units.
		float worldX0{ 0.0f }, worldX2{ 0.0f };
	};

	// ImGui without a renderer: contexts are CPU-side only, nothing reaches a
	// window or the GPU.
	struct ImGuiGuard
	{
		ImGuiGuard() { ImGui::CreateContext(); }
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	// The same transforms transformPosition() applies, recomputed against the
	// current viewport settings so the checks can predict screen coordinates.
	float toScreenX(float worldX)
	{
		return worldX * CORE_CELL_WIDTH_PIXELS + gUISettings.worldViewportX + gUISettings.xOffset;
	}

	float toScreenY(float worldY)
	{
		return gUISettings.worldViewportY + gUISettings.worldViewportHeight
			- worldY * CORE_LEVEL_HEIGHT_PIXELS - gUISettings.yOffset;
	}

	// The leaf's right edge in world units: it slides toward decreasing X as
	// the Door opens, so the remaining leaf always hugs the left jamb.
	float leafRightWorldX(DoorScene const& scene)
	{
		auto const open = scene.door->getOpenPercentage();
		return scene.worldX2 - open * (scene.worldX2 - scene.worldX0);
	}

	struct ColourExtent
	{
		float minX{ FLT_MAX }, maxX{ -FLT_MAX };
		float minY{ FLT_MAX }, maxY{ -FLT_MAX };
		int vertexCount{ 0 };

		bool painted() const { return vertexCount > 0; }
	};

	ColourExtent extentOf(ImDrawList const* drawList, ImU32 colour)
	{
		ColourExtent extent;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
		{
			if (drawList->VtxBuffer[i].col != colour) continue;
			extent.minX = std::min(extent.minX, drawList->VtxBuffer[i].pos.x);
			extent.maxX = std::max(extent.maxX, drawList->VtxBuffer[i].pos.x);
			extent.minY = std::min(extent.minY, drawList->VtxBuffer[i].pos.y);
			extent.maxY = std::max(extent.maxY, drawList->VtxBuffer[i].pos.y);
			++extent.vertexCount;
		}
		return extent;
	}

	// The ClipRect of the draw command that first consumes a vertex of the
	// given colour - the clip the renderer painted that fill under.
	ImVec4 clipCovering(ImDrawList const* drawList, ImU32 colour)
	{
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
		{
			if (drawList->VtxBuffer[i].col != colour) continue;
			for (int p = 0; p < drawList->IdxBuffer.Size; ++p)
			{
				if (drawList->IdxBuffer[p] != static_cast<ImDrawIdx>(i)) continue;
				for (int c = 0; c < drawList->CmdBuffer.Size; ++c)
				{
					auto const& cmd = drawList->CmdBuffer[c];
					if (p >= static_cast<int>(cmd.IdxOffset)
						&& p < static_cast<int>(cmd.IdxOffset) + static_cast<int>(cmd.ElemCount))
						return cmd.ClipRect;
				}
				return ImVec4(0.0f, 0.0f, -1.0f, -1.0f);
			}
			return ImVec4(0.0f, 0.0f, -1.0f, -1.0f);
		}
		return ImVec4(0.0f, 0.0f, -1.0f, -1.0f);
	}

	DoorScene buildOpenLeftDoorScene()
	{
		DoorScene scene;
		scene.world = std::make_unique<core::World>("OpenLeft door render", 8, 3);
		auto& world = *scene.world;
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 7, 1);
		world.addRoom("Back", 1, 0, 0, 7, 1);

		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenLeft;
		auto const created = world.addSectorDoor(0, 0, 3, doorOptions);
		world.finishBuild();
		world.pauseSimulation();

		scene.frontSectorIndex = created.door.sector->getIndex();
		auto sector = world.getSector(scene.frontSectorIndex);
		require(sector != nullptr, "The Door's front Sector vanished");
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto const object = sector->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::Door)
			{
				scene.door = std::const_pointer_cast<core::Door>(
					std::static_pointer_cast<const core::DoorSectorObject>(object)->getDoor());
				break;
			}
		}
		require(scene.door != nullptr, "The ordinary Door could not be found");
		require(scene.door->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"The Door was not authored OpenLeft");

		core::Vector2 lo, hi;
		scene.door->getFullShape(lo, hi);
		scene.worldX0 = std::min(lo.x, hi.x);
		scene.worldX2 = std::max(lo.x, hi.x);
		return scene;
	}

	// Drive the Door to an exact open percentage through its own update loop.
	void driveTo(DoorScene const& scene, float targetPct)
	{
		auto& door = *scene.door;
		if (targetPct <= 0.0f)
		{
			require(door.getOpenPercentage() <= 0.0f,
				"A freshly built Door should start closed");
			return;
		}
		door.open();
		auto const step = door.getOpenCloseTime() * 0.25f;
		while (door.getOpenPercentage() < targetPct)
			door.update(step);
		require(door.getOpenPercentage() <= 1.0f, "The Door opened past fully open");
		if (targetPct >= 1.0f)
			require(door.isOpen(), "The Door did not reach the Open state");
	}

	// The viewport-qualified draw list: the plain form needs a current window,
	// which headless never has.
	ImDrawList* testDrawList()
	{
		// Give the transform a non-degenerate viewport so pushed clip rects
		// survive intersection with the on-screen rect.
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 1280.0f;
		gUISettings.worldViewportHeight = 720.0f;
		return ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
	}

	void renderPass(DoorScene const& scene, LayerRenderStyle style, ImDrawList* drawList)
	{
		// Outside a framed ImGui::Render() the background draw list carries an
		// empty clip; give the pass the viewport rect a real frame would, so the
		// renderer's own PushClipRect intersects against something sane.
		drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f), false);
		renderSector(scene.world->getSector(scene.frontSectorIndex), 0, style, false,
			kFrontRoomColour, drawList);
		drawList->PopClipRect();
	}

	constexpr float kEpsilon = 0.01f;
	// AddRect's anti-aliasing spreads the outline a hair beyond the exact rect,
	// so the wireframe predictions get a slightly wider tolerance.
	constexpr float kWireframeEpsilon = 1.5f;

	void near(float actual, float expected, char const* what, float epsilon = kEpsilon)
	{
		require(std::abs(actual - expected) < epsilon,
			std::string(what) + " is not where the OpenLeft geometry predicts"
			+ " (expected " + std::to_string(expected) + ", got " + std::to_string(actual) + ")");
	}

	// Solid pass at a given percentage: the leaf hugs the left jamb and spans
	// the aperture's full height; the back Sector's fill is clipped from the
	// leaf's right edge to the aperture's right edge.
	void checkSolidPass(float openPct)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const leafRight = leafRightWorldX(scene);
		auto const apertureTop = toScreenY(CORE_DOOR_HEIGHT);
		auto const apertureBottom = toScreenY(0.0f);

		// The leaf: solid quad from the left jamb to its sliding right edge,
		// over the full aperture height - never outside the aperture.
		auto const leaf = extentOf(drawList, kDoorLeafColour);
		require(leaf.painted(), "the Solid pass painted no Door leaf");
		near(leaf.minX, toScreenX(scene.worldX0), "the leaf's left edge");
		near(leaf.maxX, toScreenX(leafRight), "the leaf's sliding right edge");
		near(leaf.minY, apertureTop, "the leaf's top edge");
		near(leaf.maxY, apertureBottom, "the leaf's bottom edge");

		// The back Sector's aperture fill: clipped to the vacated region only.
		// Its visible width grows from zero at closed to the whole aperture at
		// fully open, always anchored to the aperture's right edge.
		auto const back = extentOf(drawList, kApertureFillColour);
		require(back.painted(), "no aperture fill reached the Sector behind");
		auto const clip = clipCovering(drawList, kApertureFillColour);
		near(clip.x, toScreenX(leafRight), "the aperture clip's left edge");
		near(clip.z, toScreenX(scene.worldX2), "the aperture clip's right edge");
		near(clip.y, apertureTop, "the aperture clip's top edge");
		near(clip.w, apertureBottom, "the aperture clip's bottom edge");
	}

	// Fully open: the leaf has slid off the aperture's left jamb and paints no
	// fill at all; the back Sector's clip covers the whole aperture.
	void checkFullyOpenSolidPass()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		driveTo(scene, 1.0f);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		require(!extentOf(drawList, kDoorLeafColour).painted(),
			"the fully-open Solid pass painted a degenerate Door leaf");

		auto const apertureTop = toScreenY(CORE_DOOR_HEIGHT);
		auto const apertureBottom = toScreenY(0.0f);

		auto const back = extentOf(drawList, kApertureFillColour);
		require(back.painted(), "no aperture fill reached the Sector behind");
		auto const clip = clipCovering(drawList, kApertureFillColour);
		near(clip.x, toScreenX(scene.worldX0), "the aperture clip's left edge");
		near(clip.z, toScreenX(scene.worldX2), "the aperture clip's right edge");
		near(clip.y, apertureTop, "the aperture clip's top edge");
		near(clip.w, apertureBottom, "the aperture clip's bottom edge");
	}

	// Wireframe pass at a given percentage: the remaining leaf is outlined and
	// nothing is filled - no leaf colour, no back Sector colour.
	void checkWireframePass(float openPct)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Wireframe, drawList);

		auto const leafRight = leafRightWorldX(scene);

		auto const outline = extentOf(drawList, kThresholdOutlineColour);
		require(outline.painted(), "the wireframe pass drew no Door outline");
		near(outline.minX, toScreenX(scene.worldX0), "the outlined leaf's left edge",
			kWireframeEpsilon);
		near(outline.maxX, toScreenX(leafRight), "the outlined leaf's sliding right edge",
			kWireframeEpsilon);
		near(outline.minY, toScreenY(CORE_DOOR_HEIGHT), "the outlined leaf's top edge",
			kWireframeEpsilon);
		near(outline.maxY, toScreenY(0.0f), "the outlined leaf's bottom edge",
			kWireframeEpsilon);

		require(!extentOf(drawList, kDoorLeafColour).painted(),
			"the wireframe pass filled the leaf solid");
		require(!extentOf(drawList, kApertureFillColour).painted(),
			"the wireframe pass filled the aperture");
	}

	// Fully open: the leaf has slid off the aperture's left jamb, so the
	// wireframe pass outlines nothing at all - no stroked zero-width rectangle
	// at the jamb - and nothing is filled.
	void checkFullyOpenWireframePass()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		driveTo(scene, 1.0f);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Wireframe, drawList);

		require(!extentOf(drawList, kThresholdOutlineColour).painted(),
			"the fully-open wireframe pass drew a leaf outline at the jamb");
		require(!extentOf(drawList, kDoorLeafColour).painted(),
			"the fully-open wireframe pass filled the leaf solid");
		require(!extentOf(drawList, kApertureFillColour).painted(),
			"the fully-open wireframe pass filled the aperture");
	}
}

void runDoorOpenLeftRenderSmokeChecks()
{
	// Closed: the leaf fills the whole aperture; nothing behind shows through.
	checkSolidPass(0.0f);
	checkWireframePass(0.0f);

	// Partial: the leaf has slid left, the back Sector is revealed from the
	// right edge to the leaf's right edge.
	checkSolidPass(0.5f);
	checkWireframePass(0.5f);

	// Fully open: the leaf has slid off the aperture's left jamb; the back
	// Sector's clip covers the whole aperture, and no leaf geometry remains.
	checkFullyOpenSolidPass();
	checkFullyOpenWireframePass();
}
