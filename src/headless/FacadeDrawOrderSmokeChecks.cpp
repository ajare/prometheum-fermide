// The real renderSector() draw-call order for a Facade hosting thresholds,
// for ticket #49.
//
// A Facade's flat fill used to be emitted in the type switch, after the
// BEHIND object pass - so a Door or Window authored on a Facade was drawn
// and then painted over by the opaque fill. The policy-model checks in
// FacadeRenderSmokeChecks.cpp could not see that: they replay the surface
// decisions, not the z-order. This check executes the real renderer against
// a live ImDrawList and reads the vertex buffer back, pinning the order the
// viewport actually paints in:
//
//   * the Solid pass fills the Facade with its own colour as one quad, and
//     that fill lands before the Door's leaf and before any aperture fill -
//     the apertures are painted over the fill, never under it;
//   * the fill covers the Facade's whole bounds, so moving it earlier did
//     not shrink or clip the surface itself;
//   * the wireframe overlay still outlines the Facade (never fills it) and
//     still lands before the thresholds' own outlines.
//
// Everything runs headless: ImGui is created without a renderer, so no
// window, dialog, or GPU is ever touched.

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "imgui/imgui.h"

#include "Render.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Facade.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Vector2.h"
#include "core/Window.h"
#include "UISettings.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// A Facade colour no other fill source can produce: not the Fore/Back
	// Layer colours, not a Door leaf, not the lights-off tint.
	core::BackgroundColour const kFacadeColour{ 210, 66, 138 };

	ImU32 facadeFillColour()
	{
		return ImU32(ImColor(kFacadeColour.r, kFacadeColour.g, kFacadeColour.b, 255));
	}

	// The colours Render.cpp paints around the Facade's surface.
	ImU32 const kDoorLeafColour = ImU32(ImColor(64, 192, 255));      // renderDoor's leaf
	ImU32 const kApertureFillColour = ImU32(ImColor(224, 224, 255)); // BackLocationColour
	ImU32 const kThresholdOutlineColour = ImU32(ImColor(0, 0, 0));    // wireframe thresholds

	// The first vertex painted with a colour, and how many vertices wear it.
	struct PaintRun
	{
		int firstVertex{ -1 };
		int vertexCount{ 0 };

		bool painted() const { return firstVertex >= 0; }
	};

	PaintRun paintedWith(ImDrawList const* drawList, ImU32 colour)
	{
		PaintRun run;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
		{
			if (drawList->VtxBuffer[i].col == colour)
			{
				if (run.firstVertex < 0) run.firstVertex = i;
				++run.vertexCount;
			}
		}
		return run;
	}

	// The ElemCount of the draw command that first consumes a vertex: 6 for a
	// filled quad (AddRectFilled), 10 for an outlined rect (AddRect). The
	// command-to-vertex mapping goes through the index buffer because this
	// imgui build only fills VtxOffset when the 16-bit index space wraps.
	int elemCountCovering(ImDrawList const* drawList, int vertexIndex)
	{
		for (int p = 0; p < drawList->IdxBuffer.Size; ++p)
		{
			if (drawList->IdxBuffer[p] != static_cast<ImDrawIdx>(vertexIndex))
				continue;
			for (int c = 0; c < drawList->CmdBuffer.Size; ++c)
			{
				auto const& cmd = drawList->CmdBuffer[c];
				if (p >= static_cast<int>(cmd.IdxOffset)
					&& p < static_cast<int>(cmd.IdxOffset) + static_cast<int>(cmd.ElemCount))
					return cmd.ElemCount;
			}
			return -1;
		}
		return -1;
	}

	// ImGui without a renderer: contexts are CPU-side only, nothing reaches a
	// window or the GPU.
	struct ImGuiGuard
	{
		ImGuiGuard() { ImGui::CreateContext(); }
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	// A two-Layer scene: a Room on Layer 1, a Facade directly in front of it
	// on Layer 0 (Layers are numbered front-to-back), carrying one Window and
	// one Door - the two threshold types the flat fill used to erase. Hands
	// the caller the World and the Facade's Sector index.
	template <typename Fn>
	void withFacadeHostingThresholds(Fn&& fn)
	{
		core::World world("Facade draw order", 12, 3);
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Behind", 1, 0, 0, 4, 1);
		auto const facadeIndex = world.addFacade("Frontage", 0, 0, 0, 4, 2,
			CORE_ROOM_MAX_HEIGHT);
		world.finishBuild();
		world.pauseSimulation();

		// Paint the Facade with the sentinel colour the checks read back.
		std::string colourDiagnostic;
		require(world.setFacadeColour(facadeIndex, kFacadeColour, &colourDiagnostic),
			("The Facade recolour was refused: " + colourDiagnostic).c_str());

		std::string diagnostic;
		require(world.canAddSectorWindow(0, 0, 1, 2, 1, &diagnostic),
			("A Window on a Facade was refused: " + diagnostic).c_str());
		world.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.addSectorDoor(0, 0, 3);
		world.finishBuild();

		fn(world, facadeIndex);
	}

	void renderFacadePass(core::World& world, uint32_t facadeIndex,
		LayerRenderStyle style, ImDrawList* drawList)
	{
		// The Layer colour the viewport would pass down as the generic fill.
		renderSector(world.getSector(facadeIndex), 0, style, false,
			ImColor(192, 192, 255), drawList);
	}
}

// The Solid pass: the Facade's own fill lands first, and the Door's leaf and
// the apertures behind the thresholds are painted over it - not under it.
// With the old ordering (fill in the type switch, after the BEHIND pass)
// both ordering assertions below fail.
void facadeFillPrecedesThresholdApertures()
{
	ImGuiGuard imgui;
	// The viewport-qualified form: the plain one needs a current window,
	// which headless never has.
	ImDrawList* drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());

	withFacadeHostingThresholds([&](core::World& world, uint32_t facadeIndex)
	{
		renderFacadePass(world, facadeIndex, LayerRenderStyle::Solid, drawList);

		auto const fill = paintedWith(drawList, facadeFillColour());
		require(fill.painted(), "the Solid pass painted no Facade fill at all");
		require(fill.vertexCount == 4, "the Facade fill was not exactly one quad");
		require(elemCountCovering(drawList, fill.firstVertex) == 6,
			"the Facade surface was not drawn as a filled quad");

		auto const door = paintedWith(drawList, kDoorLeafColour);
		require(door.painted(), "the Door on the Facade painted no leaf");
		require(fill.firstVertex < door.firstVertex,
			"the Facade fill was painted over the Door's leaf (#49 regression)");

		auto const aperture = paintedWith(drawList, kApertureFillColour);
		require(aperture.painted(), "no aperture fill reached the Sector behind");
		require(fill.firstVertex < aperture.firstVertex,
			"the Facade fill was painted over the threshold apertures (#49 regression)");
	});
}

// The fill moved earlier must still be the Facade's whole surface: its quad
// spans exactly the Facade's transformed bounds.
void facadeFillCoversTheWholeFacadeSurface()
{
	ImGuiGuard imgui;
	ImDrawList* drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());

	withFacadeHostingThresholds([&](core::World& world, uint32_t facadeIndex)
	{
		auto sector = world.getSector(facadeIndex);
		renderFacadePass(world, facadeIndex, LayerRenderStyle::Solid, drawList);

		core::Vector2 b0, b1;
		sector->getBounds(b0, b1);

		// The same transform transformPosition() applies, recomputed here.
		auto const tx = [](float x)
		{
			return x * CORE_CELL_WIDTH_PIXELS + gUISettings.worldViewportX + gUISettings.xOffset;
		};
		auto const ty = [](float y)
		{
			return gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- y * CORE_LEVEL_HEIGHT_PIXELS - gUISettings.yOffset;
		};

		float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
		auto const colour = facadeFillColour();
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
		{
			if (drawList->VtxBuffer[i].col != colour) continue;
			minX = std::min(minX, drawList->VtxBuffer[i].pos.x);
			maxX = std::max(maxX, drawList->VtxBuffer[i].pos.x);
			minY = std::min(minY, drawList->VtxBuffer[i].pos.y);
			maxY = std::max(maxY, drawList->VtxBuffer[i].pos.y);
		}

		require(minX != FLT_MAX, "no Facade-coloured vertices to measure");

		constexpr float kEpsilon = 0.01f;
		require(std::abs(minX - tx(std::min(b0.x, b1.x))) < kEpsilon
			&& std::abs(maxX - tx(std::max(b0.x, b1.x))) < kEpsilon
			&& std::abs(minY - ty(std::max(b0.y, b1.y))) < kEpsilon
			&& std::abs(maxY - ty(std::min(b0.y, b1.y))) < kEpsilon,
			"the Facade fill no longer covers the Facade's whole bounds");
	});
}

// The wireframe overlay: the Facade contributes its outline - never a fill -
// and that outline lands before the thresholds' own outlines.
void wireframePassOutlinesFacadeBeforeThresholds()
{
	ImGuiGuard imgui;
	ImDrawList* drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());

	withFacadeHostingThresholds([&](core::World& world, uint32_t facadeIndex)
	{
		renderFacadePass(world, facadeIndex, LayerRenderStyle::Wireframe, drawList);

		auto const outline = paintedWith(drawList, facadeFillColour());
		require(outline.painted(), "the wireframe overlay drew no Facade outline");
		// An anti-aliased AddRect outline wears 16 vertices; the AddRectFilled
		// surface wears 4. 16 here proves the overlay outlined and did not fill.
		require(outline.vertexCount == 16,
			"the wireframe Facade surface was filled, not outlined");

		auto const threshold = paintedWith(drawList, kThresholdOutlineColour);
		require(threshold.painted(), "the wireframe pass drew no threshold outlines");
		require(outline.firstVertex < threshold.firstVertex,
			"the Facade outline did not precede the threshold outlines");
	});
}

void runFacadeDrawOrderSmokeChecks()
{
	facadeFillPrecedesThresholdApertures();
	facadeFillCoversTheWholeFacadeSurface();
	wireframePassOutlinesFacadeBeforeThresholds();
}
