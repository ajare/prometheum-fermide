// The real renderer's OpenApart Door passes, for ticket #84.
//
// An OpenApart Door is one logical Door that renders as two equal leaves split at
// the aperture midpoint. Each leaf travels outward by half an aperture width over
// the open percentage, so:
//
//   * the two leaves always stay equal and symmetric about the aperture midpoint;
//   * the leaves' inner edges are drawn - coincident as a centre seam while the
//     Door is closed, separating into two visible facing edges while it opens;
//   * the back Sector is exposed only through the centred gap between the inner
//     edges, which becomes the whole aperture at 100% open;
//   * the leaf fill is clipped to the full Door aperture, so a sliding leaf never
//     paints over the surrounding Location;
//   * the wireframe pass outlines both remaining leaves and adds no solid fill.
//
// This check drives the real renderSector() - which reaches renderDoor() and the
// OpenApart branch - against a live ImDrawList at closed, partial, and fully open
// percentages, and reads the emitted quads, vertex colours, and draw-command clip
// rects back out of the draw list.
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
#include <vector>

#include "imgui/imgui.h"

#include "Render.h"
#include "core/Building.h"
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

	// The colours Render.cpp paints an OpenApart Door with.
	ImU32 const kDoorLeafColour = ImU32(ImColor(64, 192, 255));      // renderDoor*'s leaves
	ImU32 const kApertureFillColour = ImU32(ImColor(224, 224, 255)); // BackLocationColour
	ImU32 const kSeamColour = ImU32(ImColor(0, 0, 0));               // inner-edge/centre seam
	// The front Room's own surface fill. Deliberately not black, so the seam and
	// the wireframe outlines stay distinguishable from it.
	ImColor const kFrontRoomColour(192, 192, 255);

	// The Door under test: an ordinary OpenApart Door on Layer 0 at cell (3, 0),
	// with a Room directly behind it on Layer 1.
	struct DoorScene
	{
		std::unique_ptr<core::Building> building;
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
			- worldY * CORE_DECK_HEIGHT_PIXELS - gUISettings.yOffset;
	}

	// The leaf geometry the renderer should produce for the Door's current open
	// percentage, in world units.
	struct LeafPrediction
	{
		bool visible{ false };
		float leftX0{ 0.0f }, leftX1{ 0.0f };
		float rightX0{ 0.0f }, rightX1{ 0.0f };
	};

	LeafPrediction predictLeaves(DoorScene const& scene)
	{
		auto const halfWidth = (scene.worldX2 - scene.worldX0) * 0.5f;
		auto const midX = scene.worldX0 + halfWidth;
		auto const travel = scene.door->getOpenPercentage() * halfWidth;
		LeafPrediction prediction;
		prediction.leftX0 = midX - halfWidth - travel;
		prediction.leftX1 = midX - travel;
		prediction.rightX0 = midX + travel;
		prediction.rightX1 = midX + halfWidth + travel;
		// A leaf paints only while some part of it is still inside the aperture.
		prediction.visible = prediction.leftX1 > scene.worldX0
			&& prediction.rightX0 < scene.worldX2;
		return prediction;
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

	struct Rect
	{
		float minX{ 0.0f }, maxX{ 0.0f }, minY{ 0.0f }, maxY{ 0.0f };

		float width() const { return maxX - minX; }
	};

	// The filled/outlined rectangles of one colour: each AddRectFilled or AddRect
	// contributes four consecutive vertices, so grouping them in fours recovers
	// the individual quads the renderer emitted.
	std::vector<Rect> rectsOf(ImDrawList const* drawList, ImU32 colour)
	{
		std::vector<int> indices;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
			if (drawList->VtxBuffer[i].col == colour) indices.push_back(i);
		require(indices.size() % 4 == 0,
			"vertices of one colour did not group into whole rectangles");
		std::vector<Rect> rects;
		for (size_t start = 0; start < indices.size(); start += 4)
		{
			Rect rect;
			rect.minX = rect.maxX = drawList->VtxBuffer[indices[start]].pos.x;
			rect.minY = rect.maxY = drawList->VtxBuffer[indices[start]].pos.y;
			for (size_t i = start; i < start + 4; ++i)
			{
				auto const& v = drawList->VtxBuffer[indices[i]];
				rect.minX = std::min(rect.minX, v.pos.x);
				rect.maxX = std::max(rect.maxX, v.pos.x);
				rect.minY = std::min(rect.minY, v.pos.y);
				rect.maxY = std::max(rect.maxY, v.pos.y);
			}
			rects.push_back(rect);
		}
		return rects;
	}

	// The X of every vertex painted in the given colour - the seam is a pair of
	// vertical lines, so its position is fully described by its vertices' X.
	std::vector<float> xsOf(ImDrawList const* drawList, ImU32 colour)
	{
		std::vector<float> xs;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
			if (drawList->VtxBuffer[i].col == colour) xs.push_back(drawList->VtxBuffer[i].pos.x);
		return xs;
	}

	bool anyNear(std::vector<float> const& values, float target, float epsilon)
	{
		return std::any_of(values.begin(), values.end(),
			[&](float value) { return std::abs(value - target) < epsilon; });
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

	DoorScene buildOpenApartDoorScene(uint32_t cellsWide)
	{
		DoorScene scene;
		scene.building = std::make_unique<core::Building>("OpenApart door render", 8, 3);
		auto& building = *scene.building;
		while (building.getLayerCount() < 2) building.addLayer();
		building.addRoom("Front", 0, 0, 0, 7, 1);
		building.addRoom("Back", 1, 0, 0, 7, 1);

		core::Building::CreateDoorOptions doorOptions;
		doorOptions.width = cellsWide;
		doorOptions.openStyle = core::Door::OpenStyle::OpenApart;
		auto const created = building.addSectorDoor(0, 0, 3, doorOptions);
		building.finishBuild();
		building.pauseSimulation();

		scene.frontSectorIndex = created.door.sector->getIndex();
		auto sector = building.getSector(scene.frontSectorIndex);
		require(sector != nullptr, "The Door's front Sector vanished");
		uint32_t doorObjects{ 0 };
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto const object = sector->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::Door)
			{
				++doorObjects;
				if (!scene.door)
				{
					scene.door = std::const_pointer_cast<core::Door>(
						std::static_pointer_cast<const core::DoorSectorObject>(object)->getDoor());
				}
			}
		}
		require(scene.door != nullptr, "The ordinary Door could not be found");
		require(doorObjects == 1,
			"An OpenApart Door rendered as more than one logical Door object");
		require(scene.door->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"The Door was not authored OpenApart");

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
		renderSector(scene.building->getSector(scene.frontSectorIndex), 0, style, false,
			kFrontRoomColour, drawList);
		drawList->PopClipRect();
	}

	constexpr float kEpsilon = 0.01f;
	// AddRect's anti-aliasing spreads the outline a hair beyond the exact rect,
	// so the wireframe predictions get a slightly wider tolerance.
	constexpr float kWireframeEpsilon = 1.5f;
	// ImDrawList::AddLine offsets its path by half a pixel.
	constexpr float kSeamEpsilon = 1.5f;

	void near(float actual, float expected, char const* what, float epsilon = kEpsilon)
	{
		require(std::abs(actual - expected) < epsilon,
			std::string(what) + " is not where the OpenApart geometry predicts"
			+ " (expected " + std::to_string(expected) + ", got " + std::to_string(actual) + ")");
	}

	// Solid pass at a given percentage: two equal leaves symmetric about the
	// aperture midpoint, their inner edges drawn, their fill clipped to the full
	// aperture, and the back Sector exposed only through the centred gap.
	void checkSolidPass(uint32_t cellsWide, float openPct)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenApartDoorScene(cellsWide);
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const leaves = predictLeaves(scene);
		auto const apertureTop = toScreenY(CORE_DOOR_HEIGHT);
		auto const apertureBottom = toScreenY(0.0f);
		auto const centre = toScreenX((scene.worldX0 + scene.worldX2) * 0.5f);
		auto const apertureWidth = toScreenX(scene.worldX2) - toScreenX(scene.worldX0);

		auto const leafRects = rectsOf(drawList, kDoorLeafColour);
		if (!leaves.visible)
		{
			require(leafRects.empty(),
				"a fully open OpenApart Door still painted leaf geometry");
		}
		else
		{
			require(leafRects.size() == 2,
				"an OpenApart Door did not render as two leaves");
			auto const& left = leafRects[0];
			auto const& right = leafRects[1];

			// Two equal leaves, each spanning the aperture's full height.
			near(left.width(), right.width(), "the two leaves are not equal width");
			for (auto const& leaf : { left, right })
			{
				near(leaf.minY, apertureTop, "a leaf's top edge");
				near(leaf.maxY, apertureBottom, "a leaf's bottom edge");
			}

			// Each leaf sits where its outward travel predicts.
			near(left.minX, toScreenX(leaves.leftX0), "the left leaf's outer edge");
			near(left.maxX, toScreenX(leaves.leftX1), "the left leaf's inner edge");
			near(right.minX, toScreenX(leaves.rightX0), "the right leaf's inner edge");
			near(right.maxX, toScreenX(leaves.rightX1), "the right leaf's outer edge");

			// The pair stays symmetric about the aperture midpoint.
			near((left.minX + right.maxX) * 0.5f, centre,
				"the leaf pair is not symmetric about the aperture midpoint");

			// The leaves slide out of the Door, so their fill is clipped to the
			// full aperture and never paints the surrounding Location.
			auto const leafClip = clipCovering(drawList, kDoorLeafColour);
			near(leafClip.x, toScreenX(scene.worldX0),
				"the leaf fill is not clipped to the aperture's left edge");
			near(leafClip.z, toScreenX(scene.worldX2),
				"the leaf fill is not clipped to the aperture's right edge");
			near(leafClip.y, apertureTop, "the leaf clip's top edge");
			near(leafClip.w, apertureBottom, "the leaf clip's bottom edge");
		}

		// The inner edges stay drawn: coincident as the centre seam while closed,
		// two facing edges while part open, gone once the leaves have left.
		auto const seamXs = xsOf(drawList, kSeamColour);
		if (!leaves.visible)
		{
			require(seamXs.empty(), "a fully open OpenApart Door still drew a seam");
		}
		else
		{
			require(anyNear(seamXs, toScreenX(leaves.leftX1), kSeamEpsilon),
				"the left leaf's inner edge was not outlined");
			require(anyNear(seamXs, toScreenX(leaves.rightX0), kSeamEpsilon),
				"the right leaf's inner edge was not outlined");
			if (openPct <= 0.0f)
			{
				// Closed: the two inner edges meet at the midpoint as one seam.
				require(std::abs(toScreenX(leaves.leftX1) - centre) < kEpsilon
					&& std::abs(toScreenX(leaves.rightX0) - centre) < kEpsilon,
					"a closed OpenApart Door's seam is not at the aperture midpoint");
			}
		}

		// The back Sector shows only through the centred gap between the leaves:
		// it grows symmetrically from the midpoint and becomes the whole aperture
		// at 100% open.
		auto const back = extentOf(drawList, kApertureFillColour);
		require(back.painted(), "no aperture fill reached the Sector behind");
		auto const clip = clipCovering(drawList, kApertureFillColour);
		near(clip.x, toScreenX(leaves.leftX1), "the exposed gap's left edge");
		near(clip.z, toScreenX(leaves.rightX0), "the exposed gap's right edge");
		near(clip.y, apertureTop, "the gap clip's top edge");
		near(clip.w, apertureBottom, "the gap clip's bottom edge");
		near((clip.x + clip.z) * 0.5f, centre, "the exposed gap is not centred");
		near(clip.z - clip.x, openPct * apertureWidth,
			"the exposed gap does not track the open percentage");
	}

	// Wireframe pass at a given percentage: both remaining leaves are outlined
	// separately and nothing is filled.
	void checkWireframePass(uint32_t cellsWide, float openPct)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenApartDoorScene(cellsWide);
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Wireframe, drawList);

		auto const leaves = predictLeaves(scene);
		auto const outline = extentOf(drawList, kSeamColour);
		auto const gapLeft = toScreenX(leaves.leftX1);
		auto const gapRight = toScreenX(leaves.rightX0);

		if (!leaves.visible)
		{
			require(!outline.painted(),
				"a fully open OpenApart Door still outlined leaves in wireframe");
		}
		else
		{
			// The two leaf outlines together span the two remaining leaves: from
			// the left leaf's outer edge to the right leaf's outer edge, over the
			// aperture's full height.
			require(outline.painted(), "the wireframe pass drew no leaf outlines");
			near(outline.minX, toScreenX(leaves.leftX0),
				"the outlined leaves' left edge", kWireframeEpsilon);
			near(outline.maxX, toScreenX(leaves.rightX1),
				"the outlined leaves' right edge", kWireframeEpsilon);
			near(outline.minY, toScreenY(CORE_DOOR_HEIGHT),
				"the outlined leaves' top edge", kWireframeEpsilon);
			near(outline.maxY, toScreenY(0.0f),
				"the outlined leaves' bottom edge", kWireframeEpsilon);

			// Each leaf's own inner edge is outlined, and nothing is drawn between
			// them: the pair cannot be one rectangle thrown over the whole aperture.
			auto const outlineXs = xsOf(drawList, kSeamColour);
			require(anyNear(outlineXs, gapLeft, kWireframeEpsilon),
				"the outlined left leaf's inner edge is missing");
			require(anyNear(outlineXs, gapRight, kWireframeEpsilon),
				"the outlined right leaf's inner edge is missing");
			for (auto const x : outlineXs)
				require(!(x > gapLeft + kWireframeEpsilon && x < gapRight - kWireframeEpsilon),
					"the wireframe pass drew inside the OpenApart gap, so the two"
					" leaves are not outlined separately");
		}

		require(!extentOf(drawList, kDoorLeafColour).painted(),
			"the wireframe pass filled the leaves solid");
		require(!extentOf(drawList, kApertureFillColour).painted(),
			"the wireframe pass filled the aperture");
	}
}

void runDoorOpenApartRenderSmokeChecks()
{
	// Closed: the leaves meet at the midpoint as one visible centre seam; the
	// exposed gap is zero-width.
	checkSolidPass(2, 0.0f);
	checkWireframePass(2, 0.0f);

	// Partial: the leaves have separated, keeping their inner edges visible, and
	// the centred gap has grown to half the aperture.
	checkSolidPass(2, 0.5f);
	checkWireframePass(2, 0.5f);

	// Fully open: both leaves have slid out of the aperture, so no leaf geometry
	// or seam remains, and the gap is the whole aperture.
	checkSolidPass(2, 1.0f);
	checkWireframePass(2, 1.0f);

	// A one-cell Door splits at a fractional midpoint, so OpenApart needs no
	// minimum authored width.
	checkSolidPass(1, 0.0f);
	checkSolidPass(1, 0.5f);
	checkSolidPass(1, 1.0f);
	checkWireframePass(1, 0.5f);
}
