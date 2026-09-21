// The real renderer's Location wall passes, for the open-wall ticket.
//
// A Location's side wall is drawn per deck, and each deck end carries its own
// state: Wall, BulkheadDoor, or None (open). Opening a shared wall clears the
// end on both sides of the boundary at once, and the viewport must then show
// the two Locations as connected.
//
// The opening is the *intersection*, never the whole deck:
//
//   * an open Corridor end removes its whole deck, because a Corridor deck is
//     exactly the height of the opening it makes;
//   * an open Room end removes only the stretch the neighbour shares. A Room
//     deck standing 1.0 tall beside a 0.7-tall Corridor keeps the 0.3 of wall
//     above the Corridor's ceiling - removing the whole deck there punched a
//     hole through the Room's own wall and made it look open to the void;
//   * a closed end draws its whole deck, and a BulkheadDoor end draws no plain
//     wall line, exactly as before.
//
// The wireframe overlay gets a second cut. It x-rays the Layer directly behind
// the selection, and a behind-Layer wall drawn straight across an opening the
// selected Layer has made reads as the wall the player just removed - the
// connection disappears again. So a behind-Layer wall also loses the stretch
// the viewed Layer's opening covers, and only that stretch: the rest of the
// behind wall still shows, and a boundary the selected Layer keeps closed
// still comes through in full.
//
// This check drives the real renderSector() against a live ImDrawList and reads
// the emitted wall segments back out of the vertex buffer, so it catches the
// renderer's own geometry rather than a model of it. The pure rule behind it,
// wallSpansToDraw(), is exercised alongside so a regression names the rule that
// broke rather than only the pixels.
//
// Everything runs headless: ImGui is created without a renderer, so no window,
// dialog, or GPU is ever touched.

#include <algorithm>
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
#include "core/Sector.h"
#include "core/SectorEnd.h"
#include "UISettings.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// The colour Render.cpp strokes a Location's floor, ceiling, and walls with.
	ImU32 const kWallColour = ImU32(ImColor(0, 0, 0));
	ImColor const kRoomColour(192, 192, 255);

	// ImGui without a renderer: contexts are CPU-side only, nothing reaches a
	// window or the GPU.
	struct ImGuiGuard
	{
		ImGuiGuard() { ImGui::CreateContext(); }
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	// The viewport-qualified draw list: the plain form needs a current window,
	// which headless never has.
	ImDrawList* testDrawList()
	{
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 1280.0f;
		gUISettings.worldViewportHeight = 720.0f;
		return ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
	}

	// One stroked line, reduced to its centreline in world units.
	struct Segment
	{
		float x0, y0, x1, y1;

		[[nodiscard]] bool vertical() const { return std::abs(x1 - x0) < 0.5f; }
		[[nodiscard]] bool horizontal() const { return std::abs(y1 - y0) < 0.5f; }
	};

	float toWorldX(float screenX)
	{
		return (screenX - gUISettings.worldViewportX - gUISettings.xOffset)
			/ CORE_CELL_WIDTH_PIXELS;
	}

	float toWorldY(float screenY)
	{
		return (gUISettings.worldViewportY + gUISettings.worldViewportHeight
			- gUISettings.yOffset - screenY) / CORE_DECK_HEIGHT_PIXELS;
	}

	// AddLine's thick stroke emits the four corners of the stroked rectangle.
	// Collapse each group of four back onto the line it was drawn for.
	std::vector<Segment> wallSegments(ImDrawList const* drawList, int start)
	{
		std::vector<ImVec2> vertices;
		for (int i = start; i < drawList->VtxBuffer.Size; ++i)
		{
			if (drawList->VtxBuffer[i].col == kWallColour)
				vertices.push_back(drawList->VtxBuffer[i].pos);
		}

		std::vector<Segment> segments;
		for (size_t i = 0; i + 3 < vertices.size(); i += 4)
		{
			float minX = vertices[i].x, maxX = vertices[i].x;
			float minY = vertices[i].y, maxY = vertices[i].y;
			for (size_t k = 1; k < 4; ++k)
			{
				minX = std::min(minX, vertices[i + k].x);
				maxX = std::max(maxX, vertices[i + k].x);
				minY = std::min(minY, vertices[i + k].y);
				maxY = std::max(maxY, vertices[i + k].y);
			}

			Segment s;
			if (maxY - minY > maxX - minX)
			{
				s.x0 = s.x1 = toWorldX((minX + maxX) * 0.5f);
				s.y0 = toWorldY(maxY);
				s.y1 = toWorldY(minY);
			}
			else
			{
				s.y0 = s.y1 = toWorldY((minY + maxY) * 0.5f);
				s.x0 = toWorldX(minX);
				s.x1 = toWorldX(maxX);
			}
			segments.push_back(s);
		}
		return segments;
	}

	// The vertical wall lines one Sector render emitted, keyed by the world x
	// they stand on.
	struct WallLine
	{
		float x;
		float y0;
		float y1;
	};

	std::vector<WallLine> verticalWalls(ImDrawList const* drawList, int start)
	{
		std::vector<WallLine> lines;
		for (auto const& s : wallSegments(drawList, start))
		{
			if (!s.vertical()) continue;
			lines.push_back({ s.x0, std::min(s.y0, s.y1), std::max(s.y0, s.y1) });
		}
		return lines;
	}

	// Render one Sector the way the viewport does - edges and all - and return
	// the wall lines it emitted in world units. `viewLayer` is the Layer the
	// viewport is showing, which decides whether this Sector draws as the
	// selection or as the wireframe overlay behind it.
	std::vector<WallLine> renderWalls(std::shared_ptr<core::Building> const& building,
		uint32_t sectorIndex, int viewLayer = 0)
	{
		setRenderBuilding(building);
		gUISettings.visibleLayer = viewLayer;

		auto drawList = testDrawList();
		int const start = drawList->VtxBuffer.Size;

		auto const layer = building->getSector(sectorIndex)->getLayerIndex();
		auto const style = static_cast<int>(layer) == viewLayer
			? LayerRenderStyle::Solid
			: LayerRenderStyle::Wireframe;

		drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f), false);
		renderSector(building->getSector(sectorIndex), layer, style, true,
			kRoomColour, drawList);
		drawList->PopClipRect();

		return verticalWalls(drawList, start);
	}

	bool near(float a, float b, float epsilon = 0.01f)
	{
		return std::abs(a - b) < epsilon;
	}

	// The wall line standing on `x` covering `from`..`to` exactly.
	bool hasLine(std::vector<WallLine> const& lines, float x, float from, float to)
	{
		return std::any_of(lines.begin(), lines.end(), [&](WallLine const& line)
		{
			return near(line.x, x) && near(line.y0, from) && near(line.y1, to);
		});
	}

	std::string describe(std::vector<WallLine> const& lines)
	{
		std::string text;
		for (auto const& line : lines)
		{
			text += " x=" + std::to_string(line.x)
				+ " y=" + std::to_string(line.y0) + ".." + std::to_string(line.y1);
		}
		return text.empty() ? " <none>" : text;
	}

	// The pure rule: a closed end draws its whole deck, a BulkheadDoor end
	// draws nothing, and an open end draws only what the neighbour leaves.
	void checkWallSpanRule()
	{
		auto building = std::make_shared<core::Building>("Wall span rule", 12, 4);
		auto room = building->addRoom("Room", 0, 0, 0, 3, 2);
		auto corridor = building->addCorridor(0u, 0u, 3u, 1u, 1u);
		building->finishBuild();

		auto const roomSector = building->getSector(room);
		require(roomSector != nullptr, "The Room could not be found");
		float const deck0Top = deckFloorY(*roomSector, 1);
		float const corridorTop = deckFloorY(*building->getSector(corridor), 1);

		// Closed: the whole deck, both sides.
		auto spans = wallSpansToDraw(building, *roomSector, 0, CORE_SIDE_LEFT);
		require(spans.size() == 1 && near(spans[0].y0, 0.0f) && near(spans[0].y1, deck0Top),
			"a closed wall does not draw its whole deck");

		// Open to a shorter neighbour: only the stretch above the overlap.
		building->pauseSimulation();
		building->removeLocationWall(room, 0, CORE_SIDE_RIGHT);
		building->finishBuild();

		spans = wallSpansToDraw(building, *roomSector, 0, CORE_SIDE_RIGHT);
		require(spans.size() == 1 && near(spans[0].y0, corridorTop) && near(spans[0].y1, deck0Top),
			"an open Room wall did not keep the stretch above its shorter neighbour");

		// The Corridor's own deck is exactly the opening, so nothing is left.
		spans = wallSpansToDraw(building, *building->getSector(corridor), 0, CORE_SIDE_LEFT);
		require(spans.empty(), "an open Corridor wall still drew a span");

		// The far side of the Room is untouched.
		spans = wallSpansToDraw(building, *roomSector, 0, CORE_SIDE_LEFT);
		require(spans.size() == 1 && near(spans[0].y0, 0.0f) && near(spans[0].y1, deck0Top),
			"opening one boundary disturbed the opposite wall");

		// With no Building to ask, an open end has nothing to intersect and
		// removes its whole deck.
		spans = wallSpansToDraw(nullptr, *roomSector, 0, CORE_SIDE_RIGHT);
		require(spans.empty(), "an open wall with no Building did not fall back to fully open");
	}

	// Two Rooms of equal height: opening their shared wall removes it entirely
	// from both sides, so the pair reads as one connected space.
	void checkEqualRoomsOpenCompletely()
	{
		auto building = std::make_shared<core::Building>("Equal Rooms", 12, 4);
		auto left = building->addRoom("Left", 0, 0, 0, 3, 2);
		auto right = building->addRoom("Right", 0, 0, 3, 3, 2);
		building->finishBuild();

		auto const leftSector = building->getSector(left);
		float const deck0Top = deckFloorY(*leftSector, 1);
		float const deck1Top = deckFloorY(*leftSector, 2);

		auto const leftWalls = renderWalls(building, left);
		require(hasLine(leftWalls, 3.0f, 0.0f, deck0Top),
			"a closed shared wall was not drawn: " + describe(leftWalls));

		building->pauseSimulation();
		building->removeLocationWall(left, 0, CORE_SIDE_RIGHT);
		building->finishBuild();

		auto const openLeft = renderWalls(building, left);
		require(!hasLine(openLeft, 3.0f, 0.0f, deck0Top),
			"an open shared wall still drew its whole deck on the left Room");
		require(hasLine(openLeft, 3.0f, deck0Top, deck1Top),
			"opening deck 0 disturbed the left Room's deck 1 wall: " + describe(openLeft));

		auto const openRight = renderWalls(building, right);
		require(!hasLine(openRight, 3.0f, 0.0f, deck0Top),
			"an open shared wall still drew its whole deck on the right Room");
		require(hasLine(openRight, 3.0f, deck0Top, deck1Top),
			"opening deck 0 disturbed the right Room's deck 1 wall: " + describe(openRight));
	}

	// A Room opening into a shorter Corridor: the Corridor draws no wall line at
	// the boundary, and the Room keeps the wall above the Corridor's ceiling.
	void checkRoomIntoShorterCorridorKeepsItsWallAboveTheOpening()
	{
		auto building = std::make_shared<core::Building>("Room into Corridor", 12, 4);
		auto room = building->addRoom("Room", 0, 0, 0, 3, 2);
		auto corridor = building->addCorridor(0u, 0u, 3u, 1u, 1u);
		building->finishBuild();

		auto const roomSector = building->getSector(room);
		auto const corridorSector = building->getSector(corridor);
		float const deck0Top = deckFloorY(*roomSector, 1);
		float const corridorTop = deckFloorY(*corridorSector, 1);

		require(corridorTop < deck0Top,
			"the scenario needs a Corridor shorter than the Room deck it opens into");

		building->pauseSimulation();
		building->removeLocationWall(room, 0, CORE_SIDE_RIGHT);
		building->finishBuild();

		auto const roomWalls = renderWalls(building, room);
		require(hasLine(roomWalls, 3.0f, corridorTop, deck0Top),
			"the Room did not keep its wall above the Corridor's ceiling: "
			+ describe(roomWalls));
		require(!hasLine(roomWalls, 3.0f, 0.0f, deck0Top),
			"the Room still drew its whole deck wall beside the Corridor: "
			+ describe(roomWalls));

		auto const corridorWalls = renderWalls(building, corridor);
		require(!hasLine(corridorWalls, 3.0f, 0.0f, corridorTop),
			"the open Corridor wall still rendered as a line: "
			+ describe(corridorWalls));
		require(hasLine(corridorWalls, 4.0f, 0.0f, corridorTop),
			"the Corridor's far wall disappeared with the open one: "
			+ describe(corridorWalls));
	}

	// The same rule from the other side, and one deck up: a Room whose deck sits
	// above a Corridor deck keeps the part of its wall the Corridor does not
	// reach.
	void checkUpperDeckKeepsItsWallAboveTheOpening()
	{
		auto building = std::make_shared<core::Building>("Upper deck opening", 12, 4);
		auto corridor = building->addCorridor(0u, 0u, 0u, 2u, 2u);
		auto room = building->addRoom("Room", 0, 1, 2, 3, 1);
		building->finishBuild();

		auto const corridorSector = building->getSector(corridor);
		auto const roomSector = building->getSector(room);
		float const deckFloor = deckFloorY(*roomSector, 0);
		float const corridorTop = deckFloorY(*corridorSector, 2);
		float const roomTop = deckFloorY(*roomSector, 1);

		require(corridorTop < roomTop,
			"the scenario needs a Corridor deck shorter than the Room above it");

		building->pauseSimulation();
		building->removeLocationWall(room, 0, CORE_SIDE_LEFT);
		building->finishBuild();

		auto const roomWalls = renderWalls(building, room);
		require(hasLine(roomWalls, 2.0f, corridorTop, roomTop),
			"the upper Room did not keep its wall above the Corridor's ceiling: "
			+ describe(roomWalls));
		require(!hasLine(roomWalls, 2.0f, deckFloor, roomTop),
			"the upper Room still drew its whole deck wall beside the Corridor: "
			+ describe(roomWalls));

		// The Corridor's deck is entirely the opening, so it draws nothing there.
		auto const corridorWalls = renderWalls(building, corridor);
		require(!hasLine(corridorWalls, 2.0f, deckFloor, corridorTop),
			"the open Corridor deck still rendered a wall line: "
			+ describe(corridorWalls));
	}

	// The wireframe overlay x-rays the Layer behind the selection, but a wall
	// drawn there straight across an opening the selected Layer has made reads as
	// the wall the player just removed. Only the intersecting stretch goes; the
	// rest of the behind-Layer wall still stands, and a boundary the selected
	// Layer keeps closed still shows through in full.
	void checkBehindLayerDoesNotDrawAcrossAFrontLayerOpening()
	{
		auto building = std::make_shared<core::Building>("Overlay and opening", 12, 4);
		auto leftCorridor = building->addCorridor(0u, 0u, 0u, 2u, 1u);
		require(building->getSector(leftCorridor) != nullptr,
			"the front Layer's left Corridor could not be created");
		auto room = building->addRoom("Front", 0, 0, 2, 4, 1);
		auto rightCorridor = building->addCorridor(0u, 0u, 6u, 2u, 1u);
		auto behind = building->addRoom("Behind", 1, 0, 2, 4, 1);
		building->finishBuild();

		float const roomTop = deckFloorY(*building->getSector(room), 1);
		float const corridorTop = deckFloorY(*building->getSector(rightCorridor), 1);

		// Closed to begin with: the behind Room's wall comes through in full.
		auto const closed = renderWalls(building, behind, 0);
		require(hasLine(closed, 6.0f, 0.0f, roomTop),
			"the behind Layer's wall did not show through a closed boundary: "
			+ describe(closed));

		building->pauseSimulation();
		building->removeLocationWall(room, 0, CORE_SIDE_RIGHT);
		building->finishBuild();

		auto const opened = renderWalls(building, behind, 0);
		require(!hasLine(opened, 6.0f, 0.0f, roomTop),
			"the behind Layer still drew its wall across the selected Layer's opening: "
			+ describe(opened));
		require(hasLine(opened, 6.0f, corridorTop, roomTop),
			"the behind Layer lost more than the intersection with the opening: "
			+ describe(opened));

		// The boundary the selected Layer keeps closed is untouched.
		require(hasLine(opened, 2.0f, 0.0f, roomTop),
			"a closed boundary lost its behind-Layer wall: " + describe(opened));

		// Viewed from its own Layer the behind Room is the selection, not the
		// overlay, so its wall is whole again.
		auto const own = renderWalls(building, behind, 1);
		require(hasLine(own, 6.0f, 0.0f, roomTop),
			"a Layer viewed in its own right had its wall cut by the overlay rule: "
			+ describe(own));

		// The pure rule agrees with the renderer.
		float from{ 0.0f }, to{ 0.0f };
		require(frontLayerOpening(building, 0, *building->getSector(behind), 0,
			CORE_SIDE_RIGHT, from, to)
			&& near(from, 0.0f) && near(to, corridorTop),
			"frontLayerOpening did not find the selected Layer's opening");
		require(!frontLayerOpening(building, 0, *building->getSector(behind), 0,
			CORE_SIDE_LEFT, from, to),
			"frontLayerOpening found an opening across a closed boundary");
		require(!frontLayerOpening(building, 1, *building->getSector(behind), 0,
			CORE_SIDE_RIGHT, from, to),
			"frontLayerOpening applied the overlay cut to the selection itself");
	}

	// A BulkheadDoor end draws no plain wall line, and opening a wall then
	// restoring it puts the whole deck back.
	void checkBulkheadAndRestore()
	{
		auto building = std::make_shared<core::Building>("Bulkhead and restore", 12, 4);
		auto left = building->addRoom("Left", 0, 0, 0, 3, 1);
		auto right = building->addRoom("Right", 0, 0, 3, 3, 1);
		building->finishBuild();

		float const deckTop = building->getSector(left)->getDeckHeight(0);

		building->pauseSimulation();
		building->removeLocationWall(left, 0, CORE_SIDE_RIGHT);
		building->finishBuild();

		auto const openWalls = renderWalls(building, left);
		require(!hasLine(openWalls, 3.0f, 0.0f, deckTop),
			"the opened shared wall still rendered on the left Room: "
			+ describe(openWalls));

		building->pauseSimulation();
		building->addLocationWall(left, 0, CORE_SIDE_RIGHT);
		building->finishBuild();

		auto const restored = renderWalls(building, left);
		require(hasLine(restored, 3.0f, 0.0f, deckTop),
			"restoring the wall did not bring the whole deck back: "
			+ describe(restored));

		building->pauseSimulation();
		core::Building::CreateBulkheadDoorOptions bulkheadOptions;
		bulkheadOptions.activationMode = core::DoorActivationMode::Manual;
		bulkheadOptions.controls[0] = bulkheadOptions.controls[1] = false;
		building->addSectorBulkheadDoor(0u, 0u, 3u, CORE_SIDE_LEFT, bulkheadOptions);
		building->finishBuild();

		auto const leftEnd = building->getSector(left)->getEndType(0, CORE_SIDE_RIGHT);
		auto const rightEnd = building->getSector(right)->getEndType(0, CORE_SIDE_LEFT);
		require(leftEnd == core::SectorEndType::BulkheadDoor
			|| rightEnd == core::SectorEndType::BulkheadDoor,
			"the Bulkhead Door did not take over the shared boundary");

		auto const bulkhead = renderWalls(building, left);
		require(!hasLine(bulkhead, 3.0f, 0.0f, deckTop),
			"a BulkheadDoor end still drew a plain wall line: " + describe(bulkhead));

		auto const bulkheadRight = renderWalls(building, right);
		require(!hasLine(bulkheadRight, 3.0f, 0.0f, deckTop),
			"a BulkheadDoor end still drew a plain wall line on the far Room: "
			+ describe(bulkheadRight));
	}
}

void runWallRenderSmokeChecks()
{
	try
	{
		ImGuiGuard imgui;

		checkWallSpanRule();
		checkEqualRoomsOpenCompletely();
		checkRoomIntoShorterCorridorKeepsItsWallAboveTheOpening();
		checkUpperDeckKeepsItsWallAboveTheOpening();
		checkBehindLayerDoesNotDrawAcrossAFrontLayerOpening();
		checkBulkheadAndRestore();
	}
	catch (std::exception const& error)
	{
		throw std::runtime_error(std::string("Wall render smoke check failed: ") + error.what());
	}
}
