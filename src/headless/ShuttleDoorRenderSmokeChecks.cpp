// Shuttle carriage Door rendering on the Shuttle's own Layer.
//
// Landing Doors are authored on the Layer in front of a Shuttle and registered
// on the Shuttle transit as their back Sector. When that transit Layer is drawn
// solid, the Door leaves must therefore be wireframes over the filled carriage,
// not hidden beneath it or painted as solid front-side thresholds.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "imgui/imgui.h"

#include "ObjectTileset.h"
#include "Render.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "core/World.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	ImU32 const kDoorOutlineColour = ImU32(ImColor(0, 0, 0));
	ImU32 const kDoorLeafColour = ImU32(ImColor(64, 192, 255));
	ImU32 const kShuttleColour = ImU32(ImColor(128, 128, 192));
	ImColor const kTransitColour(210, 210, 210);

	struct ImGuiGuard
	{
		ImGuiGuard() { ImGui::CreateContext(); }
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	int firstVertexOfColour(ImDrawList const* drawList, ImU32 colour)
	{
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
			if (drawList->VtxBuffer[i].col == colour) return i;
		return -1;
	}

	int lastVertexOfColour(ImDrawList const* drawList, ImU32 colour)
	{
		for (int i = drawList->VtxBuffer.Size - 1; i >= 0; --i)
			if (drawList->VtxBuffer[i].col == colour) return i;
		return -1;
	}

	std::shared_ptr<const core::Sector> buildShuttleSector(core::World& world)
	{
		world.addRoom("Left terminal", 0, 0, 0, 3, 1);
		world.addRoom("Right terminal", 0, 0, 10, 3, 1);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 10 }, 0 };
		options.doorMask = 0b101;
		auto created = world.addShuttle(1, 0, 0, 13, options);
		world.finishBuild();
		return created.shuttle.sector;
	}

	void carriageDoorsAreWireframesAboveTheCarriage()
	{
		ImGuiGuard imgui;
		core::World world("Shuttle Door rendering", 16, 2);
		auto const shuttleSector = buildShuttleSector(world);

		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 1280.0f;
		gUISettings.worldViewportHeight = 720.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		auto drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
		drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f), false);
		renderSector(shuttleSector, 1, LayerRenderStyle::Solid, false,
			kTransitColour, drawList);
		drawList->PopClipRect();

		auto const firstOutline = firstVertexOfColour(drawList, kDoorOutlineColour);
		auto const lastCarriage = lastVertexOfColour(drawList, kShuttleColour);
		require(lastCarriage >= 0, "The Shuttle Layer rendered no filled carriage");
		require(firstOutline >= 0, "The Shuttle Layer rendered no carriage Door wireframes");
		require(firstOutline > lastCarriage,
			"Shuttle carriage Door wireframes were painted beneath the carriage");
		require(firstVertexOfColour(drawList, kDoorLeafColour) < 0,
			"A Shuttle carriage Door rendered as a solid front-side leaf");
	}

	void shuttleUsesAPlainShaftAndComposableCarriageImages()
	{
		core::World world("Textured Shuttle rendering", 16, 2);
		auto const shuttleSector = buildShuttleSector(world);

		SectorTileset sectors;
		sectors.width = 100;
		sectors.height = 100;
		sectors.surfaces.emplace("lift", SectorTileRegion{ 0, 0, 10, 10 });
		sectors.surfaces.emplace("shuttle", SectorTileRegion{ 50, 50, 10, 10 });
		setSectorTileset(std::move(sectors), reinterpret_cast<ImTextureID>(1));

		ObjectTileset objects;
		objects.width = 500;
		objects.height = 100;
		objects.sprites.emplace("shuttle-car-left", ObjectSprite{ { 0, 0, 50, 100 }, false });
		objects.sprites.emplace("shuttle-car-middle", ObjectSprite{ { 100, 0, 50, 100 }, false });
		objects.sprites.emplace("shuttle-car-right", ObjectSprite{ { 200, 0, 50, 100 }, false });
		objects.sprites.emplace("shuttle-car-connector", ObjectSprite{ { 300, 0, 50, 100 }, false });
		objects.sprites.emplace("door", ObjectSprite{ { 400, 0, 50, 100 }, false });
		setObjectTileset(std::move(objects), reinterpret_cast<ImTextureID>(2));

		WorldDrawList commands({ { -100000.0f, -100000.0f }, { 100000.0f, 100000.0f } });
		renderSector(shuttleSector, 1, LayerRenderStyle::Solid, false,
			kTransitColour, &commands);

		bool plainShaft = false;
		bool left = false, middle = false, right = false;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			if (triangle->texture == WorldDrawList::Texture::SectorAtlas)
			{
				plainShaft = true;
				for (auto const& uv : triangle->texcoords)
					require(uv.x < 0.2f && uv.y < 0.2f,
						"the Shuttle Sector used decorated architecture instead of the plain shaft surface");
			}
			if (triangle->texture != WorldDrawList::Texture::ObjectAtlas) continue;
			auto const u = triangle->texcoords[0].x;
			left |= u < 0.2f;
			middle |= u >= 0.2f && u < 0.4f;
			right |= u >= 0.4f && u < 0.6f;
		}

		clearObjectTileset();
		clearSectorTileset();
		require(plainShaft, "the Shuttle Sector emitted no textured shaft surface");
		require(left && middle && right,
			"the Shuttle carriage was not composed from left, middle, and right images");
	}
}

void runShuttleDoorRenderSmokeChecks()
{
	carriageDoorsAreWireframesAboveTheCarriage();
	shuttleUsesAPlainShaftAndComposableCarriageImages();
}
