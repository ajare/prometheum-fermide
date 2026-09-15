#include <algorithm>
#include <cfloat>
#include <cmath>
#include <deque>
#include <filesystem>
#include <optional>
#include <set>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4307)
#endif
#include <spdlog/spdlog.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/IconsFontAwesome5.h"

#if defined(_WIN32)
#include <nfd.h>
#elif defined(__linux__)
#include <nfd.h>
#else
#error "Unsupported platform"
#endif

#include "core/Vector2.h"
#include "core/Button.h"
#include "core/Door.h"
#include "core/BulkheadDoor.h"
#include "core/ForceBridge.h"
#include "core/Ladder.h"
#include "core/Location.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LadderTransit.h"
#include "core/LiftSectorObject.h"
#include "core/StaircaseTransit.h"
#include "core/DoorSectorObject.h"
#include "core/WindowSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/WindowSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/Marker.h"
#include "core/Log.h"
#include "core/Exceptions.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

#include "Main.h"
#include "RecentFiles.h"
#include "UI.h"
#include "Render.h"
#include "UISettings.h"
#include "Helpers.h"
#include "Exceptions.h"


extern spdlog::logger* gLogger;

extern UISettings gUISettings;
extern ImFont* gAgentIconFont;

core::InteractionPointId gHoveredInteractionPoint;
core::Agent *gHoveredAgent{ nullptr }, *gSelectedAgent{ nullptr };
std::shared_ptr<const core::Vertex> gHoveredVertex, gSelectedVertex;
std::shared_ptr<const core::Sector> gHoveredSector, gSelectedSector;
std::shared_ptr<const core::SectorObject> gHoveredSectorObject, gSelectedSectorObject;

static std::deque<core::LogMessage> gLogMessages;

using namespace std;


static bool gWorldHovered{ false };
static bool gPegmanConsumesLeftMouse{ false };

void setSelectionMode(UISettings::SelectionMode mode);

namespace
{
	struct DocumentSnapshot
	{
		string yaml;
		uint64_t stateId{ 0 };
	};

	constexpr size_t MaximumUndoHistory{ 100 };
	deque<DocumentSnapshot> gUndoHistory;
	deque<DocumentSnapshot> gRedoHistory;
	uint64_t gCurrentStateId{ 0 };
	uint64_t gNextStateId{ 1 };
	optional<uint64_t> gSavedStateId;

	optional<DocumentSnapshot> captureDocumentSnapshot(
		shared_ptr<const core::Building> const& building)
	{
		if (!building) return nullopt;
		try
		{
			auto serializer = core::YamlSerializer::toString();
			core::SerializationWorkData workData;
			workData.markSerializedUnmodified = false;
			building->serialize(*serializer, workData);
			serializer->serialize();
			return DocumentSnapshot{ serializer->getSerializedString(), gCurrentStateId };
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Undo", 0, core::LogLevel::Error,
				"Could not capture editor state: " + string(error.what()));
			return nullopt;
		}
	}

	void commitDocumentEdit(optional<DocumentSnapshot> snapshot)
	{
		if (!snapshot) return;
		gUndoHistory.push_back(std::move(*snapshot));
		if (gUndoHistory.size() > MaximumUndoHistory) gUndoHistory.pop_front();
		gRedoHistory.clear();
		gCurrentStateId = gNextStateId++;
	}

	bool applyAgentPathEdit(shared_ptr<const core::Building> const& building, core::Agent* agent,
		shared_ptr<core::Path> path, bool startPathing, bool replaceCurrentPath)
	{
		if (!building || !agent || !path) return false;
		auto undo = captureDocumentSnapshot(building);
		if (!undo) return false;

		auto const* requestedPath = path.get();
		if (replaceCurrentPath) agent->clearPath();
		agent->setPath(std::move(path), startPathing);
		if (agent->getPath().get() != requestedPath) return false;

		commitDocumentEdit(std::move(undo));
		return true;
	}

	constexpr float PaletteSlotSize{ 36.0f };
	constexpr float PaletteSlotWidth{ 64.0f };
	constexpr float PaletteInset{ 16.0f };
	constexpr float PaletteGap{ 6.0f };
	constexpr float PalettePadding{ 6.0f };
	constexpr float MarkerIconSize{ 22.0f };
	constexpr float PegmanGravity{ 6.0f };
	constexpr float PegmanTerminalVelocity{ 8.0f };

	enum class PalettePhase
	{
		Home,
		Armed,
		Dragging,
		Falling
	};

	enum class PaletteItem
	{
		None,
		Agent,
		Marker,
		Door,
		Window,
		Walkway,
		RoomLadder
	};

	struct PaletteDropState
	{
		PalettePhase phase{ PalettePhase::Home };
		PaletteItem item{ PaletteItem::None };
		ImVec2 pressPosition{};
		shared_ptr<const core::Sector> sector;
		uint32_t deckOffset{ 0 };
		float localX{ 0.0f };
		float feetY{ 0.0f };
		float floorY{ 0.0f };
		float velocity{ 0.0f };
		uint64_t nextAgentNumber{ 1 };
		string pastedAgentName;
		uint32_t pastedAgentFlags{ 0 };
	};

	struct PegmanTarget
	{
		shared_ptr<const core::Sector> sector;
		uint32_t deckOffset{ 0 };
		float localX{ 0.0f };
		float feetY{ 0.0f };
		float floorY{ 0.0f };
		string diagnostic;
		uint32_t cellX{ 0 };
		uint32_t cellY{ 0 };

		explicit operator bool() const { return sector != nullptr && diagnostic.empty(); }
	};

	PaletteDropState gPegman;

	bool gSelectingAgentPathDestination{ false };
	UISettings::SelectionMode gPathSelectionPreviousMode{ UISettings::SelectionMode::Object };
	bool gPathSelectionPreviouslyRenderedGraph{ false };

	void beginAgentPathSelection()
	{
		if (!gSelectedAgent || gSelectingAgentPathDestination) return;
		gSelectingAgentPathDestination = true;
		gPathSelectionPreviousMode = gUISettings.selectionMode;
		gPathSelectionPreviouslyRenderedGraph = gUISettings.renderGraph;
		gUISettings.selectionMode = UISettings::SelectionMode::Vertex;
		gUISettings.renderGraph = true;
		gSelectedVertex.reset();
	}

	void endAgentPathSelection()
	{
		if (!gSelectingAgentPathDestination) return;
		gSelectingAgentPathDestination = false;
		gUISettings.selectionMode = gPathSelectionPreviousMode;
		gUISettings.renderGraph = gPathSelectionPreviouslyRenderedGraph;
		gHoveredVertex.reset();
		gSelectedVertex.reset();
	}

	enum class PaintTool
	{
		None,
		Room,
		Corridor,
		Ladder,
		Staircase,
		Lift,
		Shuttle
	};

	struct PaintState
	{
		PaintTool tool{ PaintTool::None };
		bool dragging{ false };
		uint32_t layer{ CORE_LAYER_FORE };
		int anchorX{ 0 };
		int anchorY{ 0 };
	};

	struct PaintRectangle
	{
		bool valid{ false };
		uint32_t x{ 0 };
		uint32_t y{ 0 };
		uint32_t width{ 0 };
		uint32_t height{ 0 };
		string diagnostic;
	};

	PaintState gPaint;

	struct ShuttleDraft
	{
		uint32_t x{ 0 }, y{ 0 }, cellsWide{ 0 };
		int numCars{ 1 };
		int carWidth{ 3 };
		uint32_t doorMask{ 1u << 1 };
		int capacity{ 1 };
		int initialStop{ 0 };
		float minimumDwellSeconds{ CORE_LIFT_DOOR_PAUSE_TIME };
		float maximumBoardingSeconds{ CORE_DOOR_STAY_OPEN_TIME };
		bool allowPartialLandings{ false };
		vector<uint32_t> stopOffsets;
		string diagnostic;
	};

	optional<ShuttleDraft> gShuttleDraft;
	bool gOpenShuttleDraftPopup{ false };
	vector<core::Building::ShuttleStopCandidate> gShuttleDoorCandidates;
	int gSelectedShuttleDoorCandidate{ 0 };
	bool gOpenShuttleStopPopup{ false };

	enum class ResizeEdge
	{
		None,
		Left,
		Right,
		Bottom,
		Top,
		Move
	};

	struct SectorResizeState
	{
		bool dragging{ false };
		bool lift{ false };
		bool shuttle{ false };
		bool ladder{ false };
		bool staircase{ false };
		ResizeEdge edge{ ResizeEdge::None };
		ImVec2 pressPosition{};
		uint32_t originalX{ 0 }, originalY{ 0 }, originalWidth{ 0 }, originalHeight{ 0 };
		core::Building::LocationEditPlan preview;
		core::Building::LiftEditPlan liftPreview;
		core::Building::ShuttleEditPlan shuttlePreview;
		core::Building::LadderEditPlan ladderPreview;
		core::Building::StaircaseEditPlan staircasePreview;
	};

	SectorResizeState gSectorResize;
	optional<core::Building::LocationEditPlan> gPendingLocationEdit;
	optional<core::Building::LiftEditPlan> gPendingLiftEdit;
	optional<core::Building::ShuttleEditPlan> gPendingShuttleEdit;
	optional<core::Building::LadderEditPlan> gPendingLadderEdit;
	optional<core::Building::StaircaseEditPlan> gPendingStaircaseEdit;

	struct ObjectMoveState
	{
		bool dragging{ false };
		ImVec2 pressPosition{};
		uint32_t originalX{ 0 }, originalY{ 0 };
		core::Building::ObjectMovePlan preview;
	};

	ObjectMoveState gObjectMove;

	struct AgentMoveState
	{
		bool dragging{ false };
		ImVec2 pressPosition{};
		core::Vector2 originalPosition{};
		PegmanTarget preview;
	};

	AgentMoveState gAgentMove;
	bool gOpenLocationEditPopup{ false };

	void resetSectorResize()
	{
		gSectorResize = {};
	}

	void resetObjectMove()
	{
		gObjectMove = {};
	}

	void resetAgentMove()
	{
		gAgentMove = {};
	}

	void beginObjectMove(shared_ptr<const core::Building> const& building,
		shared_ptr<const core::SectorObject> const& object, uint32_t objectIndex)
	{
		if (!building || !object || gObjectMove.dragging) return;
		auto owner = object->getSector();
		gObjectMove.dragging = true;
		gObjectMove.pressPosition = ImGui::GetIO().MousePos;
		gObjectMove.originalX = object->getCellX();
		gObjectMove.originalY = object->getCellY();
		if (object->getObjectType() == core::SectorObjectType::Marker)
		{
			auto marker = static_pointer_cast<const core::MarkerSectorObject>(object)->getMarker();
			gObjectMove.originalX = marker->getCellX() + (uint32_t)floor(marker->getOffset());
			gObjectMove.originalY = marker->getCellY();
		}
		gObjectMove.preview = building->planMoveSectorObject(owner->getIndex(), objectIndex,
			gObjectMove.originalX, gObjectMove.originalY);
	}

	void resetPaint(bool clearTool = true)
	{
		gPaint.dragging = false;
		if (clearTool) gPaint.tool = PaintTool::None;
	}

	bool pointInRect(ImVec2 point, ImVec2 min, ImVec2 max)
	{
		return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
	}

	core::Vector2 screenToWorld(ImVec2 position)
	{
		return {
			(position.x - gUISettings.worldViewportX - gUISettings.xOffset) / CORE_CELL_WIDTH_PIXELS,
			(gUISettings.worldViewportY + gUISettings.worldViewportHeight - position.y
				- gUISettings.yOffset) / CORE_DECK_HEIGHT_PIXELS
		};
	}

	ImVec2 worldToScreen(core::Vector2 position)
	{
		return {
			gUISettings.worldViewportX + gUISettings.xOffset
				+ position.x * CORE_CELL_WIDTH_PIXELS,
			gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- gUISettings.yOffset - position.y * CORE_DECK_HEIGHT_PIXELS
		};
	}

	bool locationHasCapacity(shared_ptr<const core::Sector> const& sector)
	{
		if (!sector || sector->getType() != core::SectorType::Location) return false;
		auto capacity = sector->getCapacity();
		return capacity == ~0u || sector->getAgents().size() < capacity;
	}

	PegmanTarget getPegmanTarget(shared_ptr<const core::Building> const& building,
		ImVec2 feet, ImVec2 canvasPos, ImVec2 canvasSize)
	{
		if (!pointInRect(feet, canvasPos, canvasPos + canvasSize)) return {};

		auto world = screenToWorld(feet);
		auto sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
		if (!locationHasCapacity(sector) || !sector->pointInBounds(world.x, world.y)) return {};

		auto cellY = (uint32_t)floor(world.y);
		if (cellY < sector->getCellY()) return {};
		auto deckOffset = cellY - sector->getCellY();
		if (deckOffset >= sector->getDecksHigh()) return {};

		float halfAgentWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
		float minimumX = halfAgentWidth;
		float maximumX = sector->getSize().x - halfAgentWidth;
		float localX = world.x - sector->getPosition().x;
		localX = minimumX <= maximumX
			? clamp(localX, minimumX, maximumX)
			: sector->getSize().x * 0.5f;

		return { sector, deckOffset, localX, world.y,
			(float)sector->getCellY() + deckOffset, {} };
	}

	PegmanTarget getAgentMoveTarget(shared_ptr<const core::Building> const& building,
		core::Agent const* agent, core::Vector2 world)
	{
		if (world.x < 0.0f || world.y < 0.0f
			|| world.x >= building->getCellsWide() || world.y >= building->getDecksHigh())
			return { nullptr, 0, 0.0f, world.y, world.y, "Drop the Agent inside the world" };
		auto sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
		bool const retainsCapacity = sector && agent && agent->getSector() == sector.get();
		if (!sector || sector->getType() != core::SectorType::Location
			|| (!retainsCapacity && !locationHasCapacity(sector))
			|| !sector->pointInBounds(world.x, world.y))
			return { nullptr, 0, 0.0f, world.y, world.y,
				"Agents require a viable sector with available capacity" };

		auto cellY = (uint32_t)floor(world.y);
		if (cellY < sector->getCellY() || cellY >= sector->getCellY() + sector->getDecksHigh())
			return { nullptr, 0, 0.0f, world.y, world.y, "Agent deck is outside the sector" };
		float halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
		float localX = clamp(world.x - sector->getPosition().x, halfWidth,
			max(halfWidth, sector->getSize().x - halfWidth));
		return { sector, cellY - sector->getCellY(), localX, world.y,
			(float)cellY, {} };
	}

	PegmanTarget getMarkerTarget(shared_ptr<const core::Building> const& building,
		ImVec2 position, ImVec2 canvasPos, ImVec2 canvasSize)
	{
		if (!pointInRect(position, canvasPos, canvasPos + canvasSize))
			return { nullptr, 0, 0.0f, 0.0f, 0.0f, "Drop inside the world" };

		auto world = screenToWorld(position);
		auto sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
		if (!sector || !sector->pointInBounds(world.x, world.y))
			return { nullptr, 0, 0.0f, world.y, world.y, "Markers require a viable sector" };

		auto cellY = (uint32_t)floor(world.y);
		if (cellY < sector->getCellY())
			return { sector, 0, 0.0f, world.y, world.y, "Marker deck is outside the sector" };
		auto deckOffset = cellY - sector->getCellY();
		auto localX = world.x - sector->getPosition().x;
		string diagnostic;
		building->canAddSectorMarker(sector->getIndex(), deckOffset, localX, &diagnostic);
		return { sector, deckOffset, localX,
			(float)sector->getCellY() + deckOffset,
			(float)sector->getCellY() + deckOffset, std::move(diagnostic) };
	}

	PegmanTarget getDoorTarget(shared_ptr<const core::Building> const& building,
		ImVec2 position, ImVec2 canvasPos, ImVec2 canvasSize)
	{
		PegmanTarget target;
		if (!pointInRect(position, canvasPos, canvasPos + canvasSize))
		{
			target.diagnostic = "Drop inside the world";
			return target;
		}

		auto world = screenToWorld(position);
		if (world.x < 0.0f || world.y < 0.0f)
		{
			target.diagnostic = "Door position is outside the building";
			return target;
		}
		target.cellX = (uint32_t)floor(world.x);
		target.cellY = (uint32_t)floor(world.y);
		uint32_t landingX, landingWidth;
		if (building->getLiftLandingGeometry(target.cellY, target.cellX, landingX, landingWidth))
			target.cellX = landingX;
		target.sector = building->getSectorAtPosition(gUISettings.visibleLayer,
			(float)target.cellX, world.y);
		auto shuttleStops = building->getShuttleStopCandidatesForDoor(target.cellY, target.cellX);
		if (!shuttleStops.empty()) target.diagnostic.clear();
		else building->canAddCorridorDoor(target.cellY, target.cellX, &target.diagnostic);
		return target;
	}

	PegmanTarget getWindowTarget(shared_ptr<const core::Building> const& building,
		ImVec2 position, ImVec2 canvasPos, ImVec2 canvasSize)
	{
		PegmanTarget target;
		if (!pointInRect(position, canvasPos, canvasPos + canvasSize))
		{
			target.diagnostic = "Drop inside the world";
			return target;
		}
		auto world = screenToWorld(position);
		if (world.x < 0.0f || world.y < 0.0f)
		{
			target.diagnostic = "Window position is outside the building";
			return target;
		}
		target.cellX = (uint32_t)floor(world.x);
		target.cellY = (uint32_t)floor(world.y);
		target.sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
		building->canAddSectorWindow(gUISettings.visibleLayer, target.cellY, target.cellX,
			1, 1, &target.diagnostic);
		return target;
	}

	PegmanTarget getWalkwayTarget(shared_ptr<const core::Building> const& building,
		ImVec2 position, ImVec2 canvasPos, ImVec2 canvasSize)
	{
		PegmanTarget target;
		if (!pointInRect(position, canvasPos, canvasPos + canvasSize))
		{
			target.diagnostic = "Drop inside the world";
			return target;
		}
		auto world = screenToWorld(position);
		if (world.x < 0.0f || world.y < 0.0f)
		{
			target.diagnostic = "Walkway position is outside the building";
			return target;
		}
		target.cellX = (uint32_t)floor(world.x);
		target.cellY = (uint32_t)floor(world.y);
		target.sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
		auto room = dynamic_pointer_cast<const core::Location>(target.sector);
		if (!room || room->isCorridor())
			target.diagnostic = "Walkways can only be placed in Rooms";
		else
		{
			target.deckOffset = target.cellY >= target.sector->getCellY()
				? target.cellY - target.sector->getCellY() : ~0u;
			target.localX = (float)(target.cellX - target.sector->getCellX());
			building->canAddSectorWalkway(target.sector->getIndex(), target.deckOffset,
				(uint32_t)target.localX, &target.diagnostic);
		}
		return target;
	}

	PegmanTarget getRoomLadderTarget(shared_ptr<const core::Building> const& building,
		ImVec2 position, ImVec2 canvasPos, ImVec2 canvasSize)
	{
		PegmanTarget target;
		if (!pointInRect(position, canvasPos, canvasPos + canvasSize))
		{
			target.diagnostic = "Drop inside the world";
			return target;
		}
		auto world = screenToWorld(position);
		if (world.x < 0.0f || world.y < 0.0f)
		{
			target.diagnostic = "Room Ladder position is outside the building";
			return target;
		}
		target.cellX = (uint32_t)floor(world.x);
		target.cellY = (uint32_t)floor(world.y);
		target.sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
		auto room = dynamic_pointer_cast<const core::Location>(target.sector);
		if (!room || room->isCorridor() || !room->pointInBounds(world.x, world.y))
			target.diagnostic = "Room Ladders can only be placed in Rooms";
		else
		{
			target.deckOffset = target.cellY - room->getCellY();
			target.localX = (float)(target.cellX - room->getCellX());
			uint32_t height{};
			building->canAddRoomLadder(room->getIndex(), target.deckOffset,
				(uint32_t)target.localX, &height, &target.diagnostic);
			target.floorY = (float)(target.cellY + height);
		}
		return target;
	}

	float fittedPegmanFontSize(float maximumWidth, float maximumHeight, ImVec2& renderedSize)
	{
		ImFont* font = gAgentIconFont ? gAgentIconFont : ImGui::GetFont();
		float sourceSize = font->FontSize;
		auto sourceBounds = font->CalcTextSizeA(sourceSize, FLT_MAX, 0.0f, ICON_FA_STREET_VIEW);
		float scale = min(maximumWidth / max(sourceBounds.x, 1.0f),
			maximumHeight / max(sourceBounds.y, 1.0f));
		float fontSize = sourceSize * scale;
		renderedSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ICON_FA_STREET_VIEW);
		return fontSize;
	}

	void drawPegman(ImDrawList* drawList, ImVec2 feet, float maximumWidth,
		float maximumHeight, ImU32 colour)
	{
		ImVec2 size;
		float fontSize = fittedPegmanFontSize(maximumWidth, maximumHeight, size);
		ImVec2 topLeft{ feet.x - size.x * 0.5f, feet.y - size.y };
		drawList->AddText(gAgentIconFont ? gAgentIconFont : ImGui::GetFont(), fontSize,
			topLeft, colour, ICON_FA_STREET_VIEW);
	}

	void drawMarkerIcon(ImDrawList* drawList, ImVec2 point, float maximumSize, ImU32 colour)
	{
		ImFont* font = gAgentIconFont ? gAgentIconFont : ImGui::GetFont();
		auto sourceSize = font->FontSize;
		auto sourceBounds = font->CalcTextSizeA(sourceSize, FLT_MAX, 0.0f, ICON_FA_MAP_MARKER_ALT);
		auto fontSize = sourceSize * maximumSize
			/ max(max(sourceBounds.x, sourceBounds.y), 1.0f);
		auto size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ICON_FA_MAP_MARKER_ALT);
		drawList->AddText(font, fontSize, { point.x - size.x * 0.5f, point.y - size.y },
			colour, ICON_FA_MAP_MARKER_ALT);
	}

	void drawObjectIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax,
		ImU32 colour, char const* icon)
	{
		ImFont* font = gAgentIconFont ? gAgentIconFont : ImGui::GetFont();
		auto sourceSize = font->FontSize;
		auto sourceBounds = font->CalcTextSizeA(sourceSize, FLT_MAX, 0.0f, icon);
		auto available = boundsMax - boundsMin - ImVec2(10.0f, 8.0f);
		auto fontSize = sourceSize * min(available.x / max(sourceBounds.x, 1.0f),
			available.y / max(sourceBounds.y, 1.0f));
		auto size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, icon);
		drawList->AddText(font, fontSize,
			boundsMin + (boundsMax - boundsMin - size) * 0.5f, colour, icon);
	}

	void drawDoorIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		drawObjectIcon(drawList, boundsMin, boundsMax, colour, ICON_FA_DOOR_OPED);
	}

	void drawWindowIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		drawObjectIcon(drawList, boundsMin, boundsMax, colour, ICON_FA_WINDOW_MAXIMIZE);
	}

	void drawWalkwayIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		drawObjectIcon(drawList, boundsMin, boundsMax, colour, ICON_FA_GRIP_LINES);
	}

	void drawLadderIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		auto inset = ImVec2(18.0f, 6.0f);
		auto min = boundsMin + inset;
		auto max = boundsMax - inset;
		drawList->AddLine({ min.x, min.y }, { min.x, max.y }, colour, 2.5f);
		drawList->AddLine({ max.x, min.y }, { max.x, max.y }, colour, 2.5f);
		for (int rung = 0; rung < 5; ++rung)
		{
			float y = min.y + (max.y - min.y) * (float)rung / 4.0f;
			drawList->AddLine({ min.x, y }, { max.x, y }, colour, 2.0f);
		}
	}

	shared_ptr<const core::SectorObject> markerAtScreenPosition(
		shared_ptr<const core::Building> const& building, ImVec2 position)
	{
		for (auto const& sector : building->getSectors(gUISettings.visibleLayer))
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::Marker) continue;
				auto marker = static_pointer_cast<const core::MarkerSectorObject>(object)->getMarker();
				auto world = marker->getPosition();
				world.x += marker->getOffset();
				auto point = worldToScreen(world);
				if (pointInRect(position, point - ImVec2(MarkerIconSize * 0.5f, MarkerIconSize),
					point + ImVec2(MarkerIconSize * 0.5f, 2.0f))) return object;
			}
		}
		return nullptr;
	}

	shared_ptr<const core::SectorObject> ladderAtScreenPosition(
		shared_ptr<const core::Building> const& building, ImVec2 position)
	{
		auto world = screenToWorld(position);
		shared_ptr<const core::SectorObject> selected;
		for (auto const& sector : building->getSectors(gUISettings.visibleLayer))
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::Ladder) continue;
				auto ladder = static_pointer_cast<const core::LadderSectorObject>(object)->getLadder();
				core::Vector2 min, max;
				ladder->getCurrentShape(min, max);
				float tolerance = 5.0f / (float)CORE_CELL_WIDTH_PIXELS;
				if (world.x < min.x - tolerance || world.x > max.x + tolerance
					|| world.y < min.y - tolerance || world.y > max.y + tolerance) continue;
				// At a shared endpoint the upper segment has the greater base deck.
				if (!selected || object->getCellY() > selected->getCellY()) selected = object;
			}
		}
		return selected;
	}

	shared_ptr<const core::SectorObject> walkwayAtScreenPosition(
		shared_ptr<const core::Building> const& building, ImVec2 position)
	{
		constexpr float tolerance = 8.0f;
		for (auto const& sector : building->getSectors(gUISettings.visibleLayer))
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::Walkway) continue;
				auto walkway = static_pointer_cast<const core::WalkwaySectorObject>(object)->getWalkway();
				auto left = worldToScreen({ (float)walkway->getCellX(), (float)walkway->getCellY() });
				auto right = worldToScreen({ (float)walkway->getCellX() + 1.0f, (float)walkway->getCellY() });
				if (position.x >= left.x && position.x <= right.x
					&& abs(position.y - left.y) <= tolerance) return object;
			}
		}
		return nullptr;
	}

	string nextAgentName(shared_ptr<const core::Building> const& building)
	{
		set<string> names;
		for (auto const& agent : building->getSimulationSnapshot().agents)
			names.insert(agent.name);

		while (true)
		{
			auto name = format("Agent {}", gPegman.nextAgentNumber++);
			if (!names.contains(name)) return name;
		}
	}

	string nextRoomName(shared_ptr<const core::Building> const& building)
	{
		set<string> names;
		for (uint32_t layer = 0; layer < CORE_NUM_LAYERS; ++layer)
		{
			for (auto const& sector : building->getSectors(layer))
				names.insert(sector->getName());
		}

		for (uint64_t number = 1;; ++number)
		{
			auto name = format("Room {}", number);
			if (!names.contains(name)) return name;
		}
	}

	vector<uint32_t> shuttleCandidates(shared_ptr<const core::Building> const& building,
		ShuttleDraft const& draft)
	{
		if (draft.numCars <= 0 || (draft.carWidth != 3 && draft.carWidth != 4)) return {};
		return building->getValidShuttleStopOffsets(draft.y, draft.x, draft.cellsWide,
			(uint32_t)draft.numCars, (uint32_t)draft.carWidth, draft.allowPartialLandings,
			draft.doorMask);
	}

	bool validateShuttleDraft(shared_ptr<const core::Building> const& building,
		ShuttleDraft& draft)
	{
		draft.diagnostic.clear();
		if (draft.numCars <= 0) draft.diagnostic = "A Shuttle requires at least one carriage";
		else if (draft.carWidth != 3 && draft.carWidth != 4) draft.diagnostic = "Carriage width must be 3 or 4 cells";
		else if (draft.doorMask == 0 || (draft.doorMask >> draft.carWidth) != 0)
			draft.diagnostic = "Select at least one door cell within the carriage";
		else if (draft.capacity <= 0 || draft.capacity > (int)floor((float)draft.carWidth / CORE_AGENT_MAX_WIDTH))
			draft.diagnostic = "Capacity cannot be represented by separated carriage positions";
		else if (draft.minimumDwellSeconds < 0.0f
			|| draft.maximumBoardingSeconds < draft.minimumDwellSeconds)
			draft.diagnostic = "Maximum boarding time must be at least the non-negative minimum dwell";
		auto candidates = shuttleCandidates(building, draft);
		draft.stopOffsets.erase(remove_if(draft.stopOffsets.begin(), draft.stopOffsets.end(),
			[&](auto stop) { return find(candidates.begin(), candidates.end(), stop) == candidates.end(); }),
			draft.stopOffsets.end());
		sort(draft.stopOffsets.begin(), draft.stopOffsets.end());
		draft.stopOffsets.erase(unique(draft.stopOffsets.begin(), draft.stopOffsets.end()), draft.stopOffsets.end());
		auto shuttleWidth = draft.numCars > 0 && draft.carWidth > 0
			? draft.numCars * draft.carWidth + draft.numCars - 1 : 0;
		if (draft.diagnostic.empty() && draft.stopOffsets.size() < 2)
			draft.diagnostic = "Select at least two valid platform alignments";
		for (size_t i = 1; draft.diagnostic.empty() && i < draft.stopOffsets.size(); ++i)
			if (draft.stopOffsets[i] - draft.stopOffsets[i - 1] < (uint32_t)shuttleWidth)
				draft.diagnostic = "Stops must be separated by at least the coupled vehicle width";
		draft.initialStop = clamp(draft.initialStop, 0,
			max(0, (int)draft.stopOffsets.size() - 1));
		return draft.diagnostic.empty();
	}

	ShuttleDraft makeShuttleDraft(shared_ptr<const core::Building> const& building,
		uint32_t x, uint32_t y, uint32_t cellsWide)
	{
		ShuttleDraft draft;
		draft.x = x; draft.y = y; draft.cellsWide = cellsWide;
		auto candidates = shuttleCandidates(building, draft);
		auto shuttleWidth = (uint32_t)(draft.numCars * draft.carWidth + draft.numCars - 1);
		if (!candidates.empty())
		{
			draft.stopOffsets.push_back(candidates.front());
			auto last = find_if(candidates.rbegin(), candidates.rend(), [&](auto value)
				{ return value - candidates.front() >= shuttleWidth; });
			if (last != candidates.rend()) draft.stopOffsets.push_back(*last);
		}
		validateShuttleDraft(building, draft);
		return draft;
	}

	PaintRectangle getPaintRectangle(shared_ptr<const core::Building> const& building,
		ImVec2 mousePosition)
	{
		if (!gPaint.dragging || gPaint.anchorX < 0 || gPaint.anchorY < 0
			|| gPaint.anchorX >= (int)building->getCellsWide()
			|| gPaint.anchorY >= (int)building->getDecksHigh()) return {};

		auto layer = building->getLayer(gPaint.layer);
		auto world = screenToWorld(mousePosition);
		if (gPaint.tool == PaintTool::Staircase)
		{
			bool leftward = floor(world.x) < gPaint.anchorX;
			int x = leftward ? gPaint.anchorX - 1 : gPaint.anchorX;
			if (x + 2 > (int)building->getCellsWide()) x = gPaint.anchorX - 1;
			int endY = clamp((int)floor(world.y), 0, (int)building->getDecksHigh() - 1);
			int y = min(gPaint.anchorY, endY);
			uint32_t height = (uint32_t)(abs(endY - gPaint.anchorY) + 1);
			PaintRectangle result{ false, (uint32_t)max(0, x), (uint32_t)y, 2, height, {} };
			if (x < 0 || x + 2 > (int)building->getCellsWide())
				result.diagnostic = "The Staircase is outside the Building bounds";
			else
				result.valid = building->canAddStaircase(result.y, result.x, result.height,
					&result.diagnostic);
			return result;
		}
		if (gPaint.tool == PaintTool::Ladder)
		{
			int endY = clamp((int)floor(world.y), 0, (int)building->getDecksHigh() - 1);
			int y = min(gPaint.anchorY, endY);
			uint32_t height = (uint32_t)(abs(endY - gPaint.anchorY) + 1);
			PaintRectangle result{ false, (uint32_t)gPaint.anchorX, (uint32_t)y, 1, height, {} };
			result.valid = building->canAddLadder(result.y, result.x, result.height,
				&result.diagnostic);
			return result;
		}
		if (layer->getCellDefinition(gPaint.anchorX, gPaint.anchorY).occupied()) return {};

		int endX = clamp((int)floor(world.x), 0, (int)building->getCellsWide() - 1);
		if (gPaint.tool == PaintTool::Lift)
			endX = clamp(endX, gPaint.anchorX - 1, gPaint.anchorX + 1);
		int endY = (gPaint.tool == PaintTool::Corridor || gPaint.tool == PaintTool::Shuttle)
			? gPaint.anchorY
			: clamp((int)floor(world.y), 0, (int)building->getDecksHigh() - 1);
		int directionX = endX >= gPaint.anchorX ? 1 : -1;
		int directionY = endY >= gPaint.anchorY ? 1 : -1;
		int requestedWidth = abs(endX - gPaint.anchorX) + 1;
		int requestedHeight = abs(endY - gPaint.anchorY) + 1;

		PaintRectangle best;
		uint32_t bestArea = 0;
		for (int height = 1; height <= requestedHeight; ++height)
		{
			for (int width = 1; width <= requestedWidth; ++width)
			{
				int x = directionX > 0 ? gPaint.anchorX : gPaint.anchorX - width + 1;
				int y = directionY > 0 ? gPaint.anchorY : gPaint.anchorY - height + 1;
				bool free = true;
				for (int iy = y; free && iy < y + height; ++iy)
				{
					for (int ix = x; ix < x + width; ++ix)
					{
						if (layer->getCellDefinition(ix, iy).occupied())
						{
							free = false;
							break;
						}
					}
				}
				if (!free) continue;
				auto area = (uint32_t)(width * height);
				if (area > bestArea || (area == bestArea && (uint32_t)width > best.width))
				{
					best = { true, (uint32_t)x, (uint32_t)y,
						(uint32_t)width, (uint32_t)height, {} };
					bestArea = area;
				}
			}
		}
		if (best.valid && gPaint.tool == PaintTool::Lift)
		{
			uint32_t stops = 0;
			auto fore = building->getLayer(CORE_LAYER_FORE);
			for (uint32_t y = best.y; y < best.y + best.height; ++y)
			{
				auto const& first = fore->getCellDefinition(best.x, y);
				if (first.sectorIndex == ~0u) continue;
				auto sector = building->getSector(first.sectorIndex);
				auto location = dynamic_pointer_cast<const core::Location>(sector);
				if (!location || !location->isCorridor()) continue;
				bool complete = true;
				for (uint32_t x = best.x; x < best.x + best.width; ++x)
				{
					auto const& cell = fore->getCellDefinition(x, y);
					complete = complete && cell.sectorIndex == first.sectorIndex
						&& cell.isTraversableOnFoot() && !cell.hasObject() && cell.markers.empty();
				}
				if (complete && best.x == location->getCellX0()
					&& best.x + best.width - 1 == location->getCellX1())
				{
					best.valid = false;
					best.diagnostic = "There is no corridor space for a Lift call button";
					return best;
				}
				stops += complete;
			}
			if (stops < 2)
			{
				best.valid = false;
				best.diagnostic = "A Lift requires at least two fully overlapping corridor floors";
			}
		}
		else if (best.valid && gPaint.tool == PaintTool::Shuttle)
		{
			auto draft = makeShuttleDraft(building, best.x, best.y, best.width);
			if (!draft.diagnostic.empty())
			{
				best.valid = false;
				best.diagnostic = draft.diagnostic;
			}
		}
		return best;
	}

	void resetPegman()
	{
		gPegman.phase = PalettePhase::Home;
		gPegman.item = PaletteItem::None;
		gPegman.sector.reset();
		gPegman.velocity = 0.0f;
		gPegman.pastedAgentName.clear();
		gPegman.pastedAgentFlags = 0;
	}

	void setWorldPaused(shared_ptr<core::Building> const& building, bool paused)
	{
		if (paused)
		{
			building->pauseSimulation();
			gUISettings.worldPaused = true;
			return;
		}
		if (building->isSimulationPaused() && !building->resumeSimulation())
		{
			gUISettings.worldPaused = true;
			core::addLogMessage("Object palette", 0, core::LogLevel::Error,
				building->getTopologyDiagnostic());
			return;
		}
		gUISettings.worldPaused = false;
	}

	void placeMarker(shared_ptr<core::Building> const& building, PegmanTarget const& target)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			if (!building->isSimulationPaused()) building->pauseSimulation();
			auto created = building->addSectorMarker(target.sector->getIndex(),
				target.deckOffset, target.localX);
			building->finishBuild();
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = created.sector->getObject(created.index);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Object palette", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Object palette", 0, core::LogLevel::Error, error.what());
		}
	}

	void placeDoor(shared_ptr<core::Building> const& building, PegmanTarget const& target)
	{
		gShuttleDoorCandidates = building->getShuttleStopCandidatesForDoor(target.cellY, target.cellX);
		if (!gShuttleDoorCandidates.empty())
		{
			gSelectedShuttleDoorCandidate = 0;
			gOpenShuttleStopPopup = true;
			return;
		}
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto created = building->addSectorDoor(target.cellY, target.cellX);
			building->finishBuild();
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = created.door.sector->getObject(created.door.index);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Object palette", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Object palette", 0, core::LogLevel::Error, error.what());
		}
	}

	void placeWindow(shared_ptr<core::Building> const& building, PegmanTarget const& target)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto created = building->addSectorWindow(gUISettings.visibleLayer,
				target.cellY, target.cellX, 1, 1, {});
			building->finishBuild();
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = created.window.sector->getObject(created.window.index);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Object palette", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Object palette", 0, core::LogLevel::Error, error.what());
		}
	}

	void placeWalkway(shared_ptr<core::Building> const& building, PegmanTarget const& target)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto created = building->addSectorWalkway(target.sector->getIndex(),
				target.deckOffset, (uint32_t)target.localX);
			building->finishBuild();
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = created.sector->getObject(created.index);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Object palette", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Object palette", 0, core::LogLevel::Error, error.what());
		}
	}

	void placeRoomLadder(shared_ptr<core::Building> const& building, PegmanTarget const& target)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto created = building->addRoomLadder(target.sector->getIndex(),
				target.deckOffset, (uint32_t)target.localX);
			building->finishBuild();
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = created.ladder.sector->getObject(created.ladder.index);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Room Ladder editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Room Ladder editor", 0, core::LogLevel::Error, error.what());
		}
	}

	void landPegman(shared_ptr<core::Building> const& building)
	{
		if (gUISettings.worldPaused && locationHasCapacity(gPegman.sector))
		{
			auto undo = captureDocumentSnapshot(building);
			auto const name = gPegman.pastedAgentName.empty()
				? nextAgentName(building) : gPegman.pastedAgentName;
			auto id = building->createAgent(name, gPegman.sector->getIndex(),
				gPegman.deckOffset, gPegman.localX);
			auto created = building->lookupAgent(id).entity;
			if (created) created->setFlags(gPegman.pastedAgentFlags);
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = created;
			gSelectedSector.reset();
			gSelectedSectorObject.reset();
			commitDocumentEdit(std::move(undo));
		}
		resetPegman();
	}

	void renderObjectPalette(shared_ptr<core::Building> const& building, ImVec2 canvasPos,
		ImVec2 canvasSize, ImDrawList* drawList)
	{
		constexpr ImU32 yellow = IM_COL32(251, 188, 4, 255);
		constexpr ImU32 red = IM_COL32(244, 67, 54, 255);
		constexpr ImU32 trayColour = IM_COL32(24, 24, 28, 210);
		constexpr ImU32 selectedColour = IM_COL32(105, 78, 14, 240);
		constexpr ImU32 borderColour = IM_COL32(180, 180, 190, 180);
		constexpr ImU32 disabledColour = IM_COL32(90, 90, 98, 150);
		auto const& io = ImGui::GetIO();
		bool paletteConsumedMouse = false;

		if (gPaint.dragging && gPaint.layer != (uint32_t)gUISettings.visibleLayer)
			resetPaint(false);
		if ((gPaint.tool == PaintTool::Corridor && gUISettings.visibleLayer == CORE_LAYER_BACK)
			|| ((gPaint.tool == PaintTool::Ladder || gPaint.tool == PaintTool::Staircase
				|| gPaint.tool == PaintTool::Lift || gPaint.tool == PaintTool::Shuttle)
				&& gUISettings.visibleLayer == CORE_LAYER_FORE))
			resetPaint();

		if (gPegman.phase == PalettePhase::Falling)
		{
			float frameTime = min(io.DeltaTime, 0.1f);
			gPegman.velocity = min(gPegman.velocity + PegmanGravity * frameTime,
				PegmanTerminalVelocity);
			gPegman.feetY = max(gPegman.floorY,
				gPegman.feetY - gPegman.velocity * frameTime);
			if (gPegman.feetY <= gPegman.floorY) landPegman(building);
		}

		auto trayBottomRight = canvasPos + canvasSize - ImVec2(PaletteInset, PaletteInset);
		auto traySize = ImVec2(PalettePadding * 2.0f + PaletteSlotWidth * 6.0f + PaletteGap * 5.0f,
			PalettePadding * 2.0f + PaletteSlotSize * 2.0f + PaletteGap);
		auto trayTopLeft = trayBottomRight - traySize;
		auto roomMin = trayTopLeft + ImVec2(PalettePadding, PalettePadding);
		auto corridorMin = roomMin + ImVec2(PaletteSlotWidth + PaletteGap, 0.0f);
		auto ladderMin = corridorMin + ImVec2(PaletteSlotWidth + PaletteGap, 0.0f);
		auto staircaseMin = ladderMin + ImVec2(PaletteSlotWidth + PaletteGap, 0.0f);
		auto liftMin = staircaseMin + ImVec2(PaletteSlotWidth + PaletteGap, 0.0f);
		auto shuttleMin = liftMin + ImVec2(PaletteSlotWidth + PaletteGap, 0.0f);
		auto agentMin = roomMin + ImVec2(0.0f, PaletteSlotSize + PaletteGap);
		auto markerMin = corridorMin + ImVec2(0.0f, PaletteSlotSize + PaletteGap);
		auto doorMin = ladderMin + ImVec2(0.0f, PaletteSlotSize + PaletteGap);
		auto windowMin = staircaseMin + ImVec2(0.0f, PaletteSlotSize + PaletteGap);
		auto walkwayMin = liftMin + ImVec2(0.0f, PaletteSlotSize + PaletteGap);
		auto roomLadderMin = shuttleMin + ImVec2(0.0f, PaletteSlotSize + PaletteGap);
		auto roomMax = roomMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto corridorMax = corridorMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto ladderMax = ladderMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto staircaseMax = staircaseMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto liftMax = liftMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto shuttleMax = shuttleMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto windowMax = windowMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto doorMax = doorMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto agentMax = agentMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto markerMax = markerMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto walkwayMax = walkwayMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		auto roomLadderMax = roomLadderMin + ImVec2(PaletteSlotWidth, PaletteSlotSize);
		drawList->AddRectFilled(trayTopLeft, trayBottomRight, trayColour, 5.0f);
		drawList->AddRect(trayTopLeft, trayBottomRight, borderColour, 5.0f);

		bool overTray = gWorldHovered && pointInRect(io.MousePos, trayTopLeft, trayBottomRight);
		bool roomHovered = gWorldHovered && pointInRect(io.MousePos, roomMin, roomMax);
		bool corridorHovered = gWorldHovered && pointInRect(io.MousePos, corridorMin, corridorMax);
		bool ladderHovered = gWorldHovered && pointInRect(io.MousePos, ladderMin, ladderMax);
		bool staircaseHovered = gWorldHovered && pointInRect(io.MousePos, staircaseMin, staircaseMax);
		bool liftHovered = gWorldHovered && pointInRect(io.MousePos, liftMin, liftMax);
		bool shuttleHovered = gWorldHovered && pointInRect(io.MousePos, shuttleMin, shuttleMax);
		bool corridorDisabled = gUISettings.visibleLayer == CORE_LAYER_BACK;
		bool backOnlyDisabled = gUISettings.visibleLayer == CORE_LAYER_FORE;
		if (overTray) paletteConsumedMouse = true;

		auto drawPaintButton = [&](ImVec2 min, ImVec2 max, char const* label,
			PaintTool tool, bool hovered, bool disabled)
		{
			if (gPaint.tool == tool) drawList->AddRectFilled(min, max, selectedColour, 3.0f);
			drawList->AddRect(min, max,
				disabled ? disabledColour : (hovered ? yellow : borderColour), 3.0f);
			auto textSize = ImGui::CalcTextSize(label);
			auto textPosition = min + (max - min - textSize) * 0.5f;
			drawList->AddText(textPosition, disabled ? disabledColour : IM_COL32_WHITE, label);
		};

		if (gPegman.phase == PalettePhase::Home
			&& (roomHovered || corridorHovered || ladderHovered || staircaseHovered
				|| liftHovered || shuttleHovered))
		{
			paletteConsumedMouse = true;
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (corridorHovered && corridorDisabled)
				ImGui::SetTooltip("Corridors can only be painted on the Fore Layer");
			else if (ladderHovered && backOnlyDisabled)
				ImGui::SetTooltip("Ladders can only be painted on the Back Layer");
			else if ((staircaseHovered || liftHovered || shuttleHovered) && backOnlyDisabled)
				ImGui::SetTooltip("Staircases, Lifts, and Shuttles can only be painted on the Back Layer");
			else
				ImGui::SetTooltip(roomHovered ? "Paint Room" : corridorHovered ? "Paint Corridor"
					: ladderHovered ? "Paint Ladder" : staircaseHovered ? "Paint Staircase"
					: liftHovered ? "Paint Lift" : "Paint Shuttle");

			if (io.MouseClicked[0] && !(corridorHovered && corridorDisabled)
				&& !((ladderHovered || staircaseHovered || liftHovered || shuttleHovered)
					&& backOnlyDisabled))
			{
				auto clickedTool = roomHovered ? PaintTool::Room
					: corridorHovered ? PaintTool::Corridor
					: ladderHovered ? PaintTool::Ladder
					: staircaseHovered ? PaintTool::Staircase
					: liftHovered ? PaintTool::Lift : PaintTool::Shuttle;
				gPaint.tool = gPaint.tool == clickedTool ? PaintTool::None : clickedTool;
				gPaint.dragging = false;
				resetPegman();
				if (gPaint.tool != PaintTool::None)
				{
					if (!building->isSimulationPaused()) building->pauseSimulation();
					gUISettings.worldPaused = true;
				}
			}
		}

		drawPaintButton(roomMin, roomMax, "Room", PaintTool::Room, roomHovered, false);
		drawPaintButton(corridorMin, corridorMax, "Corridor", PaintTool::Corridor,
			corridorHovered, corridorDisabled);
		drawPaintButton(ladderMin, ladderMax, "Ladder", PaintTool::Ladder,
			ladderHovered, backOnlyDisabled);
		drawPaintButton(staircaseMin, staircaseMax, "Staircase", PaintTool::Staircase,
			staircaseHovered, backOnlyDisabled);
		drawPaintButton(liftMin, liftMax, "Lift", PaintTool::Lift, liftHovered, backOnlyDisabled);
		drawPaintButton(shuttleMin, shuttleMax, "Shuttle", PaintTool::Shuttle,
			shuttleHovered, backOnlyDisabled);
		drawWindowIcon(drawList, windowMin, windowMax, yellow);
		drawWalkwayIcon(drawList, walkwayMin, walkwayMax, yellow);
		drawLadderIcon(drawList, roomLadderMin, roomLadderMax, yellow);

		bool paintWasActive = gPaint.tool != PaintTool::None;
		if (paintWasActive && (ImGui::IsKeyPressed(ImGuiKey_Escape)
			|| (gWorldHovered && io.MouseClicked[1])))
		{
			resetPaint();
			paletteConsumedMouse = true;
		}

		if (gPaint.tool != PaintTool::None && !gPaint.dragging && gWorldHovered
			&& !overTray && io.MouseClicked[0])
		{
			auto world = screenToWorld(io.MousePos);
			int x = (int)floor(world.x);
			int y = (int)floor(world.y);
			if (x >= 0 && y >= 0 && x < (int)building->getCellsWide()
				&& y < (int)building->getDecksHigh())
			{
				gPaint.dragging = true;
				gPaint.layer = (uint32_t)gUISettings.visibleLayer;
				gPaint.anchorX = x;
				gPaint.anchorY = y;
			}
		}

		PaintRectangle paintRectangle;
		if (gPaint.dragging)
		{
			paletteConsumedMouse = true;
			paintRectangle = getPaintRectangle(building, io.MousePos);
			if (paintRectangle.valid)
			{
				auto topLeft = worldToScreen({ (float)paintRectangle.x,
					(float)(paintRectangle.y + paintRectangle.height) });
				auto bottomRight = worldToScreen({ (float)(paintRectangle.x + paintRectangle.width),
					(float)paintRectangle.y });
				drawList->AddRectFilled(topLeft, bottomRight, IM_COL32(251, 188, 4, 55));
				drawList->AddRect(topLeft, bottomRight, yellow, 0.0f, 0, 2.0f);
			}
			else
			{
				auto x = paintRectangle.width ? paintRectangle.x : (uint32_t)gPaint.anchorX;
				auto y = paintRectangle.height ? paintRectangle.y : (uint32_t)gPaint.anchorY;
				auto width = paintRectangle.width ? paintRectangle.width : 1u;
				auto height = paintRectangle.height ? paintRectangle.height : 1u;
				auto topLeft = worldToScreen({ (float)x, (float)(y + height) });
				auto bottomRight = worldToScreen({ (float)(x + width), (float)y });
				drawList->AddRectFilled(topLeft, bottomRight, IM_COL32(244, 67, 54, 55));
				drawList->AddRect(topLeft, bottomRight, red, 0.0f, 0, 2.0f);
				ImGui::SetTooltip("%s", paintRectangle.diagnostic.empty()
					? "The starting cell is occupied" : paintRectangle.diagnostic.c_str());
			}
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

			if (io.MouseReleased[0])
			{
				auto tool = gPaint.tool;
				resetPaint(false);
				if (paintRectangle.valid)
				{
					if (tool == PaintTool::Shuttle)
					{
						gShuttleDraft = makeShuttleDraft(building, paintRectangle.x,
							paintRectangle.y, paintRectangle.width);
						gOpenShuttleDraftPopup = true;
					}
					else try
					{
						auto undo = captureDocumentSnapshot(building);
						if (tool == PaintTool::Room)
							building->addRoom(nextRoomName(building), gPaint.layer,
								paintRectangle.y, paintRectangle.x, paintRectangle.width,
								paintRectangle.height, CORE_ROOM_MAX_HEIGHT);
						else if (tool == PaintTool::Corridor)
							building->addCorridor(paintRectangle.y, paintRectangle.x,
								paintRectangle.width, 1);
						else if (tool == PaintTool::Ladder)
							building->addLadder(paintRectangle.y, paintRectangle.x,
								{ paintRectangle.height, false, true });
						else if (tool == PaintTool::Staircase)
						{
							int mountSide = paintRectangle.x < (uint32_t)gPaint.anchorX
								? CORE_SIDE_RIGHT : CORE_SIDE_LEFT;
							building->addStaircase(paintRectangle.y, paintRectangle.x,
								{ paintRectangle.height, mountSide });
						}
						else building->addLift(paintRectangle.y, paintRectangle.x,
							paintRectangle.width, paintRectangle.height);
						building->finishBuild();
						commitDocumentEdit(std::move(undo));
					}
					catch (core::Exception const& error)
					{
						core::addLogMessage("Paint palette", 0, core::LogLevel::Error,
							error.getMessage());
					}
					catch (std::exception const& error)
					{
						core::addLogMessage("Paint palette", 0, core::LogLevel::Error,
							error.what());
					}
				}
			}
		}

		PaletteItem hoveredItem = PaletteItem::None;
		if (gWorldHovered && gPegman.phase == PalettePhase::Home)
		{
			if (pointInRect(io.MousePos, agentMin, agentMax)) hoveredItem = PaletteItem::Agent;
			else if (pointInRect(io.MousePos, markerMin, markerMax)) hoveredItem = PaletteItem::Marker;
			else if (pointInRect(io.MousePos, doorMin, doorMax)) hoveredItem = PaletteItem::Door;
			else if (pointInRect(io.MousePos, windowMin, windowMax)) hoveredItem = PaletteItem::Window;
			else if (pointInRect(io.MousePos, walkwayMin, walkwayMax)) hoveredItem = PaletteItem::Walkway;
			else if (pointInRect(io.MousePos, roomLadderMin, roomLadderMax)) hoveredItem = PaletteItem::RoomLadder;
		}
		drawList->AddRect(windowMin, windowMax,
			hoveredItem == PaletteItem::Window ? yellow : borderColour, 3.0f);
		drawList->AddRect(walkwayMin, walkwayMax,
			hoveredItem == PaletteItem::Walkway ? yellow : borderColour, 3.0f);
		drawList->AddRect(roomLadderMin, roomLadderMax,
			hoveredItem == PaletteItem::RoomLadder ? yellow : borderColour, 3.0f);
		drawList->AddRect(agentMin, agentMax,
			hoveredItem == PaletteItem::Agent ? yellow : borderColour, 3.0f);
		drawList->AddRect(markerMin, markerMax,
			hoveredItem == PaletteItem::Marker ? yellow : borderColour, 3.0f);
		drawList->AddRect(doorMin, doorMax,
			hoveredItem == PaletteItem::Door ? yellow : borderColour, 3.0f);
		if (hoveredItem != PaletteItem::None)
		{
			paletteConsumedMouse = true;
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImGui::SetTooltip(hoveredItem == PaletteItem::Agent ? "Drag to add Agent"
				: hoveredItem == PaletteItem::Marker ? "Drag to add Marker"
				: hoveredItem == PaletteItem::Window ? "Drag to add Window"
				: hoveredItem == PaletteItem::Walkway ? "Drag to add Walkway"
				: hoveredItem == PaletteItem::RoomLadder ? "Drag to add Room Ladder" : "Drag to add Door");
			if (io.MouseClicked[0])
			{
				gPegman.phase = PalettePhase::Armed;
				gPegman.item = hoveredItem;
				gPegman.pressPosition = io.MousePos;
			}
		}

		if (gPegman.phase == PalettePhase::Armed)
		{
			paletteConsumedMouse = true;
			ImVec2 movement = io.MousePos - gPegman.pressPosition;
			if (io.MouseDown[0] && movement.x * movement.x + movement.y * movement.y
				>= io.MouseDragThreshold * io.MouseDragThreshold)
			{
				resetPaint();
				gPegman.phase = PalettePhase::Dragging;
			}
			else if (io.MouseReleased[0]) resetPegman();
		}

		PegmanTarget target;
		if (gPegman.phase == PalettePhase::Dragging)
		{
			paletteConsumedMouse = true;
			if (gPegman.item == PaletteItem::Marker)
				target = getMarkerTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::Door)
				target = getDoorTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::Window)
				target = getWindowTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::Walkway)
				target = getWalkwayTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::RoomLadder)
				target = getRoomLadderTarget(building, io.MousePos, canvasPos, canvasSize);
			else
				target = getPegmanTarget(building, io.MousePos, canvasPos, canvasSize);
			if (!gUISettings.worldPaused) target.diagnostic = "Pause simulation to place objects";
			if (ImGui::IsKeyPressed(ImGuiKey_Escape) || io.MouseClicked[1]) resetPegman();
			else if (io.MouseReleased[0])
			{
				if (target && gPegman.item == PaletteItem::Marker)
				{
					placeMarker(building, target);
					resetPegman();
				}
				else if (target && gPegman.item == PaletteItem::Door)
				{
					placeDoor(building, target);
					resetPegman();
				}
				else if (target && gPegman.item == PaletteItem::Window)
				{
					placeWindow(building, target);
					resetPegman();
				}
				else if (target && gPegman.item == PaletteItem::Walkway)
				{
					placeWalkway(building, target);
					resetPegman();
				}
				else if (target && gPegman.item == PaletteItem::RoomLadder)
				{
					placeRoomLadder(building, target);
					resetPegman();
				}
				else if (target && gPegman.item == PaletteItem::Agent)
				{
					gPegman.phase = PalettePhase::Falling;
					gPegman.sector = target.sector;
					gPegman.deckOffset = target.deckOffset;
					gPegman.localX = target.localX;
					gPegman.feetY = target.feetY;
					gPegman.floorY = target.floorY;
					gPegman.velocity = 0.0f;
					if (gPegman.feetY <= gPegman.floorY) landPegman(building);
				}
				else resetPegman();
			}
		}

		drawPegman(drawList, { (agentMin.x + agentMax.x) * 0.5f, agentMax.y - 3.0f },
			PaletteSlotSize - 8.0f, PaletteSlotSize - 8.0f, yellow);
		drawMarkerIcon(drawList, { (markerMin.x + markerMax.x) * 0.5f, markerMax.y - 5.0f },
			PaletteSlotSize - 10.0f, yellow);
		drawDoorIcon(drawList, doorMin, doorMax, yellow);

		if (gPegman.phase == PalettePhase::Dragging)
		{
			auto colour = target ? yellow : red;
			if (gPegman.item == PaletteItem::Marker)
			{
				auto preview = target.sector
					? worldToScreen({ target.sector->getPosition().x + target.localX, target.floorY })
					: io.MousePos;
				drawMarkerIcon(drawList, preview, MarkerIconSize, colour);
			}
			else if (gPegman.item == PaletteItem::Walkway)
			{
				if (target.sector)
				{
					auto topLeft = worldToScreen({ (float)target.cellX, (float)target.cellY + 1.0f });
					auto bottomRight = worldToScreen({ (float)target.cellX + 1.0f, (float)target.cellY });
					drawWalkwayIcon(drawList, topLeft, bottomRight, colour);
				}
				else drawWalkwayIcon(drawList, io.MousePos - ImVec2(24.0f, 18.0f),
					io.MousePos + ImVec2(24.0f, 18.0f), colour);
			}
			else if (gPegman.item == PaletteItem::RoomLadder)
			{
				if (target.sector && target.floorY > (float)target.cellY)
					drawLadderIcon(drawList,
						worldToScreen({ (float)target.cellX, target.floorY }),
						worldToScreen({ (float)target.cellX + 1.0f, (float)target.cellY }), colour);
				else drawLadderIcon(drawList, io.MousePos - ImVec2(24.0f, 18.0f),
					io.MousePos + ImVec2(24.0f, 18.0f), colour);
			}
			else if (gPegman.item == PaletteItem::Door
				|| gPegman.item == PaletteItem::Window)
			{
				if (target.sector)
				{
					auto previewHeight = gPegman.item == PaletteItem::Door ? CORE_DOOR_HEIGHT : 1.0f;
					auto topLeft = worldToScreen({ (float)target.cellX,
						(float)target.cellY + previewHeight });
					auto bottomRight = worldToScreen({ (float)target.cellX + 1.0f,
						(float)target.cellY });
					drawList->AddRectFilled(topLeft, bottomRight,
						colour == yellow ? IM_COL32(251, 188, 4, 55) : IM_COL32(244, 67, 54, 55));
					drawList->AddRect(topLeft, bottomRight, colour, 0.0f, 0, 2.0f);
				}
				else
				{
					auto previewHeight = gPegman.item == PaletteItem::Door ? CORE_DOOR_HEIGHT : 1.0f;
					auto halfSize = ImVec2(CORE_CELL_WIDTH_PIXELS * 0.5f,
						previewHeight * CORE_DECK_HEIGHT_PIXELS * 0.5f);
					drawList->AddRect(io.MousePos - halfSize, io.MousePos + halfSize,
						colour, 0.0f, 0, 2.0f);
				}
			}
			else
			{
				drawPegman(drawList, io.MousePos,
					CORE_AGENT_MAX_WIDTH * CORE_CELL_WIDTH_PIXELS,
					CORE_AGENT_MAX_HEIGHT * CORE_DECK_HEIGHT_PIXELS, colour);
			}
			if (!target.diagnostic.empty()) ImGui::SetTooltip("%s", target.diagnostic.c_str());
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}
		else if (gPegman.phase == PalettePhase::Falling)
		{
			auto globalX = gPegman.sector->getPosition().x + gPegman.localX;
			drawPegman(drawList, worldToScreen({ globalX, gPegman.feetY }),
				CORE_AGENT_MAX_WIDTH * CORE_CELL_WIDTH_PIXELS,
				CORE_AGENT_MAX_HEIGHT * CORE_DECK_HEIGHT_PIXELS, yellow);
		}

		gPegmanConsumesLeftMouse = gSectorResize.dragging || gObjectMove.dragging
			|| gAgentMove.dragging || paletteConsumedMouse || gPaint.tool != PaintTool::None
			|| gPaint.dragging || paintWasActive;
	}
}

// The world is now an ImGui window, so WantCaptureMouse is true over it.
// Track its canvas explicitly to distinguish it from the controls.
bool mouseInteractingWithBackground()
{
	return gWorldHovered;
}


MouseButtonStatus getMouseButtonStatus()
{
	MouseButtonStatus status;

	auto const& io = ImGui::GetIO();

	static ImVec2 frameDragDelta[2];

	if (!io.WantCaptureMouse || mouseInteractingWithBackground())
	{
		for (int i = 0; i < 2; ++i)
		{
			if (io.MouseClicked[i])
			{
				frameDragDelta[i] = ImGui::GetMouseDragDelta(i);
				status.state[i] = MouseButtonStatus::State::Clicked;
			}
			else if (io.MouseDown[i])
			{
				status.state[i] = MouseButtonStatus::State::Down;
			}
			else if (io.MouseReleased[i])
			{
				status.state[i] = MouseButtonStatus::State::Released;
			}

			status.dragging[i] = ImGui::IsMouseDragging(i, 2);
			status.dragDelta[i] = ImGui::GetMouseDragDelta(i) - frameDragDelta[i];

			frameDragDelta[i] = ImGui::GetMouseDragDelta(i);
		}
	}
	else
	{
		for (int i = 0; i < 2; ++i)
		{
			status.state[i] = MouseButtonStatus::State::Unavailable;
			status.dragging[i] = false;
			status.dragDelta[i] = { 0, 0 };
		}
	}

	// Modifiers
	if (!io.WantCaptureKeyboard)
	{
		status.modCtrl = io.KeyCtrl;
		status.modShift = io.KeyShift;
		status.modAlt = io.KeyAlt;
	}
	else
	{
		status.modCtrl = false;
		status.modShift = false;
		status.modAlt = false;
	}

	return status;
}


void setSelectionMode(UISettings::SelectionMode mode)
{
	endAgentPathSelection();
	if (gUISettings.selectionMode != mode)
	{
		resetObjectMove();
		resetAgentMove();
		gUISettings.selectionMode = mode;
		gSelectedAgent = nullptr;
		gSelectedVertex.reset();
		gSelectedSector.reset();
		gSelectedSectorObject.reset();
		resetSectorResize();
	}

	if (mode == UISettings::SelectionMode::Vertex)
	{
		gUISettings.renderGraph = true;
	}
}


void clearSelections()
{
	switch (gUISettings.selectionMode)
	{
	case UISettings::SelectionMode::Vertex:
		gSelectedVertex = nullptr;
		break;

	case UISettings::SelectionMode::Object:
		gSelectedAgent = nullptr;
		gSelectedSector = nullptr;
		resetAgentMove();
		gSelectedSectorObject = nullptr;
		break;

	case UISettings::SelectionMode::Sector:
		gSelectedSector.reset();
		resetSectorResize();
		break;
	}
}

namespace
{
	enum class PendingFileAction
	{
		None,
		New,
		Open,
		OpenRecent,
		Close,
		Exit
	};

	constexpr size_t MaximumRecentFiles{ 5 };
	string gBuildingFilepath;
	RecentFiles gRecentFiles{ MaximumRecentFiles };
	string gPendingRecentFilepath;
	PendingFileAction gPendingFileAction{ PendingFileAction::None };
	bool gOpenUnsavedChangesPopup{ false };
	bool gOpenNewBuildingPopup{ false };
	bool gOpenFileErrorPopup{ false };
	string gFileError;
	char gNewBuildingName[128]{ "Untitled" };
	int gNewBuildingWidth{ 48 };
	int gNewBuildingDecks{ 6 };

	void clearDocumentState(bool clearHistory = true)
	{
		gHoveredInteractionPoint = {};
		gHoveredAgent = nullptr;
		gSelectedAgent = nullptr;
		gHoveredVertex.reset();
		gSelectedVertex.reset();
		gHoveredSector.reset();
		gSelectedSector.reset();
		gHoveredSectorObject.reset();
		gSelectedSectorObject.reset();
		resetPegman();
		resetPaint();
		endAgentPathSelection();
		resetSectorResize();
		resetObjectMove();
		resetAgentMove();
		gPendingLocationEdit.reset();
		gPendingLiftEdit.reset();
		gPendingShuttleEdit.reset();
		gPendingLadderEdit.reset();
		gPendingStaircaseEdit.reset();
		gShuttleDraft.reset();
		gShuttleDoorCandidates.clear();
		gUISettings.worldPaused = false;
		if (clearHistory)
		{
			gUndoHistory.clear();
			gRedoHistory.clear();
			gCurrentStateId = 0;
			gNextStateId = 1;
			gSavedStateId.reset();
		}
	}

	bool restoreDocumentSnapshot(shared_ptr<core::Building>& building, bool redo)
	{
		auto& source = redo ? gRedoHistory : gUndoHistory;
		auto& destination = redo ? gUndoHistory : gRedoHistory;
		if (!building || source.empty()) return false;

		auto current = captureDocumentSnapshot(building);
		if (!current) return false;
		try
		{
			auto const& target = source.back();
			auto loaded = make_shared<core::Building>("Loading", 1, 1);
			auto serializer = core::YamlSerializer::fromString(target.yaml);
			serializer->deserialize();
			core::SerializationWorkData workData;
			loaded->deserialize(*serializer, workData);
			if (!gSavedStateId || target.stateId != *gSavedStateId) loaded->markModified();

			destination.push_back(std::move(*current));
			if (destination.size() > MaximumUndoHistory) destination.pop_front();
			gCurrentStateId = target.stateId;
			source.pop_back();
			building = std::move(loaded);
			clearDocumentState(false);
			setWorldPaused(building, true);
			return true;
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Undo", 0, core::LogLevel::Error,
				"Could not restore editor state: " + string(error.what()));
			return false;
		}
	}

	bool isDocumentStale(shared_ptr<core::Building> const& building)
	{
		return building && building->isModified();
	}

	void reportFileError(string message)
	{
		gFileError = std::move(message);
		gOpenFileErrorPopup = true;
		core::addLogMessage("File", 0, core::LogLevel::Error, gFileError);
	}

	bool saveBuilding(shared_ptr<core::Building> const& building, bool saveAs)
	{
		if (!building) return false;

		string filepath = gBuildingFilepath;
		if (saveAs || filepath.empty())
		{
			nfdu8char_t* selectedPathRaw{ nullptr };
			nfdu8filteritem_t const filters[] = { { "Building YAML", "yaml,yml" } };
			filesystem::path const current(filepath);
			auto const directory = filepath.empty() ? string() : current.parent_path().string();
			auto defaultName = filepath.empty()
				? building->getName() + ".yaml"
				: current.filename().string();
			auto const result = NFD_SaveDialogU8(&selectedPathRaw, filters, 1,
				directory.empty() ? nullptr : directory.c_str(), defaultName.c_str());
			unique_ptr<nfdu8char_t, decltype(&NFD_FreePathU8)> selectedPath(
				selectedPathRaw, NFD_FreePathU8);
			if (result == NFD_CANCEL) return false;
			if (result == NFD_ERROR)
			{
				reportFileError(string("Could not choose a save location: ")
					+ (NFD_GetError() ? NFD_GetError() : "unknown native dialog error"));
				return false;
			}
			filepath = selectedPath.get();
			filesystem::path selected(filepath);
			if (!selected.has_extension()) filepath += ".yaml";
		}

		try
		{
			auto serializer = core::YamlSerializer::toFile(filepath);
			core::SerializationWorkData workData;
			building->serialize(*serializer, workData);
			serializer->serialize();
			gBuildingFilepath = std::move(filepath);
			gSavedStateId = gCurrentStateId;
			core::addLogMessage("File", 0, core::LogLevel::Info,
				"Saved Building to " + gBuildingFilepath);
			return true;
		}
		catch (std::exception const& error)
		{
			reportFileError("Could not save Building: " + string(error.what()));
			return false;
		}
	}

	string normalizedFilepath(string const& filepath)
	{
		filesystem::path path(filepath);
		error_code error;
		auto const absolute = filesystem::absolute(path, error);
		if (!error) path = absolute;
		return path.lexically_normal().string();
	}

	void addRecentFile(string const& filepath)
	{
		try
		{
			gRecentFiles.add(normalizedFilepath(filepath));
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("File", 0, core::LogLevel::Error,
				"Could not persist recent files: " + string(error.what()));
		}
	}

	void openBuilding(shared_ptr<core::Building>& building, string const& filepath)
	{
		try
		{
			auto const normalized = normalizedFilepath(filepath);
			auto loaded = make_shared<core::Building>("Loading", 1, 1);
			auto serializer = core::YamlSerializer::fromFile(normalized);
			serializer->deserialize();
			core::SerializationWorkData workData;
			loaded->deserialize(*serializer, workData);
			building = std::move(loaded);
			gBuildingFilepath = normalized;
			addRecentFile(gBuildingFilepath);
			clearDocumentState();
			gSavedStateId = gCurrentStateId;
			setWorldPaused(building, true);
			core::addLogMessage("File", 0, core::LogLevel::Info,
				"Opened Building from " + gBuildingFilepath);
		}
		catch (std::exception const& error)
		{
			reportFileError("Could not open Building: " + string(error.what()));
		}
	}

	void openBuilding(shared_ptr<core::Building>& building)
	{
		nfdu8char_t* selectedPathRaw{ nullptr };
		nfdu8filteritem_t const filters[] = { { "Building YAML", "yaml,yml" } };
		auto const result = NFD_OpenDialogU8(&selectedPathRaw, filters, 1, nullptr);
		unique_ptr<nfdu8char_t, decltype(&NFD_FreePathU8)> selectedPath(
			selectedPathRaw, NFD_FreePathU8);
		if (result == NFD_CANCEL) return;
		if (result == NFD_ERROR)
		{
			reportFileError(string("Could not choose a Building file: ")
				+ (NFD_GetError() ? NFD_GetError() : "unknown native dialog error"));
			return;
		}

		openBuilding(building, selectedPath.get());
	}

	void executeFileAction(PendingFileAction action, shared_ptr<core::Building>& building)
	{
		switch (action)
		{
		case PendingFileAction::New:
			gOpenNewBuildingPopup = true;
			break;
		case PendingFileAction::Open:
			openBuilding(building);
			break;
		case PendingFileAction::OpenRecent:
		{
			auto const filepath = std::move(gPendingRecentFilepath);
			gPendingRecentFilepath.clear();
			openBuilding(building, filepath);
			break;
		}
		case PendingFileAction::Close:
			building.reset();
			gBuildingFilepath.clear();
			clearDocumentState();
			break;
		case PendingFileAction::Exit:
			throw ExitApplicationException(0, "Exit");
		case PendingFileAction::None:
			break;
		}
	}

	void requestFileAction(PendingFileAction action, shared_ptr<core::Building>& building)
	{
		if (isDocumentStale(building))
		{
			gPendingFileAction = action;
			gOpenUnsavedChangesPopup = true;
			return;
		}
		executeFileAction(action, building);
	}

	void requestRecentFile(string const& filepath, shared_ptr<core::Building>& building)
	{
		gPendingRecentFilepath = filepath;
		requestFileAction(PendingFileAction::OpenRecent, building);
	}

	void commitLocationEdit(shared_ptr<core::Building> const& building,
		core::Building::LocationEditPlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto newIndex = building->applyLocationEdit(plan);
			gUISettings.worldPaused = true;
			gHoveredAgent = nullptr;
			gHoveredSector.reset();
			gHoveredSectorObject.reset();
			gSelectedAgent = nullptr;
			gSelectedSectorObject.reset();
			gSelectedSector = plan.remove ? nullptr : building->getSector(newIndex);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Sector editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Sector editor", 0, core::LogLevel::Error, error.what());
		}
		resetSectorResize();
	}

	void queueLocationEdit(shared_ptr<core::Building> const& building,
		core::Building::LocationEditPlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation())
		{
			gPendingLocationEdit = plan;
			gOpenLocationEditPopup = true;
		}
		else commitLocationEdit(building, plan);
	}

	void commitLiftEdit(shared_ptr<core::Building> const& building,
		core::Building::LiftEditPlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto newIndex = building->applyLiftEdit(plan);
			gUISettings.worldPaused = true;
			gHoveredAgent = nullptr; gHoveredSector.reset(); gHoveredSectorObject.reset();
			gSelectedAgent = nullptr; gSelectedSectorObject.reset();
			gSelectedSector = plan.remove ? nullptr : building->getSector(newIndex);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{ core::addLogMessage("Lift editor", 0, core::LogLevel::Error, error.getMessage()); }
		catch (std::exception const& error)
		{ core::addLogMessage("Lift editor", 0, core::LogLevel::Error, error.what()); }
		resetSectorResize();
	}

	void queueLiftEdit(shared_ptr<core::Building> const& building,
		core::Building::LiftEditPlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation())
		{
			gPendingLiftEdit = plan;
			gOpenLocationEditPopup = true;
		}
		else commitLiftEdit(building, plan);
	}

	void commitShuttleEdit(shared_ptr<core::Building> const& building,
		core::Building::ShuttleEditPlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto newIndex = building->applyShuttleEdit(plan);
			gUISettings.worldPaused = true;
			gHoveredAgent = nullptr; gHoveredSector.reset(); gHoveredSectorObject.reset();
			gSelectedAgent = nullptr; gSelectedSectorObject.reset();
			gSelectedSector = plan.remove ? nullptr : building->getSector(newIndex);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{ core::addLogMessage("Shuttle editor", 0, core::LogLevel::Error, error.getMessage()); }
		catch (std::exception const& error)
		{ core::addLogMessage("Shuttle editor", 0, core::LogLevel::Error, error.what()); }
		resetSectorResize();
	}

	void queueShuttleEdit(shared_ptr<core::Building> const& building,
		core::Building::ShuttleEditPlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation())
		{
			gPendingShuttleEdit = plan;
			gOpenLocationEditPopup = true;
		}
		else commitShuttleEdit(building, plan);
	}

	void commitLadderEdit(shared_ptr<core::Building> const& building,
		core::Building::LadderEditPlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto newIndex = building->applyLadderEdit(plan);
			gUISettings.worldPaused = true;
			gHoveredAgent = nullptr; gHoveredSector.reset(); gHoveredSectorObject.reset();
			gSelectedAgent = nullptr; gSelectedSectorObject.reset();
			gSelectedSector = plan.remove ? nullptr : building->getSector(newIndex);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{ core::addLogMessage("Ladder editor", 0, core::LogLevel::Error, error.getMessage()); }
		catch (std::exception const& error)
		{ core::addLogMessage("Ladder editor", 0, core::LogLevel::Error, error.what()); }
		resetSectorResize();
	}

	void queueLadderEdit(shared_ptr<core::Building> const& building,
		core::Building::LadderEditPlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation())
		{
			gPendingLadderEdit = plan;
			gOpenLocationEditPopup = true;
		}
		else commitLadderEdit(building, plan);
	}

	void commitStaircaseEdit(shared_ptr<core::Building> const& building,
		core::Building::StaircaseEditPlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto newIndex = building->applyStaircaseEdit(plan);
			gUISettings.worldPaused = true;
			gHoveredAgent = nullptr; gHoveredSector.reset(); gHoveredSectorObject.reset();
			gSelectedAgent = nullptr; gSelectedSectorObject.reset();
			gSelectedSector = plan.remove ? nullptr : building->getSector(newIndex);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{ core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, error.getMessage()); }
		catch (std::exception const& error)
		{ core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, error.what()); }
		resetSectorResize();
	}

	void queueStaircaseEdit(shared_ptr<core::Building> const& building,
		core::Building::StaircaseEditPlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation())
		{
			gPendingStaircaseEdit = plan;
			gOpenLocationEditPopup = true;
		}
		else commitStaircaseEdit(building, plan);
	}

	void renderFilePopups(shared_ptr<core::Building>& building)
	{
		if (gOpenUnsavedChangesPopup)
		{
			ImGui::OpenPopup("Unsaved changes");
			gOpenUnsavedChangesPopup = false;
		}
		if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("Save changes to the current Building?");
			if (ImGui::Button("Save"))
			{
				if (saveBuilding(building, false))
				{
					auto const action = gPendingFileAction;
					gPendingFileAction = PendingFileAction::None;
					ImGui::CloseCurrentPopup();
					executeFileAction(action, building);
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Discard"))
			{
				auto const action = gPendingFileAction;
				gPendingFileAction = PendingFileAction::None;
				ImGui::CloseCurrentPopup();
				executeFileAction(action, building);
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				gPendingFileAction = PendingFileAction::None;
				gPendingRecentFilepath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		if (gOpenNewBuildingPopup)
		{
			ImGui::OpenPopup("New Building");
			gOpenNewBuildingPopup = false;
		}
		if (ImGui::BeginPopupModal("New Building", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::InputText("Name", gNewBuildingName, sizeof(gNewBuildingName));
			ImGui::InputInt("Width", &gNewBuildingWidth);
			ImGui::InputInt("Decks", &gNewBuildingDecks);
			bool const valid = gNewBuildingName[0] != '\0'
				&& gNewBuildingWidth > 0 && gNewBuildingDecks > 0;
			if (ImGui::Button("Create") && valid)
			{
				building = make_shared<core::Building>(gNewBuildingName,
					static_cast<uint32_t>(gNewBuildingWidth),
					static_cast<uint32_t>(gNewBuildingDecks));
				gBuildingFilepath.clear();
				clearDocumentState();
				setWorldPaused(building, true);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
			if (!valid) ImGui::TextDisabled("Name, width, and deck count are required.");
			ImGui::EndPopup();
		}

		if (gOpenFileErrorPopup)
		{
			ImGui::OpenPopup("File error");
			gOpenFileErrorPopup = false;
		}
		if (ImGui::BeginPopupModal("File error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped("%s", gFileError.c_str());
			if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (gOpenShuttleDraftPopup)
		{
			ImGui::OpenPopup("Create Shuttle");
			gOpenShuttleDraftPopup = false;
		}
		if (ImGui::BeginPopupModal("Create Shuttle", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			if (!building || !gShuttleDraft)
			{
				ImGui::CloseCurrentPopup();
			}
			else
			{
				auto& draft = *gShuttleDraft;
				ImGui::Text("Track: deck %u, x %u..%u", draft.y, draft.x,
					draft.x + draft.cellsWide - 1);
				// Authoring is intentionally limited to a single carriage for now.
				draft.numCars = 1;
				ImGui::TextUnformatted("Carriages: 1");
				ImGui::InputInt("Carriage width", &draft.carWidth);
				if (draft.carWidth > 0 && draft.carWidth < 32)
					draft.doorMask &= (1u << draft.carWidth) - 1;
				ImGui::TextUnformatted("Carriage door layout (click cells to toggle)");
				if (draft.carWidth == 3 || draft.carWidth == 4)
				{
					for (int cell = 0; cell < draft.carWidth; ++cell)
					{
						bool door = (draft.doorMask & (1u << cell)) != 0;
						ImGui::PushID(cell);
						if (ImGui::Selectable(door ? "Door" : "Wall", door,
							ImGuiSelectableFlags_DontClosePopups, ImVec2(58.0f, 36.0f)))
							draft.doorMask ^= 1u << cell;
						ImGui::PopID();
						if (cell + 1 < draft.carWidth) ImGui::SameLine();
					}
				}
				ImGui::InputInt("Capacity per carriage", &draft.capacity);
				ImGui::InputFloat("Minimum dwell (seconds)", &draft.minimumDwellSeconds, 0.1f, 1.0f, "%.2f");
				ImGui::InputFloat("Maximum boarding (seconds)", &draft.maximumBoardingSeconds, 0.1f, 1.0f, "%.2f");
				ImGui::Checkbox("Allow partial landings", &draft.allowPartialLandings);

				auto candidates = shuttleCandidates(building, draft);
				draft.stopOffsets.erase(remove_if(draft.stopOffsets.begin(), draft.stopOffsets.end(),
					[&](auto value) { return find(candidates.begin(), candidates.end(), value) == candidates.end(); }),
					draft.stopOffsets.end());
				auto vehicleWidth = (uint32_t)max(0,
					draft.numCars * draft.carWidth + draft.numCars - 1);
				vector<uint32_t> endpointSuggestions;
				if (!candidates.empty())
				{
					auto last = find_if(candidates.rbegin(), candidates.rend(), [&](auto value)
						{ return value - candidates.front() >= vehicleWidth; });
					if (last != candidates.rend()) endpointSuggestions = { candidates.front(), *last };
				}
				ImGui::Separator();
				ImGui::Text("Stops (%zu valid alignments)", candidates.size());
				if (candidates.empty())
				{
					ImGui::TextColored(ImVec4(1, 0.65f, 0.2f, 1),
						"No selectable platform alignments exist for this configuration.");
					ImGui::TextWrapped("Each configured Door must align with an unobstructed "
						"Fore-layer Location cell. Extend the platforms, change the Door layout, "
						"or allow partial landings.");
				}
				else if (endpointSuggestions.empty())
					ImGui::TextColored(ImVec4(1, 0.65f, 0.2f, 1),
						"Valid alignments exist, but no two are at least %u cells apart.", vehicleWidth);
				for (auto offset : candidates)
				{
					bool selected = find(draft.stopOffsets.begin(), draft.stopOffsets.end(), offset)
						!= draft.stopOffsets.end();
					ImGui::PushID((int)offset);
					if (ImGui::Checkbox("##stop", &selected))
					{
						if (selected) draft.stopOffsets.push_back(offset);
						else draft.stopOffsets.erase(remove(draft.stopOffsets.begin(),
							draft.stopOffsets.end(), offset), draft.stopOffsets.end());
					}
					ImGui::SameLine();
					uint32_t coverage = 0;
					auto fore = static_cast<core::Building const&>(*building).getLayer(CORE_LAYER_FORE);
					uint32_t selectedDoors = 0;
					for (int cell = 0; cell < draft.carWidth; ++cell)
						selectedDoors += (draft.doorMask & (1u << cell)) != 0;
					for (int car = 0; car < draft.numCars; ++car)
						for (int doorOffset = 0; doorOffset < draft.carWidth; ++doorOffset)
						{
							if ((draft.doorMask & (1u << doorOffset)) == 0) continue;
							auto doorX = draft.x + offset + car * (draft.carWidth + 1) + doorOffset;
							coverage += fore->getCellDefinition(doorX, draft.y).sectorIndex != ~0u;
						}
					ImGui::Text("offset %u (global x %u, %u/%u door landings)",
						offset, draft.x + offset, coverage,
						(uint32_t)draft.numCars * selectedDoors);
					ImGui::PopID();
				}
				if (draft.stopOffsets.size() < 2 && !endpointSuggestions.empty()
					&& ImGui::Button("Use endpoint suggestions"))
					draft.stopOffsets = endpointSuggestions;
				sort(draft.stopOffsets.begin(), draft.stopOffsets.end());
				bool valid = validateShuttleDraft(building, draft);
				if (!draft.stopOffsets.empty())
				{
					vector<string> labels;
					string items;
					for (auto offset : draft.stopOffsets)
					{
						labels.push_back(format("Stop at x {}", draft.x + offset));
						items += labels.back(); items += '\0';
					}
					ImGui::Combo("Initial stop", &draft.initialStop, items.c_str());
				}
				if (!valid) ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", draft.diagnostic.c_str());
				ImGui::BeginDisabled(!valid);
				if (ImGui::Button("Create"))
				{
					auto undo = captureDocumentSnapshot(building);
					try
					{
						core::Building::CreateShuttleOptions options{
							(uint32_t)draft.numCars, (uint32_t)draft.carWidth, draft.stopOffsets,
							(uint32_t)draft.initialStop, (uint32_t)draft.capacity,
							draft.minimumDwellSeconds, draft.maximumBoardingSeconds,
							draft.allowPartialLandings, draft.doorMask };
						auto created = building->addShuttle(draft.y, draft.x, draft.cellsWide, options);
						building->finishBuild();
						setSelectionMode(UISettings::SelectionMode::Sector);
						gSelectedSector = created.shuttle.sector;
						commitDocumentEdit(std::move(undo));
						gShuttleDraft.reset();
						ImGui::CloseCurrentPopup();
					}
					catch (core::Exception const& error)
					{ draft.diagnostic = error.getMessage(); }
					catch (std::exception const& error)
					{ draft.diagnostic = error.what(); }
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
				{
					gShuttleDraft.reset();
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}

		if (gOpenShuttleStopPopup)
		{
			ImGui::OpenPopup("Add Shuttle stop");
			gOpenShuttleStopPopup = false;
		}
		if (ImGui::BeginPopupModal("Add Shuttle stop", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			if (!building || gShuttleDoorCandidates.empty()) ImGui::CloseCurrentPopup();
			else
			{
				string items;
				for (auto const& candidate : gShuttleDoorCandidates)
				{
					items += format("Shuttle {} at offset {}", candidate.sectorIndex, candidate.stopOffset);
					items += '\0';
				}
				ImGui::Combo("Alignment", &gSelectedShuttleDoorCandidate, items.c_str());
				if (ImGui::Button("Add stop"))
				{
					auto candidate = gShuttleDoorCandidates[(size_t)gSelectedShuttleDoorCandidate];
					auto plan = building->planAddShuttleStop(candidate.sectorIndex, candidate.stopOffset);
					gShuttleDoorCandidates.clear();
					ImGui::CloseCurrentPopup();
					if (!plan.valid) core::addLogMessage("Shuttle editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueShuttleEdit(building, plan);
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
				{
					gShuttleDoorCandidates.clear();
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndPopup();
		}

		if (gOpenLocationEditPopup)
		{
			ImGui::OpenPopup("Confirm sector edit");
			gOpenLocationEditPopup = false;
		}
		if (ImGui::BeginPopupModal("Confirm sector edit", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextUnformatted("This edit will also:");
			ImGui::Separator();
			if (gPendingLocationEdit)
				for (auto const& consequence : gPendingLocationEdit->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			if (gPendingLiftEdit)
				for (auto const& consequence : gPendingLiftEdit->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			if (gPendingShuttleEdit)
				for (auto const& consequence : gPendingShuttleEdit->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			if (gPendingLadderEdit)
				for (auto const& consequence : gPendingLadderEdit->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			if (gPendingStaircaseEdit)
				for (auto const& consequence : gPendingStaircaseEdit->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			ImGui::Separator();
			if (ImGui::Button("OK") && building
				&& (gPendingLocationEdit || gPendingLiftEdit || gPendingShuttleEdit
					|| gPendingLadderEdit || gPendingStaircaseEdit))
			{
				auto locationPlan = gPendingLocationEdit;
				auto liftPlan = gPendingLiftEdit;
				auto shuttlePlan = gPendingShuttleEdit;
				auto ladderPlan = gPendingLadderEdit;
				auto staircasePlan = gPendingStaircaseEdit;
				gPendingLocationEdit.reset(); gPendingLiftEdit.reset(); gPendingShuttleEdit.reset();
				gPendingLadderEdit.reset(); gPendingStaircaseEdit.reset();
				ImGui::CloseCurrentPopup();
				if (locationPlan) commitLocationEdit(building, *locationPlan);
				else if (liftPlan) commitLiftEdit(building, *liftPlan);
				else if (shuttlePlan) commitShuttleEdit(building, *shuttlePlan);
				else if (ladderPlan) commitLadderEdit(building, *ladderPlan);
				else commitStaircaseEdit(building, *staircasePlan);
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				gPendingLocationEdit.reset();
				gPendingLiftEdit.reset();
				gPendingShuttleEdit.reset();
				gPendingLadderEdit.reset();
				gPendingStaircaseEdit.reset();
				resetSectorResize();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	enum class ClipboardObjectType { Agent, Door, Window, Marker, Walkway, RoomLadder };
	struct ClipboardDefinition
	{
		ClipboardObjectType type{};
		bool cut{ false };
		string name;
		uint32_t flags{ 0 };
		core::Building::CreateDoorOptions door;
		core::Building::CreateWindowOptions window;
		core::Building::CreateLadderOptions ladder{ 0, false, true };
		uint32_t width{ 1 }, height{ 1 };
	};

	optional<core::Vector2> gLastWorldCursor;
	string gClipboardError;
	double gClipboardErrorUntil{ 0.0 };
	string gConsumedCutClipboard;

	void reportClipboardError(string message)
	{
		gClipboardError = std::move(message);
		gClipboardErrorUntil = ImGui::GetTime() + 3.0;
		core::addLogMessage("Clipboard", 0, core::LogLevel::Error, gClipboardError);
	}

	bool hasClipboardSelection()
	{
		if (gUISettings.selectionMode != UISettings::SelectionMode::Object) return false;
		if (gSelectedAgent) return true;
		if (!gSelectedSectorObject) return false;
		auto type = gSelectedSectorObject->getObjectType();
		return type == core::SectorObjectType::Door || type == core::SectorObjectType::Window
			|| type == core::SectorObjectType::Marker || type == core::SectorObjectType::Walkway
			|| type == core::SectorObjectType::Ladder;
	}

	char const* activationModeName(core::DoorActivationMode mode)
	{
		switch (mode)
		{
		case core::DoorActivationMode::Automatic: return "Automatic";
		case core::DoorActivationMode::Manual: return "Manual";
		case core::DoorActivationMode::RemoteControlled: return "RemoteControlled";
		case core::DoorActivationMode::Unavailable: return "Unavailable";
		}
		return "Manual";
	}

	char const* windowStateName(core::Window::State state)
	{
		switch (state)
		{
		case core::Window::State::Open: return "Open";
		case core::Window::State::Opening: return "Opening";
		case core::Window::State::Closed: return "Closed";
		case core::Window::State::Closing: return "Closing";
		case core::Window::State::Broken: return "Broken";
		case core::Window::State::Frosted: return "Frosted";
		case core::Window::State::Frosting: return "Frosting";
		case core::Window::State::Unfrosting: return "Unfrosting";
		case core::Window::State::Tinted: return "Tinted";
		case core::Window::State::Tinting: return "Tinting";
		case core::Window::State::Untinting: return "Untinting";
		}
		return "Closed";
	}

	char const* windowStyleName(core::Window::Style style)
	{
		switch (style)
		{
		case core::Window::Style::Clear: return "Clear";
		case core::Window::Style::Tinted: return "Tinted";
		case core::Window::Style::Frosted: return "Frosted";
		}
		return "Clear";
	}

	string uniqueAgentName(shared_ptr<const core::Building> const& building, string base)
	{
		set<string> names;
		for (auto const& agent : building->getSimulationSnapshot().agents) names.insert(agent.name);
		if (!names.contains(base)) return base;
		for (uint32_t suffix = 2;; ++suffix)
		{
			auto candidate = format("{} {}", base, suffix);
			if (!names.contains(candidate)) return candidate;
		}
	}

	optional<string> serializeClipboardSelection(shared_ptr<const core::Building> const& building,
		bool cut)
	{
		if (!hasClipboardSelection()) return nullopt;
		YAML::Emitter output;
		output << YAML::BeginMap << YAML::Key << "prometheumClipboard" << YAML::Value
			<< YAML::BeginMap << YAML::Key << "version" << YAML::Value << 1
			<< YAML::Key << "operation" << YAML::Value << (cut ? "cut" : "copy");
		if (gSelectedAgent)
		{
			auto name = cut ? gSelectedAgent->getName()
				: uniqueAgentName(building, gSelectedAgent->getName() + " copy");
			output << YAML::Key << "type" << YAML::Value << "Agent"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
				<< YAML::Key << "name" << YAML::Value << name
				<< YAML::Key << "flags" << YAML::Value << gSelectedAgent->getFlags()
				<< YAML::EndMap;
		}
		else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Door)
		{
			auto door = static_pointer_cast<const core::DoorSectorObject>(gSelectedSectorObject)->getDoor();
			core::Building::CreateDoorOptions options;
			if (!building->getSectorDoorOptions(gSelectedSectorObject->getCellY(),
				gSelectedSectorObject->getCellX(), door->getCellsWide(), options))
				throw runtime_error("The selected Door has no authored definition");
			output << YAML::Key << "type" << YAML::Value << "Door"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
				<< YAML::Key << "width" << YAML::Value << options.width
				<< YAML::Key << "controls" << YAML::Value << YAML::Flow << YAML::BeginSeq
				<< options.controls[0] << options.controls[1] << YAML::EndSeq
				<< YAML::Key << "activationMode" << YAML::Value << activationModeName(options.activationMode)
				<< YAML::Key << "holdOpenSeconds" << YAML::Value << options.holdOpenSeconds
				<< YAML::Key << "crossingLanes" << YAML::Value << options.crossingLanes
				<< YAML::EndMap;
		}
		else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Window)
		{
			auto window = static_pointer_cast<const core::WindowSectorObject>(gSelectedSectorObject)->getWindow();
			core::Building::CreateWindowOptions options;
			bool found = false;
			for (uint32_t layer = 0; layer < CORE_NUM_LAYERS && !found; ++layer)
				found = building->getSectorWindowOptions(layer, gSelectedSectorObject->getCellY(),
					gSelectedSectorObject->getCellX(), window->getCellsWide(), window->getDecksHigh(), options);
			if (!found) throw runtime_error("The selected Window has no authored definition");
			output << YAML::Key << "type" << YAML::Value << "Window"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
				<< YAML::Key << "width" << YAML::Value << window->getCellsWide()
				<< YAML::Key << "height" << YAML::Value << window->getDecksHigh()
				<< YAML::Key << "traversable" << YAML::Value << options.traversable
				<< YAML::Key << "initialState" << YAML::Value << windowStateName(options.initialState)
				<< YAML::Key << "style" << YAML::Value << windowStyleName(options.style)
				<< YAML::EndMap;
		}
		else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Walkway)
		{
			output << YAML::Key << "type" << YAML::Value << "Walkway"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap << YAML::EndMap;
		}
		else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Ladder)
		{
			auto owner = gSelectedSectorObject->getSector();
			uint32_t index = ~0u;
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == gSelectedSectorObject) { index = i; break; }
			core::Building::CreateLadderOptions options{};
			if (index == ~0u || !building->getRoomLadderOptions(owner->getIndex(), index, options))
				throw runtime_error("The selected Room Ladder has no authored definition");
			output << YAML::Key << "type" << YAML::Value << "RoomLadder"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
				<< YAML::Key << "extensible" << YAML::Value << options.extensible
				<< YAML::Key << "startExtended" << YAML::Value << options.startExtended
				<< YAML::Key << "agentSpacing" << YAML::Value << options.agentSpacing
				<< YAML::Key << "directionalBatchLimit" << YAML::Value << options.directionalBatchLimit
				<< YAML::EndMap;
		}
		else
		{
			output << YAML::Key << "type" << YAML::Value << "Marker"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap << YAML::EndMap;
		}
		output << YAML::EndMap << YAML::EndMap;
		if (!output.good()) throw runtime_error(output.GetLastError());
		return string(output.c_str());
	}

	template<typename T>
	T requiredYaml(YAML::Node const& map, char const* field)
	{
		if (!map[field]) throw runtime_error(format("Clipboard field '{}' is required", field));
		try { return map[field].as<T>(); }
		catch (std::exception const&) { throw runtime_error(format("Clipboard field '{}' has an invalid value", field)); }
	}

	ClipboardDefinition parseClipboard(string const& text)
	{
		auto document = YAML::Load(text);
		auto root = document["prometheumClipboard"];
		if (!root || !root.IsMap()) throw runtime_error("Clipboard does not contain a supported object");
		if (requiredYaml<uint32_t>(root, "version") != 1)
			throw runtime_error("Clipboard object version is not supported");
		ClipboardDefinition definition;
		auto operation = requiredYaml<string>(root, "operation");
		if (operation != "copy" && operation != "cut") throw runtime_error("Clipboard operation is not supported");
		definition.cut = operation == "cut";
		auto type = requiredYaml<string>(root, "type");
		auto object = root["object"];
		if (!object || !object.IsMap()) throw runtime_error("Clipboard object definition is required");
		if (type == "Agent")
		{
			definition.type = ClipboardObjectType::Agent;
			definition.name = requiredYaml<string>(object, "name");
			definition.flags = requiredYaml<uint32_t>(object, "flags");
			if (definition.name.empty()) throw runtime_error("Agent name cannot be empty");
		}
		else if (type == "Door")
		{
			definition.type = ClipboardObjectType::Door;
			definition.door.width = requiredYaml<uint32_t>(object, "width");
			auto controls = object["controls"];
			if (!controls || !controls.IsSequence() || controls.size() != 2)
				throw runtime_error("Door controls must contain two values");
			definition.door.controls[0] = controls[0].as<bool>();
			definition.door.controls[1] = controls[1].as<bool>();
			auto mode = requiredYaml<string>(object, "activationMode");
			if (mode == "Automatic") definition.door.activationMode = core::DoorActivationMode::Automatic;
			else if (mode == "Manual") definition.door.activationMode = core::DoorActivationMode::Manual;
			else if (mode == "RemoteControlled") definition.door.activationMode = core::DoorActivationMode::RemoteControlled;
			else if (mode == "Unavailable") definition.door.activationMode = core::DoorActivationMode::Unavailable;
			else throw runtime_error("Door activationMode is invalid");
			definition.door.holdOpenSeconds = requiredYaml<float>(object, "holdOpenSeconds");
			definition.door.crossingLanes = requiredYaml<uint32_t>(object, "crossingLanes");
		}
		else if (type == "Window")
		{
			definition.type = ClipboardObjectType::Window;
			definition.width = requiredYaml<uint32_t>(object, "width");
			definition.height = requiredYaml<uint32_t>(object, "height");
			definition.window.traversable = requiredYaml<bool>(object, "traversable");
			auto state = requiredYaml<string>(object, "initialState");
			map<string, core::Window::State> states = {
				{ "Open", core::Window::State::Open }, { "Opening", core::Window::State::Opening },
				{ "Closed", core::Window::State::Closed }, { "Closing", core::Window::State::Closing },
				{ "Broken", core::Window::State::Broken }, { "Frosted", core::Window::State::Frosted },
				{ "Frosting", core::Window::State::Frosting }, { "Unfrosting", core::Window::State::Unfrosting },
				{ "Tinted", core::Window::State::Tinted }, { "Tinting", core::Window::State::Tinting },
				{ "Untinting", core::Window::State::Untinting }
			};
			if (!states.contains(state)) throw runtime_error("Window initialState is invalid");
			definition.window.initialState = states[state];
			auto style = requiredYaml<string>(object, "style");
			if (style == "Clear") definition.window.style = core::Window::Style::Clear;
			else if (style == "Tinted") definition.window.style = core::Window::Style::Tinted;
			else if (style == "Frosted") definition.window.style = core::Window::Style::Frosted;
			else throw runtime_error("Window style is invalid");
		}
		else if (type == "Marker") definition.type = ClipboardObjectType::Marker;
		else if (type == "Walkway") definition.type = ClipboardObjectType::Walkway;
		else if (type == "RoomLadder")
		{
			definition.type = ClipboardObjectType::RoomLadder;
			definition.ladder.extensible = requiredYaml<bool>(object, "extensible");
			definition.ladder.startExtended = requiredYaml<bool>(object, "startExtended");
			definition.ladder.agentSpacing = requiredYaml<float>(object, "agentSpacing");
			definition.ladder.directionalBatchLimit = requiredYaml<uint32_t>(object, "directionalBatchLimit");
		}
		else throw runtime_error("Clipboard object type is not supported");
		return definition;
	}

	bool removeClipboardSelection(shared_ptr<core::Building> const& building)
	{
		if (gSelectedAgent)
		{
			auto id = building->getAgentId(gSelectedAgent);
			if (!id) return false;
			auto selected = gSelectedAgent;
			selected->clearPath();
			if (!building->removeAgent(id)) return false;
			if (gHoveredAgent == selected) gHoveredAgent = nullptr;
			gSelectedAgent = nullptr;
			return true;
		}
		auto selected = gSelectedSectorObject;
		if (!selected) return false;
		auto sector = selected->getSector();
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			if (sector->getObject(i) != selected) continue;
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			auto type = selected->getObjectType();
			bool removed = type == core::SectorObjectType::Marker
				? building->removeSectorMarker(sector->getIndex(), i)
				: type == core::SectorObjectType::Door
					? building->removeSectorDoor(sector->getIndex(), i)
					: type == core::SectorObjectType::Window
						? building->removeSectorWindow(sector->getIndex(), i)
						: type == core::SectorObjectType::Ladder
							? building->removeRoomLadder(sector->getIndex(), i)
							: building->removeSectorWalkway(sector->getIndex(), i);
			if (!removed) return false;
			if (type == core::SectorObjectType::Marker) building->finishBuild();
			if (gHoveredSectorObject == selected) gHoveredSectorObject.reset();
			gSelectedSectorObject.reset();
			return true;
		}
		return false;
	}

	void restoreClipboardSnapshot(shared_ptr<core::Building>& building,
		DocumentSnapshot const& snapshot, bool wasPaused);

	void copyOrCutSelection(shared_ptr<core::Building>& building, bool cut)
	{
		if (!building || !hasClipboardSelection()) return;
		try
		{
			auto text = serializeClipboardSelection(building, cut);
			if (!text) return;
			auto previousClipboard = ImGui::GetClipboardText();
			string const previousText = previousClipboard ? previousClipboard : "";
			ImGui::SetClipboardText(text->c_str());
			auto readBack = ImGui::GetClipboardText();
			if (!readBack || *text != readBack)
			{
				ImGui::SetClipboardText(previousText.c_str());
				throw runtime_error("Could not update the system clipboard");
			}
			if (!cut) return;
			auto undo = captureDocumentSnapshot(building);
			if (!undo) throw runtime_error("Could not capture editor state");
			bool const wasPaused = building->isSimulationPaused();
			try
			{
				if (!wasPaused) building->pauseSimulation();
				gUISettings.worldPaused = true;
				if (!removeClipboardSelection(building)) throw runtime_error("Could not remove the selected object");
				gConsumedCutClipboard.clear();
				commitDocumentEdit(std::move(undo));
			}
			catch (...)
			{
				auto failure = current_exception();
				ImGui::SetClipboardText(previousText.c_str());
				restoreClipboardSnapshot(building, *undo, wasPaused);
				rethrow_exception(failure);
			}
		}
		catch (core::Exception const& error) { reportClipboardError(error.getMessage()); }
		catch (std::exception const& error) { reportClipboardError(error.what()); }
	}

	void restoreClipboardSnapshot(shared_ptr<core::Building>& building,
		DocumentSnapshot const& snapshot, bool wasPaused)
	{
		auto loaded = make_shared<core::Building>("Loading", 1, 1);
		auto serializer = core::YamlSerializer::fromString(snapshot.yaml);
		serializer->deserialize();
		core::SerializationWorkData workData;
		loaded->deserialize(*serializer, workData);
		building = std::move(loaded);
		clearDocumentState(false);
		if (wasPaused) setWorldPaused(building, true);
		else gUISettings.worldPaused = false;
	}

	void pasteClipboard(shared_ptr<core::Building>& building, bool useCurrentCursor = false)
	{
		if (!building) return;
		if (gPegman.phase != PalettePhase::Home || gPaint.tool != PaintTool::None
			|| gAgentMove.dragging || gObjectMove.dragging || gSectorResize.dragging
			|| gPendingLocationEdit || gPendingLiftEdit || gPendingShuttleEdit
			|| gPendingLadderEdit || gPendingStaircaseEdit || gShuttleDraft
			|| !gShuttleDoorCandidates.empty())
		{
			reportClipboardError("Finish the current placement first");
			return;
		}
		if (useCurrentCursor)
		{
			auto const mouse = ImGui::GetIO().MousePos;
			auto const canvasMin = ImVec2(gUISettings.worldViewportX, gUISettings.worldViewportY);
			auto const canvasMax = canvasMin + ImVec2(gUISettings.worldViewportWidth,
				gUISettings.worldViewportHeight);
			if (!pointInRect(mouse, canvasMin, canvasMax))
			{
				reportClipboardError("Paste position is outside the world");
				return;
			}
			gLastWorldCursor = screenToWorld(mouse);
		}
		if (!gLastWorldCursor)
		{
			reportClipboardError("Move the cursor over the world before pasting");
			return;
		}
		auto clipboard = ImGui::GetClipboardText();
		if (!clipboard || !*clipboard) { reportClipboardError("Clipboard does not contain a supported object"); return; }
		string const clipboardText = clipboard;
		try
		{
			auto definition = parseClipboard(clipboardText);
			auto world = *gLastWorldCursor;
			if (world.x < 0.0f || world.y < 0.0f) throw runtime_error("Paste position is outside the building");
			auto x = static_cast<uint32_t>(floor(world.x));
			auto y = static_cast<uint32_t>(floor(world.y));
			if (definition.type == ClipboardObjectType::Agent)
			{
				auto sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
				if (!locationHasCapacity(sector) || !sector->pointInBounds(world.x, world.y))
					throw runtime_error("Agents require a viable sector with available capacity");
				if (y < sector->getCellY() || y >= sector->getCellY() + sector->getDecksHigh())
					throw runtime_error("Agent deck is outside the sector");
				bool consumedCut = definition.cut && clipboardText == gConsumedCutClipboard;
				if (definition.cut && !consumedCut)
				{
					auto unique = uniqueAgentName(building, definition.name);
					if (unique != definition.name) throw runtime_error("An Agent with this name already exists");
				}
				else definition.name = uniqueAgentName(building,
					consumedCut ? definition.name + " copy" : definition.name);
				if (!building->isSimulationPaused()) building->pauseSimulation();
				gUISettings.worldPaused = true;
				float halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
				float localX = clamp(world.x - sector->getPosition().x, halfWidth,
					max(halfWidth, sector->getSize().x - halfWidth));
				gPegman.phase = PalettePhase::Falling;
				gPegman.item = PaletteItem::Agent;
				gPegman.sector = sector;
				gPegman.deckOffset = y - sector->getCellY();
				gPegman.localX = localX;
				gPegman.feetY = world.y;
				gPegman.floorY = static_cast<float>(y);
				gPegman.velocity = 0.0f;
				gPegman.pastedAgentName = definition.name;
				gPegman.pastedAgentFlags = definition.flags;
				if (definition.cut) gConsumedCutClipboard = clipboardText;
				if (gPegman.feetY <= gPegman.floorY) landPegman(building);
				return;
			}

			string diagnostic;
			shared_ptr<const core::Sector> markerSector;
			float markerOffset = 0.0f;
			if (definition.type == ClipboardObjectType::Door)
			{
				uint32_t landingX, landingWidth;
				if (building->getLiftLandingGeometry(y, x, landingX, landingWidth))
				{
					x = landingX;
					definition.door = {};
					definition.door.width = landingWidth;
				}
				if (!building->canAddCorridorDoor(y, x, definition.door, &diagnostic))
					throw runtime_error(diagnostic);
			}
			else if (definition.type == ClipboardObjectType::Window)
			{
				if (!building->canAddSectorWindow(gUISettings.visibleLayer, y, x,
					definition.width, definition.height, &diagnostic)) throw runtime_error(diagnostic);
			}
			else if (definition.type == ClipboardObjectType::Walkway)
			{
				markerSector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
				auto room = dynamic_pointer_cast<const core::Location>(markerSector);
				if (!room || room->isCorridor() || !markerSector->pointInBounds(world.x, world.y))
					throw runtime_error("Walkways can only be placed in Rooms");
				if (!building->canAddSectorWalkway(markerSector->getIndex(),
					y - markerSector->getCellY(), x - markerSector->getCellX(), &diagnostic))
					throw runtime_error(diagnostic);
			}
			else if (definition.type == ClipboardObjectType::RoomLadder)
			{
				markerSector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
				auto room = dynamic_pointer_cast<const core::Location>(markerSector);
				if (!room || room->isCorridor() || !markerSector->pointInBounds(world.x, world.y))
					throw runtime_error("Room Ladders can only be placed in Rooms");
				if (!building->canAddRoomLadder(markerSector->getIndex(),
					y - markerSector->getCellY(), x - markerSector->getCellX(), nullptr, &diagnostic))
					throw runtime_error(diagnostic);
			}
			else
			{
				markerSector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
				if (!markerSector || !markerSector->pointInBounds(world.x, world.y))
					throw runtime_error("Markers require a viable sector");
				markerOffset = world.x - markerSector->getPosition().x;
				if (!building->canAddSectorMarker(markerSector->getIndex(),
					y - markerSector->getCellY(), markerOffset, &diagnostic)) throw runtime_error(diagnostic);
			}

			auto undo = captureDocumentSnapshot(building);
			if (!undo) throw runtime_error("Could not capture editor state");
			bool wasPaused = building->isSimulationPaused();
			try
			{
				if (!wasPaused) building->pauseSimulation();
				gUISettings.worldPaused = true;
				shared_ptr<const core::SectorObject> created;
				if (definition.type == ClipboardObjectType::Door)
				{
					auto result = building->addSectorDoor(y, x, definition.door);
					created = result.door.sector->getObject(result.door.index);
				}
				else if (definition.type == ClipboardObjectType::Window)
				{
					auto result = building->addSectorWindow(gUISettings.visibleLayer, y, x,
						definition.width, definition.height, definition.window);
					created = result.window.sector->getObject(result.window.index);
				}
				else if (definition.type == ClipboardObjectType::Walkway)
				{
					auto result = building->addSectorWalkway(markerSector->getIndex(),
						y - markerSector->getCellY(), x - markerSector->getCellX());
					created = result.sector->getObject(result.index);
				}
				else if (definition.type == ClipboardObjectType::RoomLadder)
				{
					auto result = building->addRoomLadder(markerSector->getIndex(),
						y - markerSector->getCellY(), x - markerSector->getCellX(), definition.ladder);
					created = result.ladder.sector->getObject(result.ladder.index);
				}
				else
				{
					auto result = building->addSectorMarker(markerSector->getIndex(),
						y - markerSector->getCellY(), markerOffset);
					created = result.sector->getObject(result.index);
				}
				building->finishBuild();
				setSelectionMode(UISettings::SelectionMode::Object);
				gSelectedAgent = nullptr;
				gSelectedSector.reset();
				gSelectedSectorObject = created;
				commitDocumentEdit(std::move(undo));
				if (definition.cut) gConsumedCutClipboard = clipboardText;
			}
			catch (...)
			{
				auto failure = current_exception();
				restoreClipboardSnapshot(building, *undo, wasPaused);
				rethrow_exception(failure);
			}
		}
		catch (core::Exception const& error) { reportClipboardError(error.getMessage()); }
		catch (std::exception const& error) { reportClipboardError(error.what()); }
	}
}

bool requestApplicationClose(shared_ptr<core::Building>& building)
{
	if (!isDocumentStale(building)) return true;
	requestFileAction(PendingFileAction::Exit, building);
	return false;
}

void handleShortcuts(shared_ptr<core::Building>& building)
{
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, 0, ImGuiInputFlags_RouteGlobalLow))
		requestFileAction(PendingFileAction::New, building);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, 0, ImGuiInputFlags_RouteGlobalLow))
		requestFileAction(PendingFileAction::Open, building);
	if (isDocumentStale(building)
		&& ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, 0, ImGuiInputFlags_RouteGlobalLow))
		saveBuilding(building, false);

	if (!building) return;

	bool const clipboardShortcutAvailable = !ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused();
	if (clipboardShortcutAvailable
		&& ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, 0, ImGuiInputFlags_RouteGlobalLow))
		copyOrCutSelection(building, true);
	if (clipboardShortcutAvailable
		&& ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, 0, ImGuiInputFlags_RouteGlobalLow))
		copyOrCutSelection(building, false);
	if (clipboardShortcutAvailable
		&& ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		pasteClipboard(building, true);
	}

	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, 0, ImGuiInputFlags_RouteGlobalLow))
		restoreDocumentSnapshot(building, false);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, 0, ImGuiInputFlags_RouteGlobalLow))
		restoreDocumentSnapshot(building, true);

	if (gSelectingAgentPathDestination
		&& ImGui::Shortcut(ImGuiKey_Escape, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		endAgentPathSelection();
	}

	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, 0, ImGuiInputFlags_RouteGlobalLow)
		&& gSelectedAgent && !ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused())
	{
		beginAgentPathSelection();
	}

	// World pause
	if (!ImGui::GetIO().KeyCtrl
		&& ImGui::Shortcut(ImGuiKey_P, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused())
		{
			setWorldPaused(building, !gUISettings.worldPaused);
		}
	}

	// Grid
	if (ImGui::Shortcut(ImGuiKey_G, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused())
		{
			gUISettings.renderGrid = !gUISettings.renderGrid;
		}
	}

	// Delete the selected Agent or independently authored object.
	if (ImGui::Shortcut(ImGuiKey_Delete, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused())
		{
			if (gUISettings.selectionMode == UISettings::SelectionMode::Sector && gSelectedSector)
			{
				if (gSelectedSector->getType() == core::SectorType::Lift)
				{
					auto plan = building->planRemoveLift(gSelectedSector->getIndex());
					if (!plan.valid) core::addLogMessage("Lift editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueLiftEdit(building, plan);
				}
				else if (gSelectedSector->getType() == core::SectorType::Shuttle)
				{
					auto plan = building->planRemoveShuttle(gSelectedSector->getIndex());
					if (!plan.valid) core::addLogMessage("Shuttle editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueShuttleEdit(building, plan);
				}
				else if (gSelectedSector->getType() == core::SectorType::Ladder)
				{
					auto plan = building->planRemoveLadder(gSelectedSector->getIndex());
					if (!plan.valid) core::addLogMessage("Ladder editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueLadderEdit(building, plan);
				}
				else if (gSelectedSector->getType() == core::SectorType::Staircase)
				{
					auto plan = building->planRemoveStaircase(gSelectedSector->getIndex());
					if (!plan.valid) core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueStaircaseEdit(building, plan);
				}
				else
				{
					auto plan = building->planRemoveLocation(gSelectedSector->getIndex());
					if (!plan.valid)
						core::addLogMessage("Sector editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueLocationEdit(building, plan);
				}
			}
			else if (gSelectedAgent)
			{
				auto id = building->getAgentId(gSelectedAgent);
				if (id)
				{
					auto undo = captureDocumentSnapshot(building);
					auto selected = gSelectedAgent;
					selected->clearPath();
					if (building->removeAgent(id))
					{
						if (gHoveredAgent == selected) gHoveredAgent = nullptr;
						gSelectedAgent = nullptr;
						commitDocumentEdit(std::move(undo));
					}
				}
			}
			else if (gSelectedSectorObject
				&& (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Marker
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Door
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Window
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Walkway
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Ladder))
			{
				uint32_t liftIndex, stopIndex;
				if (building->isLiftOwnedDoor(gSelectedSectorObject, &liftIndex, &stopIndex))
				{
					auto plan = building->planRemoveLiftStop(liftIndex, stopIndex);
					if (!plan.valid) core::addLogMessage("Lift editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueLiftEdit(building, plan);
				}
				else if (building->isShuttleOwnedDoor(gSelectedSectorObject, &liftIndex, &stopIndex))
				{
					auto plan = building->planRemoveShuttleStop(liftIndex, stopIndex);
					if (!plan.valid) core::addLogMessage("Shuttle editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueShuttleEdit(building, plan);
				}
				else try
				{
					auto undo = captureDocumentSnapshot(building);
					auto selected = gSelectedSectorObject;
					auto sector = selected->getSector();
					for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
					{
						if (sector->getObject(i) != selected) continue;
						if (!building->isSimulationPaused()) building->pauseSimulation();
						gUISettings.worldPaused = true;
						auto type = selected->getObjectType();
						bool removed = type == core::SectorObjectType::Marker
							? building->removeSectorMarker(sector->getIndex(), i)
							: type == core::SectorObjectType::Door
								? building->removeSectorDoor(sector->getIndex(), i)
								: type == core::SectorObjectType::Window
									? building->removeSectorWindow(sector->getIndex(), i)
									: type == core::SectorObjectType::Ladder
										? building->removeRoomLadder(sector->getIndex(), i)
										: building->removeSectorWalkway(sector->getIndex(), i);
						if (removed)
						{
							if (gHoveredSectorObject == selected) gHoveredSectorObject.reset();
							gHoveredAgent = nullptr;
							gSelectedAgent = nullptr;
							gSelectedSectorObject.reset();
							if (type == core::SectorObjectType::Marker) building->finishBuild();
							commitDocumentEdit(std::move(undo));
						}
						break;
					}
				}
				catch (core::Exception const& error)
				{
					core::addLogMessage("Editor", 0, core::LogLevel::Error, error.getMessage());
				}
				catch (std::exception const& error)
				{
					core::addLogMessage("Editor", 0, core::LogLevel::Error, error.what());
				}
			}
		}
	}

	// Object selection mode
	if (ImGui::Shortcut(ImGuiKey_O, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		setSelectionMode(UISettings::SelectionMode::Object);
	}

	// Vertex selection mode
	if (ImGui::Shortcut(ImGuiKey_V, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		setSelectionMode(UISettings::SelectionMode::Vertex);
	}

	// Sector selection mode
	if (ImGui::Shortcut(ImGuiKey_S, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		setSelectionMode(UISettings::SelectionMode::Sector);
	}

	// View
	if (ImGui::Shortcut(ImGuiKey_F2, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		gUISettings.visibleLayer = 1 - gUISettings.visibleLayer;
	}
	if (ImGui::Shortcut(ImGuiKey_F3, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		gUISettings.renderNonVisibleLayer = !gUISettings.renderNonVisibleLayer;
	}
	if (!gSelectingAgentPathDestination
		&& ImGui::Shortcut(ImGuiKey_F4, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		gUISettings.renderGraph = !gUISettings.renderGraph;
	}
	if (ImGui::Shortcut(ImGuiKey_F5, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		gUISettings.highlightNearestVertex = !gUISettings.highlightNearestVertex;
	}
}


void handleWorldInteraction(shared_ptr<core::Building> building,
	shared_ptr<const core::Graph> graph, MouseButtonStatus const& mouseStatus)
{
	if (!gPegmanConsumesLeftMouse
		&& mouseStatus.state[MouseButtonStatus::Left] == MouseButtonStatus::State::Clicked)
	{
		// Select the visually topmost item and adopt its selection mode.
		if (gHoveredVertex)
		{
			if (gSelectingAgentPathDestination && gSelectedAgent)
			{
				auto path = graph->calculatePath(gSelectedAgent, nullptr, gHoveredVertex);
				if (path)
				{
					if (applyAgentPathEdit(building, gSelectedAgent, std::move(path), true, true))
						endAgentPathSelection();
				}
				else
				{
					core::addLogMessage("Agent path", 0, core::LogLevel::Error,
						"No path is available to the selected vertex");
				}
			}
			else if (ImGui::GetIO().KeyCtrl)
			{
				if (gSelectedAgent)
				{
					auto path = graph->calculatePath(gSelectedAgent, nullptr, gHoveredVertex);
					applyAgentPathEdit(building, gSelectedAgent, std::move(path), false, false);
				}
			}
			else
			{
				setSelectionMode(UISettings::SelectionMode::Vertex);
				gSelectedVertex = gHoveredVertex;
			}
		}
		else if (gHoveredAgent)
		{
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = gHoveredAgent;
			gSelectedSector.reset();
			gSelectedSectorObject.reset();
		}
		else if (gHoveredInteractionPoint && !gUISettings.worldPaused && gSelectedAgent)
		{
			auto actor = building->getAgentId(gSelectedAgent);
			if (actor) building->requestInteraction(gHoveredInteractionPoint, actor);
		}
		else if (gHoveredSectorObject)
		{
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = gHoveredSectorObject;
		}
		else if (gHoveredSector)
		{
			setSelectionMode(UISettings::SelectionMode::Sector);
			gSelectedSector = gHoveredSector;
			gSelectedAgent = nullptr;
			gSelectedVertex.reset();
			gSelectedSectorObject.reset();
		}
	}

	if (!gPegmanConsumesLeftMouse
		&& mouseStatus.state[MouseButtonStatus::Right] == MouseButtonStatus::State::Clicked)
	{
		if (gSelectingAgentPathDestination) endAgentPathSelection();
		else clearSelections();
	}

	if (!gPegmanConsumesLeftMouse && mouseStatus.dragging[MouseButtonStatus::Left])
	{
		if (ImGui::GetIO().KeyShift)
		{
		}
		else
		{
		}
	}
}

void handleContinuousKeyboardInput(std::shared_ptr<core::Building> /* building */, uint64_t updateTimeMicros)
{
	const float MoveSpeed{ 500.0f };

	auto const& io = ImGui::GetIO();

	if (io.WantCaptureKeyboard)
	{
		return;
	}

	float frameTime = updateTimeMicros / 1000000.0f;

	[[maybe_unused]] float moveSpeed = MoveSpeed * frameTime * (io.KeyShift ? 4.0f : 1.0f);

	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
	{
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
	{
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow))
	{
	}
	if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow))
	{
	}

	if (ImGui::IsKeyDown(ImGuiKey_RightArrow))
	{
	}
	if (ImGui::IsKeyDown(ImGuiKey_LeftArrow))
	{
	}
	if (ImGui::IsKeyDown(ImGuiKey_UpArrow))
	{
	}
	if (ImGui::IsKeyDown(ImGuiKey_DownArrow))
	{
	}

	//worldBounds.getExtents(minExtent, maxExtent);
	//gViewOffset.x = clamp(gViewOffset.x, minExtent.x + APP_WINDOW_WIDTH * 0.5f, maxExtent.x - APP_WINDOW_WIDTH * 0.5f);
	//gViewOffset.y = clamp(gViewOffset.y, minExtent.y + APP_WINDOW_HEIGHT * 0.5f, maxExtent.y - APP_WINDOW_HEIGHT * 0.5f);
}


// ImGui helpers/widgets
namespace imgui
{

	void PushDisabled()
	{
		ImGuiContext& g = *GImGui;
		if ((g.CurrentItemFlags & ImGuiItemFlags_Disabled) == 0)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g.Style.Alpha * 0.6f);
		}

		ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
	}


	void PopDisabled()
	{
		ImGui::PopItemFlag();

		ImGuiContext& g = *GImGui;
		if ((g.CurrentItemFlags & ImGuiItemFlags_Disabled) == 0)
		{
			ImGui::PopStyleVar();
		}
	}

	bool ToggleButton(const char* str_id, const char* title, bool v)
	{
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		float height = ImGui::GetFrameHeight();
		float width = height * 1.55f;
		float radius = height * 0.50f;

		ImGui::InvisibleButton(str_id, ImVec2(width, height));

		bool clicked = ImGui::IsItemClicked();

		float t = v ? 1.0f : 0.0f;

		ImGuiContext& g = *GImGui;
		float ANIM_SPEED = 0.08f;
		if (g.LastActiveId == g.CurrentWindow->GetID(str_id))// && g.LastActiveIdTimer < ANIM_SPEED)
		{
			float t_anim = ImSaturate(g.LastActiveIdTimer / ANIM_SPEED);
			t = v ? (t_anim) : (1.0f - t_anim);
		}

		ImU32 col_bg;
		if (ImGui::IsItemHovered())
		{
			col_bg = ImGui::GetColorU32(ImLerp(ImVec4(0.78f, 0.78f, 0.78f, 1.0f), ImVec4(0.64f, 0.83f, 0.34f, 1.0f), t));
		}
		else
		{
			col_bg = ImGui::GetColorU32(ImLerp(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), ImVec4(0.56f, 0.83f, 0.26f, 1.0f), t));
		}

		draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col_bg, height * 0.5f);
		draw_list->AddCircleFilled(ImVec2(p.x + radius + t * (width - radius * 2.0f), p.y + radius), radius - 1.5f, IM_COL32(255, 255, 255, 255));

		ImGui::SameLine();
		ImGui::TextUnformatted(title);

		return clicked;
	}

	bool ToggleButton(const char* str_id, const char* title, bool* v)
	{
		auto clicked = ToggleButton(str_id, title, *v);

		if (clicked)
		{
			*v = !*v;
		}

		return clicked;
	}

} // imgui

void initializeRecentFiles(filesystem::path const& filepath)
{
	gRecentFiles.initialize(filepath);
}

ImVec2 gMainMenuWindowSize;

void renderMenu(shared_ptr<core::Building>& building)
{
	optional<string> recentFileToOpen;
	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New", "Ctrl+N"))
				requestFileAction(PendingFileAction::New, building);
			if (ImGui::MenuItem("Open...", "Ctrl+O"))
				requestFileAction(PendingFileAction::Open, building);
			if (ImGui::BeginMenu("Open Recent", !gRecentFiles.empty()))
			{
				for (auto const& filepath : gRecentFiles.entries())
				{
					if (ImGui::MenuItem(filepath.c_str())) recentFileToOpen = filepath;
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Save", "Ctrl+S", false, isDocumentStale(building)))
				saveBuilding(building, false);
			if (ImGui::MenuItem("Save As...", nullptr, false, building != nullptr))
				saveBuilding(building, true);
			if (ImGui::MenuItem("Close", nullptr, false, building != nullptr))
				requestFileAction(PendingFileAction::Close, building);
			ImGui::Separator();
			if (ImGui::MenuItem("Exit"))
				requestFileAction(PendingFileAction::Exit, building);

			ImGui::EndMenu();
		}
		
		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false,
				building != nullptr && !gUndoHistory.empty()))
				restoreDocumentSnapshot(building, false);
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false,
				building != nullptr && !gRedoHistory.empty()))
				restoreDocumentSnapshot(building, true);
			ImGui::Separator();
			bool const canCopy = building != nullptr && hasClipboardSelection();
			if (ImGui::MenuItem("Cut", "Ctrl+X", false, canCopy)) copyOrCutSelection(building, true);
			if (ImGui::MenuItem("Copy", "Ctrl+C", false, canCopy)) copyOrCutSelection(building, false);
			auto clipboard = ImGui::GetClipboardText();
			if (ImGui::MenuItem("Paste", "Ctrl+V", false,
				building != nullptr && clipboard && *clipboard)) pasteClipboard(building);
			ImGui::Separator();

			if (ImGui::BeginMenu("Selection"))
			{
				bool selected = gUISettings.selectionMode == UISettings::SelectionMode::Object;

				if (ImGui::MenuItem("Objects", 0, &selected))
				{
					if (selected)
					{
						setSelectionMode(UISettings::SelectionMode::Object);
					}
				}

				selected = gUISettings.selectionMode == UISettings::SelectionMode::Vertex;

				if (ImGui::MenuItem("Vertices", 0, &selected))
				{
					if (selected)
					{
						setSelectionMode(UISettings::SelectionMode::Vertex);
					}
				}

				selected = gUISettings.selectionMode == UISettings::SelectionMode::Sector;
				if (ImGui::MenuItem("Sectors", "S", &selected) && selected)
					setSelectionMode(UISettings::SelectionMode::Sector);

				ImGui::EndMenu();
			}

			ImGui::EndMenu();
		}
		
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::BeginMenu("Style"))
			{
				bool selected = gUISettings.style == UISettings::Style::Light;

				if (ImGui::MenuItem("Light", 0, &selected))
				{
					if (selected)
					{
						gUISettings.style = UISettings::Style::Light;
						ImGui::StyleColorsLight();
					}
				}

				selected = gUISettings.style == UISettings::Style::Dark;

				if (ImGui::MenuItem("Dark", 0, &selected))
				{
					if (selected)
					{
						gUISettings.style = UISettings::Style::Dark;
						ImGui::StyleColorsDark();
					}
				}

				selected = gUISettings.style == UISettings::Style::Classic;

				if (ImGui::MenuItem("Classic", 0, &selected))
				{
					if (selected)
					{
						gUISettings.style = UISettings::Style::Classic;
						ImGui::StyleColorsClassic();
					}
				}

				ImGui::EndMenu();
			}

			ImGui::MenuItem("Grid", "G", &gUISettings.renderGrid);
			ImGui::MenuItem("Show non-visible layer", "F3", &gUISettings.renderNonVisibleLayer);
			ImGui::MenuItem("Building graph", "F4", &gUISettings.renderGraph);
			ImGui::MenuItem("Highlight nearest vertex", "F5", &gUISettings.highlightNearestVertex);

			ImGui::EndMenu();
		}
		
		gMainMenuWindowSize = ImGui::GetWindowSize();
		ImGui::EndMainMenuBar();
	}

	if (recentFileToOpen) requestRecentFile(*recentFileToOpen, building);
}


void renderDocumentToolbar(shared_ptr<core::Building>& building)
{
	ImGuiViewportP* viewport = (ImGuiViewportP*)(void*)ImGui::GetMainViewport();
	auto const& style = ImGui::GetStyle();
	float const height = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
	ImGuiWindowFlags const windowFlags = ImGuiWindowFlags_NoScrollbar
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;

	if (!ImGui::BeginViewportSideBar("##MainToolbar", viewport, ImGuiDir_Up,
		height, windowFlags))
	{
		ImGui::End();
		return;
	}

	if (ImGui::Button(ICON_FA_FILE "##NewDocument"))
		requestFileAction(PendingFileAction::New, building);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("New (Ctrl+N)");

	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_FOLDER_OPEN "##OpenDocument"))
		requestFileAction(PendingFileAction::Open, building);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open... (Ctrl+O)");

	ImGui::SameLine();
	ImGui::BeginDisabled(!isDocumentStale(building));
	if (ImGui::Button(ICON_FA_SAVE "##SaveDocument")) saveBuilding(building, false);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save (Ctrl+S)");
	ImGui::EndDisabled();

	ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
	ImGui::BeginDisabled(building == nullptr || gUndoHistory.empty());
	if (ImGui::Button(ICON_FA_UNDO "##Undo")) restoreDocumentSnapshot(building, false);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo (Ctrl+Z)");
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(building == nullptr || gRedoHistory.empty());
	if (ImGui::Button(ICON_FA_REDO "##Redo")) restoreDocumentSnapshot(building, true);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Redo (Ctrl+Y)");
	ImGui::EndDisabled();

	ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
	ImGui::BeginDisabled(building == nullptr || !hasClipboardSelection());
	if (ImGui::Button(ICON_FA_CUT "##Cut")) copyOrCutSelection(building, true);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cut (Ctrl+X)");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_COPY "##Copy")) copyOrCutSelection(building, false);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy (Ctrl+C)");
	ImGui::EndDisabled();

	auto clipboard = ImGui::GetClipboardText();
	ImGui::SameLine();
	ImGui::BeginDisabled(building == nullptr || !clipboard || !*clipboard);
	if (ImGui::Button(ICON_FA_PASTE "##Paste")) pasteClipboard(building);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paste (Ctrl+V)");
	ImGui::EndDisabled();

	ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);
	imgui::ToggleButton("ToggleNonVisibleLayer", "Show non-visible layer", &gUISettings.renderNonVisibleLayer);
	ImGui::SameLine();
	imgui::ToggleButton("ToggleGraph", "Building graph", &gUISettings.renderGraph);
	ImGui::SameLine();
	imgui::ToggleButton("Agent Debug", "Agent debug", &gUISettings.renderAgentDebug);

	ImGui::End();
}


void renderToolbar(shared_ptr<core::Building> building)
{
	if (ImGui::Button(gUISettings.worldPaused ? "Resume" : "Pause"))
	{
		setWorldPaused(building, !gUISettings.worldPaused);
	}

		ImGui::SameLine();

		if (ImGui::Button("Wake Agents"))
		{
			building->wakeAllAgents();
		}


		// Select visible Layer
		vector<string> layers = {
			"Fore Layer",
			"Back Layer"
		};

		string layersStr;

		for (auto const& layer : layers)
		{
			layersStr += layer;
			layersStr += '\0';
		}

		ImGui::SetNextItemWidth(128);
		ImGui::Combo("Visible Layer", &gUISettings.visibleLayer, layersStr.c_str(), 6);

		// Selection mode

		vector<string> selectionModes = {
			"Objects",
			"Vertices",
			"Sectors"
		};

		string modesStr;

		for (auto const& mode: selectionModes)
		{
			modesStr += mode;
			modesStr += '\0';
		}

		ImGui::SetNextItemWidth(128);
		
		int selectedMode = (int)gUISettings.selectionMode;
		if (ImGui::Combo("Selection", &selectedMode, modesStr.c_str(), 6))
		{
			setSelectionMode((UISettings::SelectionMode)selectedMode);
		}

}


void renderStatusBar()
{
	ImGuiViewportP* viewport = (ImGuiViewportP*)(void*)ImGui::GetMainViewport();

	auto windowFlags =
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_MenuBar;

	float height = ImGui::GetFrameHeight();

	if (ImGui::BeginViewportSideBar("##MainStatusBar", viewport, ImGuiDir_Down, height, windowFlags))
	{
		if (ImGui::BeginMenuBar())
		{
			ImGui::Text("Layer: %s", gUISettings.visibleLayer == CORE_LAYER_FORE ? "Fore" : "Back");

			ImGui::SetNextItemWidth(128);

			auto mousePos = getMouseWorldPosition();
			string mouseData = format("{:.1f}, {:.1f}", mousePos.x, mousePos.y);

			ImGui::TextUnformatted(mouseData.c_str());

			if (gHoveredAgent)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(128);

				string objectData = format("{}", gHoveredAgent->getDescription());

				ImGui::TextUnformatted(objectData.c_str());
			}
			else if (gHoveredSectorObject)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(128);
				ImGui::Text("%s", gHoveredSectorObject->getDescription().c_str());
			}
			else if (gHoveredVertex)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(128);

				string objectData = format("{}", gHoveredVertex->getDescription());

				ImGui::TextUnformatted(objectData.c_str());
			}

			if (!gClipboardError.empty() && ImGui::GetTime() < gClipboardErrorUntil)
			{
				auto const& style = ImGui::GetStyle();
				auto width = ImGui::CalcTextSize(gClipboardError.c_str()).x;
				ImGui::SameLine(max(ImGui::GetCursorPosX() + style.ItemSpacing.x,
					ImGui::GetWindowWidth() - width - style.WindowPadding.x));
				ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "%s",
					gClipboardError.c_str());
			}

			ImGui::EndMenuBar();
		}

		ImGui::End();
	}
}


void renderMarkerPanel(shared_ptr<const core::SectorObject> object)
{
	auto marker = static_pointer_cast<const core::MarkerSectorObject>(object)->getMarker();
	auto position = marker->getPosition();
	position.x += marker->getOffset();
	ImGui::Text("Marker");
	ImGui::Text("Position: %.2f, %.2f", position.x, position.y);
}


void renderWalkwayPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	auto walkway = static_pointer_cast<const core::WalkwaySectorObject>(object)->getWalkway();
	auto room = object->getSector();
	ImGui::TextUnformatted("Walkway");
	ImGui::Text("Room: %s", room->getName().c_str());
	ImGui::Text("Room index: %u", room->getIndex());
	ImGui::Text("Layer: %s", room->getLayerIndex() == CORE_LAYER_FORE ? "Fore" : "Back");
	ImGui::Text("Position: %u, %u", walkway->getCellX(), walkway->getCellY());
	ImGui::Text("Deck offset: %u", walkway->getCellY() - room->getCellY());
	ImGui::Separator();
	if (ImGui::Button("Delete Walkway"))
	{
		try
		{
			auto undo = captureDocumentSnapshot(building);
			if (removeClipboardSelection(building)) commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Walkway editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Walkway editor", 0, core::LogLevel::Error, error.what());
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Delete key");
}


void renderWindowPanel(shared_ptr<const core::SectorObject> object)
{
	auto window = static_pointer_cast<const core::WindowSectorObject>(object)->getWindow();
	auto position = window->getPosition();
	char const* style = "Clear";
	switch (window->getStyle())
	{
	case core::Window::Style::Tinted: style = "Tinted"; break;
	case core::Window::Style::Frosted: style = "Frosted"; break;
	case core::Window::Style::Clear: break;
	}
	char const* state = "Unknown";
	switch (window->getState())
	{
	case core::Window::State::Open: state = "Open"; break;
	case core::Window::State::Opening: state = "Opening"; break;
	case core::Window::State::Closed: state = "Closed"; break;
	case core::Window::State::Closing: state = "Closing"; break;
	case core::Window::State::Broken: state = "Broken"; break;
	case core::Window::State::Frosted: state = "Frosted"; break;
	case core::Window::State::Frosting: state = "Frosting"; break;
	case core::Window::State::Unfrosting: state = "Unfrosting"; break;
	case core::Window::State::Tinted: state = "Tinted"; break;
	case core::Window::State::Tinting: state = "Tinting"; break;
	case core::Window::State::Untinting: state = "Untinting"; break;
	}

	ImGui::TextUnformatted("Window");
	ImGui::Text("Position: %.2f, %.2f", position.x, position.y);
	ImGui::Text("Size: %u x %u cell%s", window->getCellsWide(), window->getDecksHigh(),
		window->getCellsWide() == 1 && window->getDecksHigh() == 1 ? "" : "s");
	ImGui::Text("Style: %s", style);
	ImGui::Text("State: %s", state);
	ImGui::Text("Traversable: %s", window->isTraversalConfigured() ? "Yes" : "No");
	auto owner = object->getSector();
	ImGui::Text("Layer: %s", owner->getLayerIndex() == CORE_LAYER_FORE ? "Fore" : "Back");
	ImGui::Text("Sector: %s", owner->getDescription().c_str());
	for (uint32_t layer = 0; layer < CORE_NUM_LAYERS; ++layer)
		if (auto sector = window->getSector(layer); sector && sector != owner)
			ImGui::Text("Connected sector: %s", sector->getDescription().c_str());
}


void renderBulkheadDoorPanel(shared_ptr<const core::SectorObject> object)
{
	auto doorObject = static_pointer_cast<const core::BulkheadDoorSectorObject>(object);
	auto door = doorObject->getDoor();

	float pct = door->getOpenPercentage() * 100;

	// State
	switch (door->getState())
	{
	case core::OpenableObject::State::Open:
		ImGui::Text("Open (%3.2f %% open)", pct);
		break;

	case core::OpenableObject::State::Opening:
		ImGui::Text("Opening (%3.2f %% open)", pct);
		break;

	case core::OpenableObject::State::Closed:
		ImGui::Text("Closed (%3.2f %% open)", pct);
		break;

	case core::OpenableObject::State::Closing:
		ImGui::Text("Closing (%3.2f %% open)", pct);
		break;
	}

	// Open/close time
	ImGui::Text("Open/close time: %3.2f s", door->getOpenCloseTime());

	// Sectors
	ImGui::Text("From: %s", door->getSideSector(CORE_SIDE_LEFT)->getDescription().c_str());
	ImGui::Text("To: %s", door->getSideSector(CORE_SIDE_RIGHT)->getDescription().c_str());
}


void renderDoorPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	auto doorObject = static_pointer_cast<const core::DoorSectorObject>(object);
	auto door = doorObject->getDoor();
	auto position = door->getPosition();

	ImGui::TextUnformatted("Door");
	ImGui::Text("Position: %.2f, %.2f", position.x, position.y);
	ImGui::Text("Width: %u cell%s", door->getCellsWide(),
		door->getCellsWide() == 1 ? "" : "s");

	float pct = door->getOpenPercentage() * 100;

	// State
	switch (door->getState())
	{
	case core::OpenableObject::State::Open:
		ImGui::Text("Open (%3.2f%% open)", pct);
		break;

	case core::OpenableObject::State::Opening:
		ImGui::Text("Opening (%3.2f%% open)", pct);
		break;

	case core::OpenableObject::State::Closed:
		ImGui::Text("Closed (%3.2f%% open)", pct);
		break;

	case core::OpenableObject::State::Closing:
		ImGui::Text("Closing (%3.2f%% open)", pct);
		break;
	}

	// Open/close time
	ImGui::Text("Open/close time: %3.2fs", door->getOpenCloseTime());
	ImGui::Text("Open wait time: %3.2fs", door->getOpenWaitTime());
	
	// Sectors
	ImGui::Text("From: %s", door->getSector(CORE_LAYER_FORE)->getDescription().c_str());
	ImGui::Text("To: %s", door->getSector(CORE_LAYER_BACK)->getDescription().c_str());

	uint32_t liftSector, stopIndex, carriageIndex;
	bool const liftOwned = building->isLiftOwnedDoor(object, &liftSector, &stopIndex);
	bool const shuttleOwned = building->isShuttleOwnedDoor(object, &liftSector, &stopIndex, &carriageIndex);
	if (liftOwned || shuttleOwned)
	{
		ImGui::Separator();
		ImGui::Text("Owned by %s", liftOwned ? "Lift" : "Shuttle");
		ImGui::Text("%s sector: %u", liftOwned ? "Lift" : "Shuttle", liftSector);
		if (liftOwned) ImGui::Text("Stop: %u (floor %u)", stopIndex, object->getCellY());
		else ImGui::Text("Stop: %u, carriage: %u", stopIndex, carriageIndex);
		ImGui::TextDisabled("Landing geometry and controls are managed by the transport.");
	}

	ImGui::BeginDisabled(!building->isSimulationPaused() || liftOwned || shuttleOwned);
	if (ImGui::Button("Add Door Button"))
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			gUISettings.worldPaused = true;
			auto owner = object->getSector();
			uint32_t objectIndex{ ~0u };
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			{
				if (owner->getObject(i) == object)
				{
					objectIndex = i;
					break;
				}
			}
			if (objectIndex == ~0u)
				throw runtime_error("The selected Door no longer exists");
			building->addSectorDoorButton(owner->getIndex(), objectIndex);
			building->finishBuild();
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Door editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Door editor", 0, core::LogLevel::Error, error.what());
		}
	}
	ImGui::EndDisabled();
}


void renderLiftOwnedControlPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	uint32_t transportSector, stopIndex;
	bool const liftOwned = building->isLiftOwnedControl(object, &transportSector, &stopIndex);
	bool const shuttleOwned = building->isShuttleOwnedControl(object, &transportSector, &stopIndex);
	if (!liftOwned && !shuttleOwned) return;
	ImGui::Text("%s call button", liftOwned ? "Lift" : "Shuttle");
	ImGui::Text("Position: %u, %u", object->getCellX(), object->getCellY());
	ImGui::Separator();
	ImGui::Text("Owned by %s", liftOwned ? "Lift" : "Shuttle");
	ImGui::Text("%s sector: %u", liftOwned ? "Lift" : "Shuttle", transportSector);
	if (liftOwned) ImGui::Text("Stop: %u (floor %u)", stopIndex, object->getCellY());
	else ImGui::Text("Stop: %u", stopIndex);
	ImGui::TextDisabled("This button is managed by its transport landing and is read-only.");
}

void renderForceBridgePanel(shared_ptr<const core::SectorObject> object)
{
	auto fbObject = static_pointer_cast<const core::ForceBridgeSectorObject>(object);
	auto forceBridge = fbObject->getForceBridge();

	float pct = forceBridge->getExtendedPercentage() * 100;

	// State
	switch (forceBridge->getState())
	{
	case core::ExtensibleObject::State::Extended:
		ImGui::Text("Extended (%3.2f %% extended)", pct);
		break;

	case core::ExtensibleObject::State::Extending:
		ImGui::Text("Extending (%3.2f %% extended)", pct);
		break;

	case core::ExtensibleObject::State::Retracted:
		ImGui::Text("Retracted (%3.2f %% extended)", pct);
		break;

	case core::ExtensibleObject::State::Retracting:
		ImGui::Text("Retracting (%3.2f %% extended)", pct);
		break;
	}

	// Open/close time
	ImGui::Text("Extend/retract time: %3.2f s", forceBridge->getExtendRetractTime());

	// Sectors
	ImGui::Text("From: %s", forceBridge->getFromSide() == CORE_SIDE_LEFT ? "left" : "right");
}


void renderLadderPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	auto ladderObject = static_pointer_cast<const core::LadderSectorObject>(object);
	auto ladder = ladderObject->getLadder();
	auto room = object->getSector();
	uint32_t objectIndex = ~0u;
	for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		if (room->getObject(i) == object) { objectIndex = i; break; }

	ImGui::TextUnformatted("Room Ladder");
	ImGui::Text("Room: %s", room->getName().c_str());
	ImGui::Text("Layer: %s", room->getLayerIndex() == CORE_LAYER_FORE ? "Fore" : "Back");
	ImGui::Text("Position: %u, %u", object->getCellX(), object->getCellY());
	ImGui::Text("Calculated height: %u decks", ladder->getDecksHigh());
	float pct = ladder->getExtendedPercentage() * 100.0f;
	char const* state = "Extended";
	switch (ladder->getState())
	{
	case core::ExtensibleObject::State::Extending: state = "Extending"; break;
	case core::ExtensibleObject::State::Retracted: state = "Retracted"; break;
	case core::ExtensibleObject::State::Retracting: state = "Retracting"; break;
	case core::ExtensibleObject::State::Extended: break;
	}
	ImGui::Text("State: %s (%3.2f%% extended)", state, pct);
	ImGui::Text("Extend/retract time: %3.2fs", ladder->getExtendRetractTime());

	static core::Building const* editedBuilding = nullptr;
	static core::SectorObject const* editedObject = nullptr;
	static bool extensible = false, initiallyExtended = true;
	static float agentSpacing = CORE_AGENT_MAX_HEIGHT;
	static int directionalBatchLimit = 4;
	core::Building::CreateLadderOptions current{};
	if ((editedBuilding != building.get() || editedObject != object.get())
		&& objectIndex != ~0u && building->getRoomLadderOptions(room->getIndex(), objectIndex, current))
	{
		editedBuilding = building.get(); editedObject = object.get();
		extensible = current.extensible;
		initiallyExtended = current.extensible ? current.startExtended : true;
		agentSpacing = current.agentSpacing;
		directionalBatchLimit = (int)current.directionalBatchLimit;
	}
	auto commitSettings = [&](bool desiredExtensible, bool desiredInitiallyExtended,
		float desiredSpacing, int desiredBatchLimit)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			gSelectedSectorObject = building->applyRoomLadderOptions(room->getIndex(), objectIndex,
				{ ladder->getDecksHigh(), desiredExtensible,
					desiredExtensible ? desiredInitiallyExtended : true,
					desiredSpacing, (uint32_t)desiredBatchLimit });
			editedObject = gSelectedSectorObject.get();
			commitDocumentEdit(std::move(undo));
			return true;
		}
		catch (core::Exception const& error)
		{ core::addLogMessage("Room Ladder editor", 0, core::LogLevel::Error, error.getMessage()); }
		catch (std::exception const& error)
		{ core::addLogMessage("Room Ladder editor", 0, core::LogLevel::Error, error.what()); }
		return false;
	};

	ImGui::Separator();
	bool previousExtensible = extensible;
	if (ImGui::Checkbox("Extensible", &extensible))
	{
		if (!extensible) initiallyExtended = true;
		if (commitSettings(extensible, initiallyExtended, agentSpacing, directionalBatchLimit)) return;
		extensible = previousExtensible;
	}
	ImGui::BeginDisabled(!extensible);
	bool previousInitiallyExtended = initiallyExtended;
	if (ImGui::Checkbox("Initially extended", &initiallyExtended))
	{
		if (commitSettings(extensible, initiallyExtended, agentSpacing, directionalBatchLimit)) return;
		initiallyExtended = previousInitiallyExtended;
	}
	ImGui::EndDisabled();
	if (!extensible) initiallyExtended = true;
	ImGui::InputFloat("Agent spacing", &agentSpacing, 0.05f, 0.25f, "%.2f");
	ImGui::InputInt("Directional batch limit", &directionalBatchLimit);
	ImGui::BeginDisabled(objectIndex == ~0u || agentSpacing <= 0.0f || directionalBatchLimit <= 0);
	if (ImGui::Button("Apply Ladder settings"))
	{
		if (commitSettings(extensible, initiallyExtended, agentSpacing, directionalBatchLimit)) return;
	}
	ImGui::EndDisabled();
	ImGui::Separator();
	if (ImGui::Button("Delete Room Ladder"))
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			if (building->removeRoomLadder(room->getIndex(), objectIndex))
			{
				gSelectedSectorObject.reset(); gHoveredSectorObject.reset();
				commitDocumentEdit(std::move(undo));
			}
		}
		catch (core::Exception const& error)
		{ core::addLogMessage("Room Ladder editor", 0, core::LogLevel::Error, error.getMessage()); }
		catch (std::exception const& error)
		{ core::addLogMessage("Room Ladder editor", 0, core::LogLevel::Error, error.what()); }
	}
	ImGui::SameLine(); ImGui::TextDisabled("Delete key");
}


void renderLiftPanel(shared_ptr<const core::Building> const& building,
	shared_ptr<const core::Lift> lift, bool includeAgentDebug = false)
{
	ImGuiTableFlags flags =
		ImGuiTableFlags_SizingStretchSame |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_ContextMenuInBody;

	// Internals
	auto internals = lift->getInternalsStrings();

	if (ImGui::BeginTable("Stops", 2, flags))
	{
		ImGui::TableSetupColumn("Key");
		ImGui::TableSetupColumn("Value");
		ImGui::TableHeadersRow();

		for (auto const& kvp : internals)
		{
			auto const& [key, value] = kvp;

			ImGui::TableNextRow();

			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(key.c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(value.c_str());
		}

		ImGui::EndTable();
	}

	if (!includeAgentDebug || !building) return;
	auto resourceId = building->getTraversalResourceId(lift.get());
	auto snapshot = building->getSimulationSnapshot();
	auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
		[resourceId](auto const& candidate) { return candidate.id == resourceId; });
	if (!resourceId || resource == snapshot.traversalResources.end())
	{
		ImGui::TextDisabled("Lift traversal resource is unavailable.");
		return;
	}

	const char* phase = "Idle";
	switch (resource->liftStopPhase)
	{
	case core::LiftStopPhase::Opening: phase = "Opening"; break;
	case core::LiftStopPhase::Disembarking: phase = "Disembarking"; break;
	case core::LiftStopPhase::Boarding: phase = "Boarding"; break;
	case core::LiftStopPhase::Closing: phase = "Closing"; break;
	case core::LiftStopPhase::Moving: phase = "Moving"; break;
	case core::LiftStopPhase::Idle: break;
	}
	ImGui::Separator();
	ImGui::Text("Resource: %llu", (unsigned long long)resourceId.value);
	ImGui::Text("Position: %.2f", resource->liftPosition);
	ImGui::Text("Current stop: %u", resource->liftCurrentStop);
	if (resource->liftTargetStop == ~0u) ImGui::TextUnformatted("Target stop: <none>");
	else ImGui::Text("Target stop: %u", resource->liftTargetStop);
	ImGui::Text("Phase: %s", phase);
	ImGui::Text("Capacity: %u (%u occupied, %u reserved)", resource->capacity,
		resource->occupantCount, resource->admissionReservationCount);

	ImGui::TextUnformatted("Agents using lift");
	if (ImGui::BeginTable("LiftAgents", 3, flags))
	{
		ImGui::TableSetupColumn("Agent");
		ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Target floor");
		ImGui::TableHeadersRow();
		for (auto const& passenger : resource->liftAgents)
		{
			auto agent = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& candidate) { return candidate.id == passenger.agent; });
			if (agent == snapshot.agents.end()) continue;
			const char* state = "Queuing at door";
			switch (passenger.state)
			{
			case core::LiftAgentState::Entering: state = "Entering"; break;
			case core::LiftAgentState::InLift: state = "In lift"; break;
			case core::LiftAgentState::Exiting: state = "Exiting"; break;
			case core::LiftAgentState::QueuingAtDoor: break;
			}
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(agent->name.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(state);
			ImGui::TableSetColumnIndex(2);
			if (passenger.targetStop == ~0u) ImGui::TextUnformatted("<unknown>");
			else ImGui::Text("%.0f", passenger.targetFloor);
		}
		ImGui::EndTable();
	}
}


void renderShuttlePanel(shared_ptr<const core::Building> const& building,
	shared_ptr<const core::Shuttle> shuttle, bool includeAgentDebug = false)
{
	ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_Resizable
		| ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV
		| ImGuiTableFlags_ContextMenuInBody;
	auto internals = shuttle->getInternalsStrings();
	internals.push_back({ "Carriages", to_string(shuttle->getNumCars()) });
	internals.push_back({ "Carriage width", to_string(shuttle->getCarWidth()) });
	core::Building::CreateShuttleOptions options{};
	if (building && building->getShuttleOptions(shuttle.get(), options))
	{
		string doorLayout;
		for (uint32_t cell = 0; cell < options.carWidth; ++cell)
			doorLayout += (options.doorMask & (1u << cell)) != 0 ? "D" : "-";
		internals.push_back({ "Carriage doors", std::move(doorLayout) });
		internals.push_back({ "Capacity per carriage", to_string(options.capacity) });
		internals.push_back({ "Minimum dwell", format("{:.2f} s", options.minimumDwellSeconds) });
		internals.push_back({ "Maximum boarding", format("{:.2f} s", options.maximumBoardingSeconds) });
		internals.push_back({ "Partial landings", options.allowPartialLandings ? "Allowed" : "Not allowed" });
	}
	if (ImGui::BeginTable("Stops", 2, flags))
	{
		ImGui::TableSetupColumn("Key"); ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
		for (auto const& [key, value] : internals)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(key.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(value.c_str());
		}
		ImGui::EndTable();
	}
	if (!includeAgentDebug || !building) return;
	auto resourceId = building->getTraversalResourceId(shuttle.get());
	auto snapshot = building->getSimulationSnapshot();
	auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
		[resourceId](auto const& value) { return value.id == resourceId; });
	if (!resourceId || resource == snapshot.traversalResources.end())
	{
		ImGui::TextDisabled("Shuttle traversal resource is unavailable.");
		return;
	}
	const char* phase = "Idle";
	switch (resource->liftStopPhase)
	{
	case core::LiftStopPhase::Opening: phase = "Opening"; break;
	case core::LiftStopPhase::Disembarking: phase = "Disembarking"; break;
	case core::LiftStopPhase::Boarding: phase = "Boarding"; break;
	case core::LiftStopPhase::Closing: phase = "Closing"; break;
	case core::LiftStopPhase::Moving: phase = "Moving"; break;
	case core::LiftStopPhase::Idle: break;
	}
	ImGui::Separator();
	ImGui::Text("Resource: %llu", (unsigned long long)resourceId.value);
	ImGui::Text("Position: %.2f", resource->liftPosition);
	ImGui::Text("Current stop: %u", resource->liftCurrentStop);
	if (resource->liftTargetStop == ~0u) ImGui::TextUnformatted("Target stop: <none>");
	else ImGui::Text("Target stop: %u", resource->liftTargetStop);
	ImGui::Text("Phase: %s", phase);
	ImGui::Text("Capacity: %u total, %u per carriage (%u occupied, %u reserved)",
		resource->capacity, resource->shuttleCapacityPerCarriage,
		resource->occupantCount, resource->admissionReservationCount);
	if (ImGui::BeginTable("ShuttleCarriages", 3, flags))
	{
		ImGui::TableSetupColumn("Carriage"); ImGui::TableSetupColumn("Occupied");
		ImGui::TableSetupColumn("Reserved"); ImGui::TableHeadersRow();
		for (auto const& carriage : resource->shuttleCarriages)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::Text("%u", carriage.index);
			ImGui::TableSetColumnIndex(1); ImGui::Text("%u / %u", carriage.occupantCount, carriage.capacity);
			ImGui::TableSetColumnIndex(2); ImGui::Text("%u", carriage.admissionReservationCount);
		}
		ImGui::EndTable();
	}
	ImGui::TextUnformatted("Passengers using shuttle");
	if (ImGui::BeginTable("ShuttlePassengers", 4, flags))
	{
		ImGui::TableSetupColumn("Agent"); ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Target stop"); ImGui::TableSetupColumn("Carriage");
		ImGui::TableHeadersRow();
		for (auto const& passenger : resource->liftAgents)
		{
			auto agent = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == passenger.agent; });
			if (agent == snapshot.agents.end()) continue;
			const char* state = "Queuing at platform";
			switch (passenger.state)
			{
			case core::LiftAgentState::Entering: state = "Entering"; break;
			case core::LiftAgentState::InLift: state = "In shuttle"; break;
			case core::LiftAgentState::Exiting: state = "Exiting"; break;
			case core::LiftAgentState::QueuingAtDoor: break;
			}
			auto request = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == passenger.agent && value.shuttleCarriage != ~0u; });
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(agent->name.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(state);
			ImGui::TableSetColumnIndex(2);
			if (passenger.targetStop == ~0u) ImGui::TextUnformatted("<unknown>"); else ImGui::Text("%u", passenger.targetStop);
			ImGui::TableSetColumnIndex(3);
			if (request == snapshot.traversalRequests.end()) ImGui::TextUnformatted("<unassigned>");
			else ImGui::Text("%u", request->shuttleCarriage);
		}
		ImGui::EndTable();
	}
	if (ImGui::BeginTable("ShuttleAccessZones", 4, flags))
	{
		ImGui::TableSetupColumn("Stop"); ImGui::TableSetupColumn("Zone");
		ImGui::TableSetupColumn("Direction"); ImGui::TableSetupColumn("Queued"); ImGui::TableHeadersRow();
		for (auto const& zone : resource->shuttleAccessZones)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::Text("%u", zone.stopIndex);
			ImGui::TableSetColumnIndex(1); ImGui::Text("%u", zone.accessZoneIndex);
			ImGui::TableSetColumnIndex(2);
			const char* direction = zone.direction == core::TraversalDirection::Ascending ? "Right"
				: zone.direction == core::TraversalDirection::Descending ? "Left" : "None";
			ImGui::TextUnformatted(direction);
			ImGui::TableSetColumnIndex(3); ImGui::Text("%u", (uint32_t)zone.queue.size());
		}
		ImGui::EndTable();
	}
}


void renderAgentView(shared_ptr<const core::Building> building)
{
	ImGuiTableFlags flags =
		ImGuiTableFlags_SizingStretchSame |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_ContextMenuInBody;

	if (ImGui::BeginTable("Agents", 4, flags))
	{
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Sector");
		ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Path");
		ImGui::TableHeadersRow();

		core::Agent* newSelectedAgent{ gSelectedAgent };

		for (int l = 0; l < CORE_NUM_LAYERS; ++l)
		{
			auto sectors = building->getSectors(l);

			for (auto sector : sectors)
			{
				auto const& agents = sector->getAgents();

				for (auto agent : agents)
				{
					bool agentIsSelected = agent == gSelectedAgent;

					ImGui::TableNextRow();
					ImGui::PushID((void const*)agent);

					auto cc = agentIsSelected ? ImGui::GetColorU32(ImVec4(1.0f, 0.5f, 0.0f, 0.75f)) 
						: ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0 + 0, cc);

					// Description
					ImGui::TableSetColumnIndex(0);

					ImGuiSelectableFlags selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
					if (ImGui::Selectable("##agentSelector", agentIsSelected, selectableFlags, ImVec2(0, 0)))
					{
						newSelectedAgent = agent;
					}

					ImGui::SameLine();
					ImGui::Text("%s", agent->getName().c_str());

					// Sector
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(sector->getDescription().c_str());

					// State
					ImGui::TableSetColumnIndex(2);

					switch (agent->getState())
					{
					case core::Agent::State::Idle:
						ImGui::Text("Idle");
						break;

					case core::Agent::State::MovingToVertex:
						ImGui::Text("Moving to Vertex");
						break;

					case core::Agent::State::WaitingForTraversal:
						ImGui::Text("Waiting for traversal");
						break;

					case core::Agent::State::TraversingEdge:
						ImGui::Text("Traversing edge");
						break;

					case core::Agent::State::AwaitingTraversalCommit:
						ImGui::Text("Awaiting traversal commit");
						break;

					default:
						ImGui::Text("???");
						break;
					}

					// Path
					ImGui::TableSetColumnIndex(3);
					
					auto const& path = agent->getPath();
					
					if (path)
					{
						ImGui::Text("%u/%zu vertices", agent->getPathTargetNodeIndex(), path->nodes.size());
					}
					else
					{
						ImGui::TextUnformatted("");
					}

					ImGui::PopID();
				}
			}
		}

		if (newSelectedAgent != gSelectedAgent)
		{
			gSelectedSector.reset();
			gSelectedSectorObject.reset();
		}
		gSelectedAgent = newSelectedAgent;
	
		ImGui::EndTable();
	}
}


void renderObjectView(shared_ptr<const core::Building> building)
{
	string layerNames[2] = { "Fore Layer", "Back Layer" };
	
	static void* selectedNode{ nullptr };

	static ImGuiTreeNodeFlags nodeFlags =
		ImGuiTreeNodeFlags_OpenOnArrow |
		ImGuiTreeNodeFlags_OpenOnDoubleClick |
		ImGuiTreeNodeFlags_SpanAvailWidth;

	for (int l = 0; l < CORE_NUM_LAYERS; ++l)
	{
		auto sectors = building->getSectors(l);

		if (ImGui::TreeNode(layerNames[l].c_str()))
		{
			for (auto sector : sectors)
			{
				auto thisNodeFlags = nodeFlags;

				if (((void*)sector.get()) == selectedNode)
				{
					thisNodeFlags |= ImGuiTreeNodeFlags_Selected;
				}

				auto text = sector->getDescription();
				auto numObjects = sector->getNumObjects();

				if (numObjects == 0)
				{
					thisNodeFlags |= (ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
				}

				bool nodeOpen = ImGui::TreeNodeEx((void*)(intptr_t)sector.get(), thisNodeFlags, "%s", text.c_str());

				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				{
					selectedNode = (void*)sector.get();
					auto sectorSelection = sector->getType() == core::SectorType::Location
						|| sector->getType() == core::SectorType::Lift
						|| sector->getType() == core::SectorType::Shuttle
						|| sector->getType() == core::SectorType::Ladder
						|| sector->getType() == core::SectorType::Staircase;
					setSelectionMode(sectorSelection
						? UISettings::SelectionMode::Sector : UISettings::SelectionMode::Object);
					gSelectedAgent = nullptr;
					gSelectedSector = sector;
					gSelectedSectorObject = nullptr;
				}

				if (nodeOpen)
				{
					for (uint32_t i = 0; i < numObjects; ++i)
					{
						auto object = sector->getObject(i);
						if (!object) continue;

						thisNodeFlags = nodeFlags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

						if (((void*)object.get()) == selectedNode)
						{
							thisNodeFlags |= ImGuiTreeNodeFlags_Selected;
						}

						auto objectText = object->getDescription();
						ImGui::TreeNodeEx((void*)(intptr_t)object.get(), thisNodeFlags, "%s", objectText.c_str());

						if (ImGui::IsItemClicked())
						{
							selectedNode = (void*)object.get();
							setSelectionMode(UISettings::SelectionMode::Object);
							gSelectedAgent = nullptr;
							gSelectedSector = nullptr;
							gSelectedSectorObject = object;
						}
					}

					if (numObjects != 0)
					{
						ImGui::TreePop();
					}
				}
			}

			ImGui::TreePop();
		}
	}

}


void renderSelectedObjectPanel(shared_ptr<core::Building> const& building)
{
	if ((!gSelectedSector && !gSelectedSectorObject)
		|| !ImGui::CollapsingHeader("Selection", nullptr, 0)) return;

	if (gSelectedSector)
	{
		const char* type = "Sector";
		switch (gSelectedSector->getType())
		{
		case core::SectorType::Location:
			type = gSelectedSector->getTopDeckHeight() == CORE_CORRIDOR_HEIGHT
				? "Corridor" : "Room";
			break;
		case core::SectorType::Lift: type = "Lift"; break;
		case core::SectorType::Shuttle: type = "Shuttle"; break;
		case core::SectorType::Ladder: type = "Ladder"; break;
		case core::SectorType::Staircase: type = "Staircase"; break;
		default: break;
		}
		ImGui::Text("%s: %s", type, gSelectedSector->getName().c_str());
		ImGui::Text("Sector index: %u", gSelectedSector->getIndex());
		ImGui::Text("Layer: %s", gSelectedSector->getLayerIndex() == CORE_LAYER_FORE ? "Fore" : "Back");
		ImGui::Text("Position: %u, %u", gSelectedSector->getCellX(), gSelectedSector->getCellY());
		ImGui::Text("Size: %u x %u cells", gSelectedSector->getCellsWide(),
			gSelectedSector->getDecksHigh());
		ImGui::Text("Agents: %u", (uint32_t)gSelectedSector->getAgents().size());

		switch (gSelectedSector->getType())
		{
		case core::SectorType::Lift:
			renderLiftPanel(building,
				static_pointer_cast<const core::LiftTransit>(gSelectedSector)->getLift(), true);
			break;

		case core::SectorType::Shuttle:
			renderShuttlePanel(building,
				static_pointer_cast<const core::ShuttleTransit>(gSelectedSector)->getShuttle(), true);
			ImGui::Separator();
			if (ImGui::Button("Delete Shuttle"))
			{
				auto plan = building->planRemoveShuttle(gSelectedSector->getIndex());
				if (!plan.valid)
					core::addLogMessage("Shuttle editor", 0, core::LogLevel::Error, plan.diagnostic);
				else queueShuttleEdit(building, plan);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("Delete key");
			break;

		case core::SectorType::Ladder:
		{
			static core::Building const* editedBuilding = nullptr;
			static uint32_t editedSector = ~0u;
			static bool extensible = false;
			static bool initiallyExtended = true;
			static float agentSpacing = CORE_AGENT_MAX_HEIGHT;
			static int directionalBatchLimit = 4;
			core::Building::CreateLadderOptions current{ 0, false, true };
			if ((editedBuilding != building.get() || editedSector != gSelectedSector->getIndex())
				&& building->getLadderOptions(gSelectedSector->getIndex(), current))
			{
				editedBuilding = building.get();
				editedSector = gSelectedSector->getIndex();
				extensible = current.extensible;
				initiallyExtended = current.extensible ? current.startExtended : true;
				agentSpacing = current.agentSpacing;
				directionalBatchLimit = (int)current.directionalBatchLimit;
			}
			auto applyToggleImmediately = [&]()
			{
				core::Building::CreateLadderOptions options{};
				if (!building->getLadderOptions(gSelectedSector->getIndex(), options))
				{
					core::addLogMessage("Ladder editor", 0, core::LogLevel::Error,
						"The selected Ladder no longer has an authored definition");
					return false;
				}
				options.extensible = extensible;
				options.startExtended = extensible ? initiallyExtended : true;
				auto plan = building->planResizeLadder(gSelectedSector->getIndex(),
					gSelectedSector->getCellX(), gSelectedSector->getCellY(), options);
				if (!plan.valid)
				{
					core::addLogMessage("Ladder editor", 0, core::LogLevel::Error,
						plan.diagnostic);
					return false;
				}
				queueLadderEdit(building, plan);
				return true;
			};
			bool previousExtensible = extensible;
			bool previousInitiallyExtended = initiallyExtended;
			if (ImGui::Checkbox("Extensible", &extensible))
			{
				if (!extensible) initiallyExtended = true;
				if (applyToggleImmediately()) return;
				extensible = previousExtensible;
				initiallyExtended = previousInitiallyExtended;
			}
			ImGui::BeginDisabled(!extensible);
			previousInitiallyExtended = initiallyExtended;
			if (ImGui::Checkbox("Initially extended", &initiallyExtended))
			{
				if (applyToggleImmediately()) return;
				initiallyExtended = previousInitiallyExtended;
			}
			ImGui::EndDisabled();
			ImGui::InputFloat("Agent spacing", &agentSpacing, 0.05f, 0.25f, "%.2f");
			ImGui::InputInt("Directional batch limit", &directionalBatchLimit);
			bool valuesValid = agentSpacing > 0.0f && directionalBatchLimit > 0;
			ImGui::BeginDisabled(!valuesValid);
			if (ImGui::Button("Apply Ladder settings"))
			{
				core::Building::CreateLadderOptions options{
					gSelectedSector->getDecksHigh(), extensible,
					extensible ? initiallyExtended : true, agentSpacing,
					(uint32_t)directionalBatchLimit };
				auto plan = building->planResizeLadder(gSelectedSector->getIndex(),
					gSelectedSector->getCellX(), gSelectedSector->getCellY(), options);
				if (!plan.valid)
					core::addLogMessage("Ladder editor", 0, core::LogLevel::Error, plan.diagnostic);
				else queueLadderEdit(building, plan);
			}
			ImGui::EndDisabled();
			ImGui::Separator();
			if (ImGui::Button("Delete Ladder"))
			{
				auto plan = building->planRemoveLadder(gSelectedSector->getIndex());
				if (!plan.valid)
					core::addLogMessage("Ladder editor", 0, core::LogLevel::Error, plan.diagnostic);
				else queueLadderEdit(building, plan);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("Delete key");
			break;
		}

		case core::SectorType::Staircase:
		{
			static core::Building const* editedBuilding = nullptr;
			static uint32_t editedSector = ~0u;
			static int mountSide = CORE_SIDE_LEFT;
			static int directionalCapacity = 0;
			static int directionalBatchLimit = 4;
			core::Building::CreateStaircaseOptions current{ 0, CORE_SIDE_LEFT };
			if ((editedBuilding != building.get() || editedSector != gSelectedSector->getIndex())
				&& building->getStaircaseOptions(gSelectedSector->getIndex(), current))
			{
				editedBuilding = building.get();
				editedSector = gSelectedSector->getIndex();
				mountSide = current.mountSide;
				directionalCapacity = (int)current.directionalCapacity;
				directionalBatchLimit = (int)current.directionalBatchLimit;
			}
			char const* sides = "Left\0Right\0";
			int sideIndex = mountSide == CORE_SIDE_LEFT ? 0 : 1;
			if (ImGui::Combo("Mount side", &sideIndex, sides))
			{
				auto requestedSide = sideIndex == 0 ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;
				core::Building::CreateStaircaseOptions options{};
				if (!building->getStaircaseOptions(gSelectedSector->getIndex(), options))
				{
					core::addLogMessage("Staircase editor", 0, core::LogLevel::Error,
						"The selected Staircase no longer has an authored definition");
				}
				else
				{
					options.mountSide = requestedSide;
					auto plan = building->planResizeStaircase(gSelectedSector->getIndex(),
						gSelectedSector->getCellX(), gSelectedSector->getCellY(), options);
					if (!plan.valid)
						core::addLogMessage("Staircase editor", 0, core::LogLevel::Error,
							plan.diagnostic);
					else
					{
						mountSide = requestedSide;
						queueStaircaseEdit(building, plan);
					}
				}
				return;
			}
			ImGui::InputInt("Directional capacity", &directionalCapacity);
			ImGui::BeginDisabled(directionalCapacity <= 0);
			ImGui::InputInt("Directional batch limit", &directionalBatchLimit);
			ImGui::EndDisabled();
			bool valuesValid = directionalCapacity >= 0
				&& (directionalCapacity == 0 || directionalBatchLimit > 0);
			ImGui::BeginDisabled(!valuesValid);
			if (ImGui::Button("Apply capacity settings"))
			{
				core::Building::CreateStaircaseOptions options{
					gSelectedSector->getDecksHigh(), mountSide,
					(uint32_t)directionalCapacity, (uint32_t)max(1, directionalBatchLimit) };
				auto plan = building->planResizeStaircase(gSelectedSector->getIndex(),
					gSelectedSector->getCellX(), gSelectedSector->getCellY(), options);
				if (!plan.valid)
					core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, plan.diagnostic);
				else queueStaircaseEdit(building, plan);
			}
			ImGui::EndDisabled();
			ImGui::Separator();
			if (ImGui::Button("Delete Staircase"))
			{
				auto plan = building->planRemoveStaircase(gSelectedSector->getIndex());
				if (!plan.valid)
					core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, plan.diagnostic);
				else queueStaircaseEdit(building, plan);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("Delete key");
			break;
		}

		default:
			break;
		}
	}
	else
	{
		switch (gSelectedSectorObject->getObjectType())
		{
		case core::SectorObjectType::BulkheadDoor:
			renderBulkheadDoorPanel(gSelectedSectorObject);
			break;

		case core::SectorObjectType::Door:
			renderDoorPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::InteractionPoint:
			renderLiftOwnedControlPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::ForceBridge:
			renderForceBridgePanel(gSelectedSectorObject);
			break;

		case core::SectorObjectType::Ladder:
			renderLadderPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::Lift:
			renderLiftPanel(building,
				static_pointer_cast<const core::LiftSectorObject>(gSelectedSectorObject)->getLift());
			break;

		case core::SectorObjectType::Marker:
			renderMarkerPanel(gSelectedSectorObject);
			break;

		case core::SectorObjectType::Window:
			renderWindowPanel(gSelectedSectorObject);
			break;

		case core::SectorObjectType::Walkway:
			renderWalkwayPanel(building, gSelectedSectorObject);
			break;

		default:
			break;
		}
	}
}


void renderSelectedAgentPanel(shared_ptr<const core::Building> building)
{
	if (!gSelectedAgent || !ImGui::CollapsingHeader("Selection")) return;

	auto id = building->getAgentId(gSelectedAgent);
	auto sector = gSelectedAgent->getSector();
	auto localPosition = gSelectedAgent->getLocalPosition();
	auto globalPosition = gSelectedAgent->getGlobalPosition();

	ImGui::Text("Agent: %s", gSelectedAgent->getName().c_str());
	ImGui::Text("ID: %llu", (unsigned long long)id.value);
	ImGui::Text("Sector: %s", sector ? sector->getDescription().c_str() : "<none>");
	if (sector) ImGui::Text("Layer: %u", sector->getLayerIndex());
	ImGui::Text("Local position: %.2f, %.2f", localPosition.x, localPosition.y);
	ImGui::Text("World position: %.2f, %.2f", globalPosition.x, globalPosition.y);

	if (gSelectingAgentPathDestination)
	{
		ImGui::TextColored({ 1.0f, 0.75f, 0.1f, 1.0f },
			"Select a destination vertex (Escape to cancel)");
		if (ImGui::Button("Cancel path selection")) endAgentPathSelection();
	}
	else if (ImGui::Button("Select path destination (Ctrl+P)"))
	{
		beginAgentPathSelection();
	}

	const char* state = "Unknown";
	switch (gSelectedAgent->getState())
	{
	case core::Agent::State::Idle: state = "Idle"; break;
	case core::Agent::State::MovingToVertex: state = "Moving to Vertex"; break;
	case core::Agent::State::WaitingForTraversal: state = "Waiting for traversal"; break;
	case core::Agent::State::TraversingEdge: state = "Traversing edge"; break;
	case core::Agent::State::AwaitingTraversalCommit: state = "Awaiting traversal commit"; break;
	}
	ImGui::Text("State: %s", state);

	auto const& path = gSelectedAgent->getPath();
	if (path)
	{
		ImGui::Text("Path: vertex %u of %u", gSelectedAgent->getPathTargetNodeIndex(),
			(uint32_t)path->nodes.size());
		if (!path->nodes.empty() && path->nodes.back().targetVertex)
			ImGui::Text("Destination: %s", path->nodes.back().targetVertex->getDescription().c_str());
	}
	else
	{
		ImGui::Text("Path: <none>");
	}

	if (gSelectedVertex)
	{
		ImGui::Text("Selected vertex: %s", gSelectedVertex->getDescription().c_str());
		if (ImGui::Button("Path to selected vertex"))
		{
			auto newPath = building->getGraph()->calculatePath(gSelectedAgent, gSelectedVertex);
			if (newPath)
			{
				// Explicitly cancel the old route first. Agent::setPath may retain an
				// active traversal permit, but this command promises replacement.
				applyAgentPathEdit(building, gSelectedAgent, std::move(newPath), true, true);
			}
		}
	}
}


void renderBuildingPanel(shared_ptr<core::Building> building)
{
	if (ImGui::CollapsingHeader("Objects"))
	{
		renderObjectView(building);
	}

	if (ImGui::CollapsingHeader("Agents"))
	{
		renderAgentView(building);
	}

	renderSelectedObjectPanel(building);
	renderSelectedAgentPanel(building);
}


void renderGraphPanel(shared_ptr<const core::Graph> graph)
{
	if (ImGui::CollapsingHeader("Edges"))
		{
			ImGuiTableFlags flags =
				ImGuiTableFlags_SizingStretchSame |
				ImGuiTableFlags_Resizable |
				ImGuiTableFlags_BordersOuter |
				ImGuiTableFlags_BordersV |
				ImGuiTableFlags_ContextMenuInBody;

			if (ImGui::BeginTable("Edges", 5, flags))
			{
				ImGui::TableSetupColumn("Edge");
				ImGui::TableSetupColumn("Vertex 0 Desc");
				ImGui::TableSetupColumn("Vertex 0 Spec");
				ImGui::TableSetupColumn("Vertex 1 Desc");
				ImGui::TableSetupColumn("Vertex 1 Spec"); 
				ImGui::TableHeadersRow();

				for (auto edge : graph->getEdges())
				{
					ImGui::TableNextRow();

					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(edge->getDescription().c_str());

					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(edge->getVertex(0)->getDescription().c_str());

					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(edge->getVertex(0)->getSpec().c_str());

					ImGui::TableSetColumnIndex(3);
					ImGui::TextUnformatted(edge->getVertex(1)->getDescription().c_str());

					ImGui::TableSetColumnIndex(4);
					ImGui::TextUnformatted(edge->getVertex(1)->getSpec().c_str());
				}

				ImGui::EndTable();
			}
		}
}


void renderPathingPanel(shared_ptr<const core::Agent> /* agent */)
{
		string selectedVertexText = format("Selected vertex: {}", gSelectedVertex ? gSelectedVertex->getDescription() : "<none>");
		string hoveredVertexText = format("Hovered vertex: {}", gHoveredVertex ? gHoveredVertex->getDescription() : "<none>");

		ImGui::TextUnformatted(selectedVertexText.c_str());
		ImGui::TextUnformatted(hoveredVertexText.c_str());

		shared_ptr<core::Path> path = gSelectedAgent ? gSelectedAgent->getPath() : nullptr;

		if (path)
		{
			auto agentIsIdle = gSelectedAgent->getState() == core::Agent::State::Idle;

			if (!agentIsIdle)
			{
				imgui::PushDisabled();
			}

			string startButtonText = format("Start {}", gSelectedAgent->getName());

			if (ImGui::Button(startButtonText.c_str()))
			{
				gSelectedAgent->startPathing();
			}

			if (!agentIsIdle)
			{
				imgui::PopDisabled();
			}

			ImGui::SameLine();

			if (agentIsIdle)
			{
				imgui::PushDisabled();
			}

			string stopButtonText = format("Stop {}", gSelectedAgent->getName());

			if (ImGui::Button(stopButtonText.c_str()))
			{
				gSelectedAgent->pausePathing();
			}

			if (agentIsIdle)
			{
				imgui::PopDisabled();
			}

			ImGuiTableFlags flags = 
				ImGuiTableFlags_SizingStretchSame | 
				ImGuiTableFlags_Resizable | 
				ImGuiTableFlags_BordersOuter | 
				ImGuiTableFlags_BordersV | 
				ImGuiTableFlags_ContextMenuInBody;

			if (ImGui::BeginTable("Nodes", 5, flags))
			{
				ImGui::TableSetupColumn("Edge");
				ImGui::TableSetupColumn("Target vertex");
				ImGui::TableSetupColumn("Action");
				ImGui::TableSetupColumn("Edge weight");
				ImGui::TableSetupColumn("Cumulative weight");
				ImGui::TableHeadersRow();

				int curRow = 0;
				float curWeight = 0.0f;
				for (auto const& node : path->nodes)
				{
					auto const& [edge, targetVertex, cumulativeWeight] = node;

					string edgeText = edge ? edge->getDescription() : "<no edge>";
					string pathVertexText = targetVertex->getDescription();
					
					string vertexActionText = "--";

					auto nodeWeight = cumulativeWeight - curWeight;
					curWeight = cumulativeWeight;
					
					string curWeightText = to_string(nodeWeight);
					string totalWeightText = to_string(cumulativeWeight);

					ImGui::TableNextRow();

					// Edge
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(edgeText.c_str());

					// Target Vertex
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(pathVertexText.c_str());

					// Vertex Action
					ImGui::TableSetColumnIndex(2);
					ImGui::TextUnformatted(vertexActionText.c_str());

					// Edge weight
					ImGui::TableSetColumnIndex(3);
					ImGui::TextUnformatted(curWeightText.c_str());

					// Cumulative weight
					ImGui::TableSetColumnIndex(4);
					ImGui::TextUnformatted(totalWeightText.c_str());

					curRow++;
				}

				ImGui::EndTable();
			}
		}
}

void renderLogPanel()
{
	auto newMessages = core::consumeLogMessages();

	while (gLogMessages.size() + newMessages.size() > 1000)
	{
		gLogMessages.pop_front();
	}

	copy(newMessages.begin(), newMessages.end(), back_inserter(gLogMessages));

	// UI
	ImGui::AlignTextToFramePadding();
	//ImGui::Text("Log events:");
	//SameLine(); CheckboxFlags("All", &g.DebugLogFlags, ImGuiDebugLogFlags_EventMask_);
	//SameLine(); CheckboxFlags("ActiveId", &g.DebugLogFlags, ImGuiDebugLogFlags_EventActiveId);
	//SameLine(); CheckboxFlags("Focus", &g.DebugLogFlags, ImGuiDebugLogFlags_EventFocus);
	//SameLine(); CheckboxFlags("Popup", &g.DebugLogFlags, ImGuiDebugLogFlags_EventPopup);
	//SameLine(); CheckboxFlags("Nav", &g.DebugLogFlags, ImGuiDebugLogFlags_EventNav);
	//SameLine(); if (CheckboxFlags("Clipper", &g.DebugLogFlags, ImGuiDebugLogFlags_EventClipper)) { g.DebugLogClipperAutoDisableFrames = 2; } if (IsItemHovered()) SetTooltip("Clipper log auto-disabled after 2 frames");
	//SameLine(); CheckboxFlags("IO", &g.DebugLogFlags, ImGuiDebugLogFlags_EventIO);

	//if (SmallButton("Clear"))
	//{
	//	g.DebugLogBuf.clear();
	//	g.DebugLogIndex.clear();
	//}
	//SameLine();
	//if (SmallButton("Copy"))
	//	SetClipboardText(g.DebugLogBuf.c_str());
	
	ImGui::BeginChild("##log", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar);

	ImGuiTableFlags flags =
		ImGuiTableFlags_SizingStretchSame |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_ContextMenuInBody;

	if (ImGui::BeginTable("Messages", 3, flags))
	{
		ImGui::TableSetupColumn("Source Id", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Message");
		ImGui::TableHeadersRow();

		ImGuiListClipper clipper;
		clipper.Begin((int)gLogMessages.size());

		while (clipper.Step())
		{
			for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd; line_no++)
			{
				auto const& msg = gLogMessages[line_no];

				ImColor textColour(1.0f, 1.0f, 1.0f);

				switch (msg.level)
				{
				case core::LogLevel::Debug:
					textColour = ImColor(0.5f, 0.5f, 0.5f);
					break;

				case core::LogLevel::Info:
					textColour = ImColor(1.0f, 1.0f, 1.0f);
					break;

				case core::LogLevel::Warning:
					textColour = ImColor(1.0f, 0.5f, 0.0f);
					break;

				case core::LogLevel::Error:
					textColour = ImColor(1.0f, 0.0f, 0.5f);
					break;

				default:
					textColour = ImColor(1.0f, 0.0f, 1.0f);
					break;
				}
				
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);

				if (msg.sourceId == ~0u)
				{
					ImGui::TextColored(textColour, "--");
				}
				else
				{
					ImGui::TextColored(textColour, "%s", to_string(msg.sourceId).c_str());
				}

				ImGui::TableSetColumnIndex(1);
				ImGui::TextColored(textColour, "%s", msg.source.c_str());

				ImGui::TableSetColumnIndex(2);
				ImGui::TextColored(textColour, "%s", msg.msg.c_str());
			}
		}

		ImGui::EndTable();
	}

	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
	{
		ImGui::SetScrollHereY(1.0f);
	}

	ImGui::EndChild();
}


void renderDockSpace()
{
	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
		ImGuiWindowFlags_NoBackground;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("DockSpace Host", nullptr, flags);
	ImGui::PopStyleVar(3);

	ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
	if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
	{
		ImGui::DockBuilderRemoveNode(dockspaceId);
		ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

		ImGuiID leftId;
		ImGuiID worldId;
		ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.32f, &leftId, &worldId);
		ImGui::DockBuilderDockWindow("Controls", leftId);
		ImGui::DockBuilderDockWindow("World", worldId);
		ImGui::DockBuilderFinish(dockspaceId);
	}

	ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f));
	ImGui::End();
}

void renderControlsWindow(shared_ptr<core::Building> building, shared_ptr<const core::Graph> graph,
	shared_ptr<core::Agent> pathingAgent)
{
	ImGui::Begin("Controls");

	if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen))
		renderToolbar(building);
	if (ImGui::CollapsingHeader("Building", ImGuiTreeNodeFlags_DefaultOpen))
		renderBuildingPanel(building);
	if (ImGui::CollapsingHeader("Path finding"))
		renderPathingPanel(pathingAgent);
	if (ImGui::CollapsingHeader("Graph"))
		renderGraphPanel(graph);
	if (ImGui::CollapsingHeader("Log"))
		renderLogPanel();

	ImGui::End();
}

namespace
{
	bool resizeRectangleFree(shared_ptr<const core::Building> const& building,
		shared_ptr<const core::Sector> const& sector, int left, int bottom, int right, int top)
	{
		if (left < 0 || bottom < 0 || right > (int)building->getCellsWide()
			|| top > (int)building->getDecksHigh() || left >= right || bottom >= top) return false;
		auto layer = building->getLayer(sector->getLayerIndex());
		for (int y = bottom; y < top; ++y)
			for (int x = left; x < right; ++x)
			{
				auto occupant = layer->getCellDefinition(x, y).sectorIndex;
				if (occupant != ~0u && occupant != sector->getIndex()) return false;
			}
		return true;
	}

	ResizeEdge hoveredResizeEdge(shared_ptr<const core::Sector> const& sector, ImVec2 mouse)
	{
		if (!sector || (sector->getType() != core::SectorType::Location
			&& sector->getType() != core::SectorType::Lift
			&& sector->getType() != core::SectorType::Shuttle
			&& sector->getType() != core::SectorType::Ladder
			&& sector->getType() != core::SectorType::Staircase)) return ResizeEdge::None;
		auto topLeft = worldToScreen({ (float)sector->getCellX(),
			(float)(sector->getCellY() + sector->getDecksHigh()) });
		auto bottomRight = worldToScreen({ (float)(sector->getCellX() + sector->getCellsWide()),
			(float)sector->getCellY() });
		constexpr float tolerance = 6.0f;
		struct Candidate { ResizeEdge edge; float distance; };
		vector<Candidate> candidates;
		if (sector->getType() != core::SectorType::Ladder
			&& sector->getType() != core::SectorType::Staircase
			&& mouse.y >= topLeft.y - tolerance && mouse.y <= bottomRight.y + tolerance)
		{
			candidates.push_back({ ResizeEdge::Left, abs(mouse.x - topLeft.x) });
			candidates.push_back({ ResizeEdge::Right, abs(mouse.x - bottomRight.x) });
		}
		bool corridor = sector->getType() == core::SectorType::Shuttle
			|| (sector->getType() == core::SectorType::Location
				&& sector->getTopDeckHeight() == CORE_CORRIDOR_HEIGHT);
		if (!corridor && mouse.x >= topLeft.x - tolerance && mouse.x <= bottomRight.x + tolerance)
		{
			candidates.push_back({ ResizeEdge::Top, abs(mouse.y - topLeft.y) });
			candidates.push_back({ ResizeEdge::Bottom, abs(mouse.y - bottomRight.y) });
		}
		auto closest = min_element(candidates.begin(), candidates.end(),
			[](auto const& a, auto const& b) { return a.distance < b.distance; });
		if (closest != candidates.end() && closest->distance <= tolerance) return closest->edge;
		return mouse.x > topLeft.x && mouse.x < bottomRight.x
			&& mouse.y > topLeft.y && mouse.y < bottomRight.y
			? ResizeEdge::Move : ResizeEdge::None;
	}

	void updateAgentMove(shared_ptr<core::Building> const& building)
	{
		auto& io = ImGui::GetIO();
		if (gUISettings.selectionMode != UISettings::SelectionMode::Object)
		{
			resetAgentMove();
			return;
		}
		if (!gAgentMove.dragging && gWorldHovered && gHoveredAgent && io.MouseClicked[0])
		{
			gSelectedAgent = gHoveredAgent;
			gSelectedSector.reset();
			gSelectedSectorObject.reset();
			gAgentMove.dragging = true;
			gAgentMove.pressPosition = io.MousePos;
			gAgentMove.originalPosition = gSelectedAgent->getGlobalPosition();
			gAgentMove.preview = getAgentMoveTarget(building, gSelectedAgent,
				gAgentMove.originalPosition);
		}
		if (!gAgentMove.dragging) return;
		if (!gSelectedAgent)
		{
			resetAgentMove();
			return;
		}

		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || io.MouseClicked[1])
		{
			resetAgentMove();
			return;
		}
		auto target = gAgentMove.originalPosition + core::Vector2{
			(io.MousePos.x - gAgentMove.pressPosition.x) / CORE_CELL_WIDTH_PIXELS,
			-(io.MousePos.y - gAgentMove.pressPosition.y) / CORE_DECK_HEIGHT_PIXELS };
		gAgentMove.preview = getAgentMoveTarget(building, gSelectedAgent, target);

		if (!io.MouseReleased[0]) return;
		if (!gWorldHovered)
		{
			resetAgentMove();
			return;
		}
		if (!gAgentMove.preview)
		{
			core::addLogMessage("Agent editor", 0, core::LogLevel::Error,
				gAgentMove.preview.diagnostic);
			resetAgentMove();
			return;
		}
		auto destinationPosition = core::Vector2{
			gAgentMove.preview.sector->getPosition().x + gAgentMove.preview.localX,
			gAgentMove.preview.floorY };
		if (abs(destinationPosition.x - gAgentMove.originalPosition.x) <= 0.001f
			&& abs(destinationPosition.y - gAgentMove.originalPosition.y) <= 0.001f)
		{
			resetAgentMove();
			return;
		}
		try
		{
			auto undo = captureDocumentSnapshot(building);
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			gSelectedAgent->clearPath();
			auto source = const_cast<core::Sector*>(gSelectedAgent->getSector());
			auto destination = const_cast<core::Sector*>(gAgentMove.preview.sector.get());
			if (source) source->exitAgent(gSelectedAgent);
			destination->enterAgent(gSelectedAgent, gAgentMove.preview.deckOffset,
				gAgentMove.preview.localX);
			gHoveredAgent = nullptr;
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Agent editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Agent editor", 0, core::LogLevel::Error, error.what());
		}
		resetAgentMove();
	}

	void updateObjectMove(shared_ptr<core::Building> const& building)
	{
		auto& io = ImGui::GetIO();
		if (!gObjectMove.dragging && gWorldHovered
			&& gUISettings.selectionMode == UISettings::SelectionMode::Object
			&& gHoveredSectorObject && io.MouseClicked[0])
		{
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = gHoveredSectorObject;
		}
		bool objectOnVisibleLayer = gSelectedSectorObject
			&& gSelectedSectorObject->getSector()->getLayerIndex() == (uint32_t)gUISettings.visibleLayer;
		if (gSelectedSectorObject
			&& gSelectedSectorObject->getObjectType() == core::SectorObjectType::Window)
		{
			auto window = static_pointer_cast<const core::WindowSectorObject>(
				gSelectedSectorObject)->getWindow();
			objectOnVisibleLayer = objectOnVisibleLayer
				|| window->getSector(gUISettings.visibleLayer) != nullptr;
		}
		if (gUISettings.selectionMode != UISettings::SelectionMode::Object
			|| !gSelectedSectorObject || !objectOnVisibleLayer)
		{
			resetObjectMove();
			return;
		}

		if (building->isLiftOwnedDoor(gSelectedSectorObject)
			|| building->isLiftOwnedControl(gSelectedSectorObject)
			|| building->isShuttleOwnedDoor(gSelectedSectorObject)
			|| building->isShuttleOwnedControl(gSelectedSectorObject))
		{
			resetObjectMove();
			return;
		}
		auto owner = gSelectedSectorObject->getSector();
		uint32_t objectIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == gSelectedSectorObject) { objectIndex = i; break; }
		if (objectIndex == ~0u)
		{
			resetObjectMove();
			return;
		}

		if (!gObjectMove.dragging && gWorldHovered
			&& gHoveredSectorObject == gSelectedSectorObject && io.MouseClicked[0])
			beginObjectMove(building, gSelectedSectorObject, objectIndex);
		if (!gObjectMove.dragging) return;

		ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || io.MouseClicked[1])
		{
			resetObjectMove();
			return;
		}

		int deltaX = (int)round((io.MousePos.x - gObjectMove.pressPosition.x) / CORE_CELL_WIDTH_PIXELS);
		int deltaY = (int)round(-(io.MousePos.y - gObjectMove.pressPosition.y) / CORE_DECK_HEIGHT_PIXELS);
		int targetX = (int)gObjectMove.originalX + deltaX;
		int targetY = (int)gObjectMove.originalY + deltaY;
		bool const targetInWorld = targetX >= 0 && targetY >= 0
			&& targetX < (int)building->getCellsWide() && targetY < (int)building->getDecksHigh();
		if (!targetInWorld)
		{
			gObjectMove.preview.valid = false;
			gObjectMove.preview.diagnostic = "Drop the object inside the world";
		}
		else if (gObjectMove.preview.x != (uint32_t)targetX || gObjectMove.preview.y != (uint32_t)targetY
			|| gObjectMove.preview.diagnostic == "Drop the object inside the world")
		{
			gObjectMove.preview = building->planMoveSectorObject(owner->getIndex(), objectIndex,
				(uint32_t)targetX, (uint32_t)targetY);
		}

		if (io.MouseReleased[0])
		{
			if (!gWorldHovered)
			{
				resetObjectMove();
				return;
			}
			if (gObjectMove.preview.x == gObjectMove.originalX
				&& gObjectMove.preview.y == gObjectMove.originalY)
			{
				resetObjectMove();
				return;
			}
			if (!gObjectMove.preview.valid)
			{
				core::addLogMessage("Object editor", 0, core::LogLevel::Error,
					gObjectMove.preview.diagnostic);
				resetObjectMove();
				return;
			}
			try
			{
				auto undo = captureDocumentSnapshot(building);
				if (!building->isSimulationPaused()) building->pauseSimulation();
				gUISettings.worldPaused = true;
				gSelectedSectorObject = building->applyObjectMove(gObjectMove.preview);
				gHoveredSectorObject.reset();
				gSelectedSector.reset();
				gSelectedAgent = nullptr;
				commitDocumentEdit(std::move(undo));
			}
			catch (core::Exception const& error)
			{
				core::addLogMessage("Object editor", 0, core::LogLevel::Error, error.getMessage());
			}
			resetObjectMove();
		}
	}

	void updateSectorResize(shared_ptr<core::Building> const& building)
	{
		auto& io = ImGui::GetIO();
		if (gUISettings.selectionMode != UISettings::SelectionMode::Sector || !gSelectedSector
			|| gSelectedSector->getLayerIndex() != (uint32_t)gUISettings.visibleLayer)
		{
			if (gSectorResize.dragging) resetSectorResize();
			return;
		}

		auto hoverEdge = gSectorResize.dragging ? gSectorResize.edge
			: gHoveredSector == gSelectedSector
				? hoveredResizeEdge(gSelectedSector, io.MousePos) : ResizeEdge::None;
		if (hoverEdge == ResizeEdge::Left || hoverEdge == ResizeEdge::Right)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		else if (hoverEdge == ResizeEdge::Top || hoverEdge == ResizeEdge::Bottom)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		else if (hoverEdge == ResizeEdge::Move)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

		if (!gSectorResize.dragging && gWorldHovered && hoverEdge != ResizeEdge::None && io.MouseClicked[0])
		{
			bool const selectedLift = gSelectedSector->getType() == core::SectorType::Lift;
			bool const selectedShuttle = gSelectedSector->getType() == core::SectorType::Shuttle;
			bool const selectedLadder = gSelectedSector->getType() == core::SectorType::Ladder;
			bool const selectedStaircase = gSelectedSector->getType() == core::SectorType::Staircase;
			if (hoverEdge != ResizeEdge::Move && !selectedLift && !selectedShuttle)
			{
				if (!building->isSimulationPaused()) building->pauseSimulation();
				gUISettings.worldPaused = true;
			}
			gSectorResize.dragging = true;
			gSectorResize.lift = selectedLift;
			gSectorResize.shuttle = selectedShuttle;
			gSectorResize.ladder = selectedLadder;
			gSectorResize.staircase = selectedStaircase;
			gSectorResize.edge = hoverEdge;
			gSectorResize.pressPosition = io.MousePos;
			gSectorResize.originalX = gSelectedSector->getCellX();
			gSectorResize.originalY = gSelectedSector->getCellY();
			gSectorResize.originalWidth = gSelectedSector->getCellsWide();
			gSectorResize.originalHeight = gSelectedSector->getDecksHigh();
			if (gSectorResize.lift)
				gSectorResize.liftPreview = building->planResizeLift(gSelectedSector->getIndex(),
					gSectorResize.originalX, gSectorResize.originalY,
					gSectorResize.originalWidth, gSectorResize.originalHeight);
			else if (gSectorResize.shuttle)
				gSectorResize.shuttlePreview = building->planResizeShuttle(gSelectedSector->getIndex(),
					gSectorResize.originalX, gSectorResize.originalY, gSectorResize.originalWidth);
			else if (gSectorResize.ladder)
			{
				core::Building::CreateLadderOptions options{
					gSectorResize.originalHeight, false, true };
				building->getLadderOptions(gSelectedSector->getIndex(), options);
				gSectorResize.ladderPreview = building->planResizeLadder(
					gSelectedSector->getIndex(), gSectorResize.originalX,
					gSectorResize.originalY, options);
			}
			else if (gSectorResize.staircase)
			{
				core::Building::CreateStaircaseOptions options{
					gSectorResize.originalHeight, CORE_SIDE_LEFT };
				building->getStaircaseOptions(gSelectedSector->getIndex(), options);
				gSectorResize.staircasePreview = building->planResizeStaircase(
					gSelectedSector->getIndex(), gSectorResize.originalX,
					gSectorResize.originalY, options);
			}
			else
				gSectorResize.preview = building->planResizeLocation(gSelectedSector->getIndex(),
					gSectorResize.originalX, gSectorResize.originalY,
					gSectorResize.originalWidth, gSectorResize.originalHeight);
		}
		if (!gSectorResize.dragging) return;

		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || io.MouseClicked[1])
		{
			resetSectorResize();
			if (io.MouseClicked[1]) gSelectedSector.reset();
			return;
		}

		int left = (int)gSectorResize.originalX;
		int bottom = (int)gSectorResize.originalY;
		int right = left + (int)gSectorResize.originalWidth;
		int top = bottom + (int)gSectorResize.originalHeight;
		int deltaX = (int)round((io.MousePos.x - gSectorResize.pressPosition.x) / CORE_CELL_WIDTH_PIXELS);
		int deltaY = (int)round(-(io.MousePos.y - gSectorResize.pressPosition.y) / CORE_DECK_HEIGHT_PIXELS);
		int* moving = nullptr;
		int desired = 0;
		switch (gSectorResize.edge)
		{
		case ResizeEdge::Left:
			moving = &left; desired = clamp(left + deltaX,
				gSectorResize.lift ? max(0, right - 2) : 0, right - 1); break;
		case ResizeEdge::Right:
			moving = &right; desired = clamp(right + deltaX, left + 1,
				gSectorResize.lift ? min(left + 2, (int)building->getCellsWide())
					: (int)building->getCellsWide()); break;
		case ResizeEdge::Bottom: moving = &bottom; desired = clamp(bottom + deltaY, 0,
			 top - ((gSectorResize.ladder || gSectorResize.staircase) ? 2 : 1)); break;
		case ResizeEdge::Top: moving = &top; desired = clamp(top + deltaY,
			bottom + ((gSectorResize.ladder || gSectorResize.staircase) ? 2 : 1),
			(int)building->getDecksHigh()); break;
		case ResizeEdge::Move:
		{
			int width = right - left;
			int height = top - bottom;
			left = clamp(left + deltaX, 0, (int)building->getCellsWide() - width);
			bottom = clamp(bottom + deltaY, 0, (int)building->getDecksHigh() - height);
			right = left + width;
			top = bottom + height;
			if ((deltaX != 0 || deltaY != 0) && !gSectorResize.lift
				&& !building->isSimulationPaused())
			{
				building->pauseSimulation();
				gUISettings.worldPaused = true;
			}
			break;
		}
		case ResizeEdge::None: break;
		}
		if (moving)
		{
			int step = desired >= *moving ? 1 : -1;
			while (*moving != desired)
			{
				int old = *moving;
				*moving += step;
				if (!resizeRectangleFree(building, gSelectedSector, left, bottom, right, top))
				{
					*moving = old;
					break;
				}
			}
		}
		if (gSectorResize.lift)
		{
			if (gSectorResize.liftPreview.x != (uint32_t)left
				|| gSectorResize.liftPreview.y != (uint32_t)bottom
				|| gSectorResize.liftPreview.cellsWide != (uint32_t)(right - left)
				|| gSectorResize.liftPreview.decksHigh != (uint32_t)(top - bottom))
				gSectorResize.liftPreview = building->planResizeLift(gSelectedSector->getIndex(),
					(uint32_t)left, (uint32_t)bottom, (uint32_t)(right - left), (uint32_t)(top - bottom));
		}
		else if (gSectorResize.shuttle)
		{
			if (gSectorResize.shuttlePreview.x != (uint32_t)left
				|| gSectorResize.shuttlePreview.y != (uint32_t)bottom
				|| gSectorResize.shuttlePreview.cellsWide != (uint32_t)(right - left))
				gSectorResize.shuttlePreview = building->planResizeShuttle(gSelectedSector->getIndex(),
					(uint32_t)left, (uint32_t)bottom, (uint32_t)(right - left));
		}
		else if (gSectorResize.ladder)
		{
			if (gSectorResize.ladderPreview.x != (uint32_t)left
				|| gSectorResize.ladderPreview.y != (uint32_t)bottom
				|| gSectorResize.ladderPreview.options.decksHigh != (uint32_t)(top - bottom))
			{
				auto options = gSectorResize.ladderPreview.options;
				options.decksHigh = (uint32_t)(top - bottom);
				gSectorResize.ladderPreview = building->planResizeLadder(
					gSelectedSector->getIndex(), (uint32_t)left, (uint32_t)bottom, options);
			}
		}
		else if (gSectorResize.staircase)
		{
			if (gSectorResize.staircasePreview.x != (uint32_t)left
				|| gSectorResize.staircasePreview.y != (uint32_t)bottom
				|| gSectorResize.staircasePreview.options.decksHigh != (uint32_t)(top - bottom))
			{
				auto options = gSectorResize.staircasePreview.options;
				options.decksHigh = (uint32_t)(top - bottom);
				gSectorResize.staircasePreview = building->planResizeStaircase(
					gSelectedSector->getIndex(), (uint32_t)left, (uint32_t)bottom, options);
			}
		}
		else if (gSectorResize.preview.x != (uint32_t)left
			|| gSectorResize.preview.y != (uint32_t)bottom
			|| gSectorResize.preview.cellsWide != (uint32_t)(right - left)
			|| gSectorResize.preview.decksHigh != (uint32_t)(top - bottom))
		{
			gSectorResize.preview = building->planResizeLocation(gSelectedSector->getIndex(),
				(uint32_t)left, (uint32_t)bottom, (uint32_t)(right - left), (uint32_t)(top - bottom));
		}

		if (io.MouseReleased[0])
		{
			gSectorResize.dragging = false;
			bool unchanged = left == (int)gSectorResize.originalX
				&& bottom == (int)gSectorResize.originalY
				&& right - left == (int)gSectorResize.originalWidth
				&& top - bottom == (int)gSectorResize.originalHeight;
			if (unchanged) resetSectorResize();
			else if (gSectorResize.lift && !gSectorResize.liftPreview.valid)
			{
				core::addLogMessage("Lift editor", 0, core::LogLevel::Error,
					gSectorResize.liftPreview.diagnostic);
				resetSectorResize();
			}
			else if (gSectorResize.shuttle && !gSectorResize.shuttlePreview.valid)
			{
				core::addLogMessage("Shuttle editor", 0, core::LogLevel::Error,
					gSectorResize.shuttlePreview.diagnostic);
				resetSectorResize();
			}
			else if (gSectorResize.ladder && !gSectorResize.ladderPreview.valid)
			{
				core::addLogMessage("Ladder editor", 0, core::LogLevel::Error,
					gSectorResize.ladderPreview.diagnostic);
				resetSectorResize();
			}
			else if (gSectorResize.staircase && !gSectorResize.staircasePreview.valid)
			{
				core::addLogMessage("Staircase editor", 0, core::LogLevel::Error,
					gSectorResize.staircasePreview.diagnostic);
				resetSectorResize();
			}
			else if (!gSectorResize.lift && !gSectorResize.shuttle && !gSectorResize.ladder
				&& !gSectorResize.staircase && !gSectorResize.preview.valid)
			{
				core::addLogMessage("Sector editor", 0, core::LogLevel::Error,
					gSectorResize.preview.diagnostic);
				resetSectorResize();
			}
			else if (gSectorResize.lift) queueLiftEdit(building, gSectorResize.liftPreview);
			else if (gSectorResize.shuttle) queueShuttleEdit(building, gSectorResize.shuttlePreview);
			else if (gSectorResize.ladder) queueLadderEdit(building, gSectorResize.ladderPreview);
			else if (gSectorResize.staircase) queueStaircaseEdit(building, gSectorResize.staircasePreview);
			else queueLocationEdit(building, gSectorResize.preview);
		}
	}

	void drawSectorEditOverlay(ImDrawList* drawList)
	{
		if (gWorldHovered && gUISettings.selectionMode == UISettings::SelectionMode::Sector
			&& gSelectedSector && shouldDrawCanvasSectorEditOverlay(
				gSelectedSector->getLayerIndex(), (uint32_t)gUISettings.visibleLayer)
			&& !gSectorResize.dragging && !gPendingLocationEdit
			&& !gPendingLiftEdit && !gPendingShuttleEdit && !gPendingLadderEdit
			&& !gPendingStaircaseEdit)
		{
			auto edge = hoveredResizeEdge(gSelectedSector, ImGui::GetIO().MousePos);
			auto topLeft = worldToScreen({ (float)gSelectedSector->getCellX(),
				(float)(gSelectedSector->getCellY() + gSelectedSector->getDecksHigh()) });
			auto bottomRight = worldToScreen({
				(float)(gSelectedSector->getCellX() + gSelectedSector->getCellsWide()),
				(float)gSelectedSector->getCellY() });
			switch (edge)
			{
			case ResizeEdge::Left: drawList->AddLine(topLeft, { topLeft.x, bottomRight.y }, IM_COL32(255, 255, 0, 255), 4.0f); break;
			case ResizeEdge::Right: drawList->AddLine({ bottomRight.x, topLeft.y }, bottomRight, IM_COL32(255, 255, 0, 255), 4.0f); break;
			case ResizeEdge::Top: drawList->AddLine(topLeft, { bottomRight.x, topLeft.y }, IM_COL32(255, 255, 0, 255), 4.0f); break;
			case ResizeEdge::Bottom: drawList->AddLine({ topLeft.x, bottomRight.y }, bottomRight, IM_COL32(255, 255, 0, 255), 4.0f); break;
			case ResizeEdge::Move:
				drawList->AddRect(topLeft, bottomRight, IM_COL32(255, 255, 0, 255), 0.0f, 0, 3.0f);
				break;
			case ResizeEdge::None: break;
			}
		}

		bool hasPlan = false, valid = false, remove = false;
		uint32_t x = 0, y = 0, width = 0, height = 0;
		string diagnostic;
		if (gSectorResize.lift && (gSectorResize.dragging || gSectorResize.liftPreview.cellsWide))
		{
			auto const& plan = gSectorResize.liftPreview;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = plan.cellsWide; height = plan.decksHigh; diagnostic = plan.diagnostic;
		}
		else if (gSectorResize.shuttle && (gSectorResize.dragging || gSectorResize.shuttlePreview.cellsWide))
		{
			auto const& plan = gSectorResize.shuttlePreview;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = plan.cellsWide; height = 1; diagnostic = plan.diagnostic;
		}
		else if (gSectorResize.ladder && (gSectorResize.dragging
			|| gSectorResize.ladderPreview.decksHigh))
		{
			auto const& plan = gSectorResize.ladderPreview;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = 1; height = plan.options.decksHigh; diagnostic = plan.diagnostic;
		}
		else if (gSectorResize.staircase && (gSectorResize.dragging
			|| gSectorResize.staircasePreview.decksHigh))
		{
			auto const& plan = gSectorResize.staircasePreview;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = 2; height = plan.options.decksHigh; diagnostic = plan.diagnostic;
		}
		else if (gSectorResize.dragging || (gSectorResize.preview.cellsWide && !gPendingLocationEdit))
		{
			auto const& plan = gSectorResize.preview;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = plan.cellsWide; height = plan.decksHigh; diagnostic = plan.diagnostic;
		}
		else if (gPendingLiftEdit)
		{
			auto const& plan = *gPendingLiftEdit;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = plan.cellsWide; height = plan.decksHigh; diagnostic = plan.diagnostic;
		}
		else if (gPendingShuttleEdit)
		{
			auto const& plan = *gPendingShuttleEdit;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = plan.cellsWide; height = 1; diagnostic = plan.diagnostic;
		}
		else if (gPendingLocationEdit)
		{
			auto const& plan = *gPendingLocationEdit;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = plan.cellsWide; height = plan.decksHigh; diagnostic = plan.diagnostic;
		}
		else if (gPendingLadderEdit)
		{
			auto const& plan = *gPendingLadderEdit;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = 1; height = plan.options.decksHigh; diagnostic = plan.diagnostic;
		}
		else if (gPendingStaircaseEdit)
		{
			auto const& plan = *gPendingStaircaseEdit;
			hasPlan = true; valid = plan.valid; remove = plan.remove; x = plan.x; y = plan.y;
			width = 2; height = plan.options.decksHigh; diagnostic = plan.diagnostic;
		}
		if (gShuttleDraft)
		{
			auto const& draft = *gShuttleDraft;
			auto topLeft = worldToScreen({ (float)draft.x, (float)draft.y + 1.0f });
			auto bottomRight = worldToScreen({ (float)(draft.x + draft.cellsWide), (float)draft.y });
			auto colour = draft.diagnostic.empty() ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 64, 64, 255);
			drawList->AddRectFilled(topLeft, bottomRight,
				draft.diagnostic.empty() ? IM_COL32(255, 255, 0, 35) : IM_COL32(255, 64, 64, 35));
			drawList->AddRect(topLeft, bottomRight, colour, 0.0f, 0, 2.0f);
			for (auto offset : draft.stopOffsets)
				for (int car = 0; car < draft.numCars; ++car)
				{
					auto carX = draft.x + offset + car * (draft.carWidth + 1);
					auto carTop = worldToScreen({ (float)carX, (float)draft.y + CORE_SHUTTLE_CAR_HEIGHT });
					auto carBottom = worldToScreen({ (float)(carX + draft.carWidth), (float)draft.y });
					drawList->AddRect(carTop, carBottom, colour, 0.0f, 0, 2.0f);
					for (int doorOffset = 0; doorOffset < draft.carWidth; ++doorOffset)
					{
						if ((draft.doorMask & (1u << doorOffset)) == 0) continue;
						auto doorTop = worldToScreen({ (float)(carX + doorOffset),
							(float)draft.y + CORE_SHUTTLE_CAR_HEIGHT * 0.75f });
						auto doorBottom = worldToScreen({ (float)(carX + doorOffset + 1), (float)draft.y });
						drawList->AddRectFilled(doorTop, doorBottom, IM_COL32(80, 180, 255, 90));
					}
				}
		}
		if (!hasPlan || remove || width == 0 || height == 0) return;
		auto topLeft = worldToScreen({ (float)x, (float)(y + height) });
		auto bottomRight = worldToScreen({ (float)(x + width), (float)y });
		auto colour = valid ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 64, 64, 255);
		drawList->AddRectFilled(topLeft, bottomRight,
			valid ? IM_COL32(255, 255, 0, 45) : IM_COL32(255, 64, 64, 45));
		drawList->AddRect(topLeft, bottomRight, colour, 0.0f, 0, 2.0f);
		if (!valid && !diagnostic.empty()) ImGui::SetTooltip("%s", diagnostic.c_str());
	}
}

void renderWorldWindow(shared_ptr<core::Building> building, shared_ptr<const core::Graph> graph)
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("World", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();

	ImVec2 canvasPos = ImGui::GetCursorScreenPos();
	ImVec2 canvasSize = ImGui::GetContentRegionAvail();
	canvasSize.x = max(canvasSize.x, 1.0f);
	canvasSize.y = max(canvasSize.y, 1.0f);

	auto const& style = ImGui::GetStyle();
	float const scrollbarThickness = ImGui::GetFrameHeight();
	float const horizontalScrollbarSpace = scrollbarThickness + style.ItemSpacing.y;
	float const verticalScrollbarSpace = scrollbarThickness + style.ItemSpacing.x;
	float const worldWidth = (float)building->getCellsWide() * (float)CORE_CELL_WIDTH_PIXELS;
	float const worldHeight = (float)building->getDecksHigh() * (float)CORE_DECK_HEIGHT_PIXELS;

	bool showHorizontalScrollbar = worldWidth > canvasSize.x;
	bool showVerticalScrollbar = worldHeight > canvasSize.y;
	// One scrollbar reduces the other axis, which can make the other scrollbar necessary.
	if (showHorizontalScrollbar && worldHeight > canvasSize.y - horizontalScrollbarSpace)
		showVerticalScrollbar = true;
	if (showVerticalScrollbar && worldWidth > canvasSize.x - verticalScrollbarSpace)
		showHorizontalScrollbar = true;

	if (showHorizontalScrollbar)
		canvasSize.y = max(canvasSize.y - horizontalScrollbarSpace, 1.0f);
	if (showVerticalScrollbar)
		canvasSize.x = max(canvasSize.x - verticalScrollbarSpace, 1.0f);
	float const horizontalScrollMax = max(worldWidth - canvasSize.x, 0.0f);
	float const verticalScrollMax = max(worldHeight - canvasSize.y, 0.0f);

	gUISettings.worldViewportX = canvasPos.x;
	gUISettings.worldViewportY = canvasPos.y;
	gUISettings.worldViewportWidth = canvasSize.x;
	gUISettings.worldViewportHeight = canvasSize.y;

	ImGui::InvisibleButton("##WorldCanvas", canvasSize,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	gWorldHovered = ImGui::IsItemHovered();
	ImGui::SetItemAllowOverlap();

	static float scrollX = 0.0f;
	static float scrollY = 0.0f;
	if (showVerticalScrollbar)
	{
		scrollY = clamp(scrollY, 0.0f, verticalScrollMax);
		ImGui::SameLine();
		ImGui::VSliderFloat("##WorldVerticalScroll",
			{ scrollbarThickness, canvasSize.y }, &scrollY, 0.0f, verticalScrollMax, "",
			ImGuiSliderFlags_NoInput);
		gUISettings.yOffset = -scrollY;
	}
	else
	{
		scrollY = 0.0f;
		if (gUISettings.yOffset < 0.0f) gUISettings.yOffset = 0.0f;
	}

	if (showHorizontalScrollbar)
	{
		scrollX = clamp(scrollX, 0.0f, horizontalScrollMax);
		ImGui::SetNextItemWidth(canvasSize.x);
		ImGui::SliderFloat("##WorldHorizontalScroll", &scrollX, 0.0f,
			horizontalScrollMax, "", ImGuiSliderFlags_NoInput);
		gUISettings.xOffset = -scrollX;
	}
	else
	{
		scrollX = 0.0f;
		if (gUISettings.xOffset < 0.0f) gUISettings.xOffset = 0.0f;
	}

	gHoveredAgent = nullptr;
	gHoveredInteractionPoint = {};
	gHoveredSector.reset();
	gHoveredSectorObject = nullptr;
	gHoveredVertex = nullptr;

	if (gWorldHovered)
	{
		auto mousePos = getMouseWorldPosition();
		gLastWorldCursor = mousePos;
		// Hit-test in reverse visual order. Graph vertices are rendered over agents,
		// agents over sector objects, and sector objects over their owning sector.
		if (gUISettings.renderGraph)
			gHoveredVertex = graph->getVertexAtPosition(gUISettings.visibleLayer, mousePos.x,
				mousePos.y, RENDER_VERTEX_SIZE / (float)CORE_DECK_HEIGHT_PIXELS);
		if (!gHoveredVertex)
			gHoveredAgent = building->getAgentAtPosition(gUISettings.visibleLayer,
				mousePos.x, mousePos.y);
		if (!gHoveredVertex && !gHoveredAgent)
		{
			gHoveredSectorObject = markerAtScreenPosition(building, ImGui::GetIO().MousePos);
			if (!gHoveredSectorObject)
				gHoveredSectorObject = ladderAtScreenPosition(building, ImGui::GetIO().MousePos);
			if (!gHoveredSectorObject)
				gHoveredSectorObject = walkwayAtScreenPosition(building, ImGui::GetIO().MousePos);
			if (!gHoveredSectorObject)
			{
				shared_ptr<const core::SectorObject> sectorObject;
				auto object = building->getObjectAtPosition(gUISettings.visibleLayer,
					mousePos.x, mousePos.y, &sectorObject);
				if (sectorObject && sectorObject->getObjectType() != core::SectorObjectType::Marker)
					gHoveredSectorObject = sectorObject;
				if (auto button = dynamic_pointer_cast<const core::Button>(object))
					gHoveredInteractionPoint = button->getInteractionPointId();
			}
		}
		if (!gHoveredVertex && !gHoveredAgent && !gHoveredSectorObject)
		{
			auto sector = building->getSectorAtPosition(gUISettings.visibleLayer,
				mousePos.x, mousePos.y);
			if (sector && isCanvasSelectableSectorType(sector->getType()))
				gHoveredSector = sector;
		}

		if (gHoveredAgent || gHoveredSector || gHoveredSectorObject || gHoveredVertex)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}

	updateAgentMove(building);
	updateObjectMove(building);
	updateSectorResize(building);
	if (gAgentMove.dragging || gObjectMove.dragging) gPegmanConsumesLeftMouse = true;

	if (gSelectingAgentPathDestination) gUISettings.renderGraph = true;

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->PushClipRect(canvasPos, canvasPos + canvasSize, true);

	// Keep world geometry and editor overlays inside the dimensions declared by
	// the building. The canvas can be larger than the world when docked or resized.
	auto worldTopLeft = worldToScreen({ 0.0f, (float)building->getDecksHigh() });
	auto worldBottomRight = worldToScreen({ (float)building->getCellsWide(), 0.0f });
	drawList->PushClipRect(worldTopLeft, worldBottomRight, true);
	renderBuilding(building);
	renderGraph(graph, building);
	drawSectorEditOverlay(drawList);
	if (gAgentMove.dragging)
	{
		auto const& preview = gAgentMove.preview;
		if (preview)
		{
			auto position = core::Vector2{ preview.sector->getPosition().x + preview.localX,
				preview.floorY };
			drawPegman(drawList, worldToScreen(position),
				CORE_AGENT_MAX_WIDTH * CORE_CELL_WIDTH_PIXELS,
				CORE_AGENT_MAX_HEIGHT * CORE_DECK_HEIGHT_PIXELS,
				IM_COL32(255, 255, 0, 255));
		}
		else if (!preview.diagnostic.empty()) ImGui::SetTooltip("%s", preview.diagnostic.c_str());
	}
	if (gObjectMove.dragging)
	{
		auto const& plan = gObjectMove.preview;
		auto width = plan.previewWidth ? (float)plan.previewWidth
			: gSelectedSectorObject ? gSelectedSectorObject->getSize().x : 1.0f;
		auto height = plan.previewHeight ? (float)plan.previewHeight
			: gSelectedSectorObject ? gSelectedSectorObject->getSize().y : 1.0f;
		auto topLeft = worldToScreen({ (float)plan.x, (float)plan.y + height });
		auto bottomRight = worldToScreen({ (float)plan.x + width, (float)plan.y });
		auto colour = plan.valid ? IM_COL32(255, 255, 0, 255) : IM_COL32(255, 64, 64, 255);
		if (gSelectedSectorObject
			&& gSelectedSectorObject->getObjectType() == core::SectorObjectType::Walkway)
			drawList->AddLine(worldToScreen({ (float)plan.x, (float)plan.y }),
				worldToScreen({ (float)plan.x + 1.0f, (float)plan.y }), colour, 3.0f);
		else
		{
			drawList->AddRectFilled(topLeft, bottomRight,
				plan.valid ? IM_COL32(255, 255, 0, 45) : IM_COL32(255, 64, 64, 45));
			drawList->AddRect(topLeft, bottomRight, colour, 0.0f, 0, 2.0f);
		}
		if (!plan.valid && !plan.diagnostic.empty()) ImGui::SetTooltip("%s", plan.diagnostic.c_str());
	}
	drawList->PopClipRect();

	// The palette is editor chrome, so it remains available across the canvas.
	renderObjectPalette(building, canvasPos, canvasSize, drawList);
	drawList->PopClipRect();

	ImGui::End();
}

void renderUI(shared_ptr<core::Building>& building, shared_ptr<core::Agent> pathingAgent)
{
	ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

	renderMenu(building);
	renderDocumentToolbar(building);
	renderFilePopups(building);
	renderDockSpace();

	if (!building)
	{
		ImGui::Begin("Building");
		ImGui::TextDisabled("No Building is open.");
		ImGui::TextUnformatted("Choose File > New or File > Open to begin.");
		ImGui::End();
		return;
	}

	auto const graph = building->getGraph();
	renderStatusBar();
	renderControlsWindow(building, graph, pathingAgent);
	renderWorldWindow(building, graph);
}
