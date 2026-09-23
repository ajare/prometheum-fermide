#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
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
#include "PaletteLayout.h"
#include "DocumentEdit.h"
#include "DoorPanel.h"
#include "AgentGroupsPanel.h"
#include "AgentGroupAssignmentPanel.h"
#include "AgentTagAssignmentPanel.h"
#include "AgentBehaviourAssignmentPanel.h"
#include "TagsPanel.h"
#include "BehavioursPanel.h"
#include "AgentClipboard.h"

#if defined(_WIN32)
#include <nfd.h>
#elif defined(__linux__)
#include <nfd.h>
#else
#error "Unsupported platform"
#endif

#include "core/Vector2.h"
#include "core/Background.h"
#include "core/Button.h"
#include "core/Door.h"
#include "core/BulkheadDoor.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/ForceBridge.h"
#include "core/Ladder.h"
#include "core/Location.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LadderTransit.h"
#include "core/LiftSectorObject.h"
#include "core/StairwellTransit.h"
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
#include "core/AgentTagRegistryDocument.h"
#include "core/AgentBehaviourRegistryDocument.h"

#include "Main.h"
#include "RecentFiles.h"
#include "UI.h"
#include "Render.h"
#include "UISettings.h"
#include "AgentDropTargets.h"
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

// Drag-scrolling the World view: shift + middle button, or shift + alt +
// left button. Kept at file scope because a pan claims the left button for
// the whole gesture - the palette reports that through
// gPegmanConsumesLeftMouse - so no selection, paint, or move starts under
// the cursor while the view is being dragged.
struct ViewPanState
{
	bool dragging{ false };
	ImVec2 pressPosition{};
	ImVec2 scrollAtPress{};
};

static ViewPanState gViewPan;

void setSelectionMode(UISettings::SelectionMode mode);

namespace
{
	bool applyAgentPathEdit(shared_ptr<const core::Building> const& building, core::Agent* agent,
		shared_ptr<core::Path> path, bool startPathing, bool replaceCurrentPath)
	{
		if (!building || !agent || !path) return false;
		auto const agentId = building->getAgentId(agent);
		if (agentId && building->agentBehaviourOwnsMovement(agentId)) return false;
		auto undo = captureDocumentSnapshot(building);
		if (!undo) return false;

		auto const* requestedPath = path.get();
		if (replaceCurrentPath) agent->clearPath();
		agent->setPath(std::move(path), startPathing);
		if (agent->getPath().get() != requestedPath) return false;

		commitDocumentEdit(std::move(undo));
		return true;
	}

	constexpr float PaletteInset{ 16.0f };
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
		BulkheadDoor,
		Window,
		Walkway,
		ForceBridge,
		RoomLadder,
		PlatformLift
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
		// A pasted Agent is carried here while it falls. The placement is a
		// payload rather than a couple of loose fields because it may never
		// land: cancelling a paste drops the struct, and with it the only
		// thing that could have written an Agent or an Agent group.
		PendingAgentPlacement pastedAgent;
	};

	PaletteDropState gPegman;

	// Palette tray placement (ticket #41). The tray is dragged by any part of
	// itself that is not a button. Its position is remembered as an offset from
	// the tray's home in the view's bottom-right corner, so the palette follows
	// that corner as the window is resized, and is clamped into the canvas as
	// it is dragged.
	ImVec2 gPaletteTrayOffset{};

	struct PaletteTrayDrag
	{
		bool dragging{ false };
		ImVec2 grab{};	// Mouse position relative to the tray's top-left.
	};

	PaletteTrayDrag gTrayDrag;

	bool gSelectingAgentPathDestination{ false };
	UISettings::SelectionMode gPathSelectionPreviousMode{ UISettings::SelectionMode::Object };
	bool gPathSelectionPreviouslyRenderedGraph{ false };

	void beginAgentPathSelection(shared_ptr<core::Building> const& building)
	{
		if (!gSelectedAgent || gSelectingAgentPathDestination) return;
		auto const id = building ? building->getAgentId(gSelectedAgent) : core::AgentId{};
		if (building && id && building->agentBehaviourOwnsMovement(id)) return;
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
		Facade,
		Corridor,
		Background,
		Ladder,
		Stairwell,
		Staircase,
		Lift,
		Shuttle
	};

	struct PaintState
	{
		PaintTool tool{ PaintTool::None };
		bool dragging{ false };
		uint32_t layer{ 0 };
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
		bool stairwell{ false };
		ResizeEdge edge{ ResizeEdge::None };
		ImVec2 pressPosition{};
		uint32_t originalX{ 0 }, originalY{ 0 }, originalWidth{ 0 }, originalHeight{ 0 };
		core::Building::LocationEditPlan preview;
		core::Building::LiftEditPlan liftPreview;
		core::Building::ShuttleEditPlan shuttlePreview;
		core::Building::LadderEditPlan ladderPreview;
		core::Building::StairwellEditPlan stairwellPreview;
	};

	SectorResizeState gSectorResize;
	optional<core::Building::LocationEditPlan> gPendingLocationEdit;
	optional<core::Building::LiftEditPlan> gPendingLiftEdit;
	optional<core::Building::ShuttleEditPlan> gPendingShuttleEdit;
	optional<core::Building::LadderEditPlan> gPendingLadderEdit;
	optional<core::Building::StairwellEditPlan> gPendingStairwellEdit;
	optional<core::Building::PlatformLiftEditPlan> gPendingPlatformLiftEdit;
	optional<core::Building::WalkwayEditPlan> gPendingWalkwayEdit;
	optional<core::Building::ObjectMovePlan> gPendingObjectMove;
	optional<core::Building::LayerDeletePlan> gPendingLayerDelete;

	void reportEditorError(string const& source, string message);

	struct ObjectMoveState
	{
		bool dragging{ false };
		ResizeEdge edge{ ResizeEdge::Move };
		ImVec2 pressPosition{};
		uint32_t originalX{ 0 }, originalY{ 0 }, originalWidth{ 0 }, originalHeight{ 0 };
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
		shared_ptr<const core::SectorObject> const& object, uint32_t objectIndex,
		ResizeEdge edge = ResizeEdge::Move)
	{
		if (!building || !object || gObjectMove.dragging) return;
		auto owner = object->getSector();
		gObjectMove.dragging = true;
		gObjectMove.edge = edge;
		gObjectMove.pressPosition = ImGui::GetIO().MousePos;
		gObjectMove.originalX = object->getCellX();
		gObjectMove.originalY = object->getCellY();
		gObjectMove.originalWidth = (uint32_t)ceil(object->getSize().x);
		gObjectMove.originalHeight = (uint32_t)ceil(object->getSize().y);
		if (object->getObjectType() == core::SectorObjectType::Marker)
		{
			auto marker = static_pointer_cast<const core::MarkerSectorObject>(object)->getMarker();
			gObjectMove.originalX = marker->getCellX() + (uint32_t)floor(marker->getOffset());
			gObjectMove.originalY = marker->getCellY();
		}
		else if (object->getObjectType() == core::SectorObjectType::BulkheadDoor)
		{
			// BulkheadDoor SectorObjects are anchored in the cell left of their threshold.
			// Editor placement coordinates identify the cell to the right (offset 0.0).
			++gObjectMove.originalX;
		}
		gObjectMove.preview = edge == ResizeEdge::Move
			? building->planMoveSectorObject(owner->getIndex(), objectIndex,
				gObjectMove.originalX, gObjectMove.originalY)
			: object->getObjectType() == core::SectorObjectType::Door
				? building->planResizeSectorDoor(owner->getIndex(), objectIndex,
					gObjectMove.originalX, gObjectMove.originalY,
					gObjectMove.originalWidth, gObjectMove.originalHeight)
				: building->planResizeSectorWindow(owner->getIndex(), objectIndex,
					gObjectMove.originalX, gObjectMove.originalY,
					gObjectMove.originalWidth, gObjectMove.originalHeight);
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

	PegmanTarget getPegmanTarget(shared_ptr<const core::Building> const& building,
		ImVec2 feet, ImVec2 canvasPos, ImVec2 canvasSize)
	{
		if (!pointInRect(feet, canvasPos, canvasPos + canvasSize)) return {};
		return pegmanAgentTargetAtWorld(building, screenToWorld(feet));
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
		if (building->getLiftLandingGeometry(gUISettings.visibleLayer + 1, target.cellY, target.cellX, landingX, landingWidth))
			target.cellX = landingX;
		target.sector = building->getSectorAtPosition(gUISettings.visibleLayer,
			(float)target.cellX, world.y);
		// A Shuttle serves the Door pair from the Layer directly behind the Layer the
		// Door is authored on, so query the Layer behind the visible one.
		auto shuttleStops = building->getShuttleStopCandidatesForDoor(gUISettings.visibleLayer + 1, target.cellY, target.cellX);
		if (!shuttleStops.empty()) target.diagnostic.clear();
		else building->canAddCorridorDoor(gUISettings.visibleLayer, target.cellY, target.cellX, &target.diagnostic);
		return target;
	}

	PegmanTarget getBulkheadDoorTarget(shared_ptr<const core::Building> const& building,
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
			target.diagnostic = "Bulkhead Door position is outside the building";
			return target;
		}
		target.cellX = (uint32_t)floor(world.x);
		target.cellY = (uint32_t)floor(world.y);
		target.sector = building->getSectorAtPosition(gUISettings.visibleLayer,
			(float)target.cellX + 0.5f, (float)target.cellY + 0.5f);
		building->canAddSectorBulkheadDoor(gUISettings.visibleLayer, target.cellY,
			target.cellX, CORE_SIDE_LEFT, {}, &target.diagnostic);
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

	PegmanTarget getForceBridgeTarget(shared_ptr<const core::Building> const& building,
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
			target.diagnostic = "Force Bridge position is outside the building";
			return target;
		}
		target.cellX = (uint32_t)floor(world.x);
		target.cellY = (uint32_t)floor(world.y);
		target.sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
		auto room = dynamic_pointer_cast<const core::Location>(target.sector);
		if (!room || room->isCorridor()) target.diagnostic = "Force Bridges can only be placed in Rooms";
		else
		{
			target.deckOffset = target.cellY - room->getCellY();
			target.localX = (float)(target.cellX - room->getCellX());
			core::Building::CreateForceBridgeOptions options;
			if (building->calculateSectorForceBridgeWidthToRight(room->getIndex(),
				target.deckOffset, (uint32_t)target.localX, target.cellsWide,
				&target.diagnostic))
			{
				options.width = target.cellsWide;
				building->canAddSectorForceBridge(room->getIndex(), target.deckOffset,
					(uint32_t)target.localX, options, &target.diagnostic);
			}
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

	PegmanTarget getPlatformLiftTarget(shared_ptr<const core::Building> const& building,
		ImVec2 position, ImVec2 canvasPos, ImVec2 canvasSize)
	{
		PegmanTarget target;
		if (!pointInRect(position, canvasPos, canvasPos + canvasSize))
		{ target.diagnostic = "Drop inside the world"; return target; }
		auto world = screenToWorld(position);
		if (world.x < 0.0f || world.y < 0.0f)
		{ target.diagnostic = "PlatformLift position is outside the building"; return target; }
		target.sector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
		auto room = dynamic_pointer_cast<const core::Location>(target.sector);
		if (!room || room->isCorridor() || !room->pointInBounds(world.x, world.y))
		{ target.diagnostic = "PlatformLifts can only be placed in Rooms"; return target; }
		target.cellX = (uint32_t)floor(world.x);
		target.cellY = room->getCellY();
		target.deckOffset = 0;
		target.localX = (float)(target.cellX - room->getCellX());
		core::Building::CreateLiftOptions options;
		options.cellsWide = 1;
		for (auto const& candidate : building->getPlatformLiftStopCandidates(
			room->getIndex(), (uint32_t)target.localX))
		{
			options.stopOffsets = { 0, candidate.deckOffset };
			if (building->canAddPlatformLift(room->getIndex(), (uint32_t)target.localX,
				options, &target.diagnostic))
			{
				target.floorY = (float)(room->getCellY() + candidate.deckOffset + 1);
				return target;
			}
		}
		target.diagnostic = "No eligible Walkway exists above this PlatformLift position";
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

	void drawBulkheadDoorIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		auto centre = (boundsMin + boundsMax) * 0.5f;
		drawList->AddLine({ centre.x, boundsMin.y + 6.0f },
			{ centre.x, boundsMax.y - 6.0f }, colour, 5.0f);
		drawList->AddLine({ centre.x - 11.0f, boundsMin.y + 8.0f },
			{ centre.x + 11.0f, boundsMin.y + 8.0f }, colour, 2.0f);
	}

	void drawWindowIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		drawObjectIcon(drawList, boundsMin, boundsMax, colour, ICON_FA_WINDOW_MAXIMIZE);
	}

	void drawWalkwayIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		drawObjectIcon(drawList, boundsMin, boundsMax, colour, ICON_FA_GRIP_LINES);
	}

	void drawForceBridgeIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		auto y = (boundsMin.y + boundsMax.y) * 0.5f;
		auto left = boundsMin.x + 10.0f, right = boundsMax.x - 10.0f;
		drawList->AddLine({ left, y }, { right, y }, colour, 2.5f);
		drawList->AddTriangleFilled({ right, y }, { right - 7.0f, y - 4.0f },
			{ right - 7.0f, y + 4.0f }, colour);
	}

	void drawPlatformLiftIcon(ImDrawList* drawList, ImVec2 boundsMin, ImVec2 boundsMax, ImU32 colour)
	{
		auto centre = (boundsMin + boundsMax) * 0.5f;
		auto left = centre.x - 11.0f, right = centre.x + 11.0f;
		auto top = boundsMin.y + 6.0f, bottom = boundsMax.y - 6.0f;
		drawList->AddLine({ left, top }, { left, bottom }, colour, 2.0f);
		drawList->AddLine({ right, top }, { right, bottom }, colour, 2.0f);
		drawList->AddLine({ left - 3.0f, centre.y + 5.0f }, { right + 3.0f, centre.y + 5.0f }, colour, 3.0f);
		drawList->AddTriangleFilled({ centre.x, top }, { centre.x - 4.0f, top + 6.0f },
			{ centre.x + 4.0f, top + 6.0f }, colour);
		drawList->AddTriangleFilled({ centre.x, bottom }, { centre.x - 4.0f, bottom - 6.0f },
			{ centre.x + 4.0f, bottom - 6.0f }, colour);
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
				// The icon is drawn MarkerDeckLift above the deck, so the hit box covers
				// the icon and the gap down to the deck the Vertex stays on.
				auto point = worldToScreen({ world.x, world.y + MarkerDeckLift });
				auto deckPad = MarkerDeckLift * CORE_DECK_HEIGHT_PIXELS + 2.0f;
				if (pointInRect(position, point - ImVec2(MarkerIconSize * 0.5f, MarkerIconSize),
					point + ImVec2(MarkerIconSize * 0.5f, deckPad))) return object;
			}
		}
		return nullptr;
	}

	shared_ptr<const core::SectorObject> bulkheadDoorAtScreenPosition(
		shared_ptr<const core::Building> const& building, ImVec2 position)
	{
		constexpr float tolerance = 7.0f;
		set<void const*> visited;
		for (auto const& sector : building->getSectors(gUISettings.visibleLayer))
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<const core::BulkheadDoorSectorObject>(sector->getObject(i));
				if (!object || !visited.insert(object.get()).second) continue;
				core::Vector2 first, second;
				object->getDoor()->getFullShape(first, second);
				auto topLeft = worldToScreen({ min(first.x, second.x), max(first.y, second.y) });
				auto bottomRight = worldToScreen({ max(first.x, second.x), min(first.y, second.y) });
				topLeft -= ImVec2(tolerance, tolerance);
				bottomRight += ImVec2(tolerance, tolerance);
				if (pointInRect(position, topLeft, bottomRight)) return object;
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

	shared_ptr<const core::SectorObject> platformLiftAtScreenPosition(
		shared_ptr<const core::Building> const& building, ImVec2 position)
	{
		constexpr float tolerance = 5.0f;
		for (auto const& sector : building->getSectors(gUISettings.visibleLayer))
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<const core::LiftSectorObject>(sector->getObject(i));
				if (!object) continue;
				core::Vector2 first, second;
				object->getLift()->getCurrentShape(first, second);
				auto topLeft = worldToScreen({ min(first.x, second.x), max(first.y, second.y) });
				auto bottomRight = worldToScreen({ max(first.x, second.x), min(first.y, second.y) });
				topLeft -= ImVec2(tolerance, tolerance);
				bottomRight += ImVec2(tolerance, tolerance);
				if (pointInRect(position, topLeft, bottomRight)) return object;
			}
		}
		return nullptr;
	}

	shared_ptr<const core::SectorObject> forceBridgeAtScreenPosition(
		shared_ptr<const core::Building> const& building, ImVec2 position)
	{
		constexpr float tolerance = 8.0f;
		for (auto const& sector : building->getSectors(gUISettings.visibleLayer))
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::ForceBridge) continue;
				auto bridge = static_pointer_cast<const core::ForceBridgeSectorObject>(object)->getForceBridge();
				auto left = worldToScreen({ (float)object->getCellX(), (float)object->getCellY() });
				auto right = worldToScreen({ (float)object->getCellX() + bridge->getSize().x,
					(float)object->getCellY() });
				if (position.x >= left.x && position.x <= right.x
					&& abs(position.y - left.y) <= tolerance) return object;
			}
		}
		return nullptr;
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
		for (uint32_t layer = 0; layer < building->getLayerCount(); ++layer)
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

	string nextFacadeName(shared_ptr<const core::Building> const& building)
	{
		set<string> names;
		for (uint32_t layer = 0; layer < building->getLayerCount(); ++layer)
		{
			for (auto const& sector : building->getSectors(layer))
				names.insert(sector->getName());
		}

		for (uint64_t number = 1;; ++number)
		{
			auto name = format("Facade {}", number);
			if (!names.contains(name)) return name;
		}
	}

	vector<uint32_t> shuttleCandidates(shared_ptr<const core::Building> const& building,
		ShuttleDraft const& draft)
	{
		if (draft.numCars <= 0 || draft.carWidth < 3 || draft.carWidth > 5) return {};
		return building->getValidShuttleStopOffsets((uint32_t)gUISettings.visibleLayer,
			draft.y, draft.x, draft.cellsWide,
			(uint32_t)draft.numCars, (uint32_t)draft.carWidth, draft.allowPartialLandings,
			draft.doorMask);
	}

	bool validateShuttleDraft(shared_ptr<const core::Building> const& building,
		ShuttleDraft& draft)
	{
		draft.diagnostic.clear();
		if (draft.numCars <= 0) draft.diagnostic = "A Shuttle requires at least one carriage";
		else if (draft.carWidth < 3 || draft.carWidth > 5) draft.diagnostic = "Carriage width must be between 3 and 5 cells";
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
			int endX = clamp((int)floor(world.x), 0, (int)building->getCellsWide() - 1);
			int endY = clamp((int)floor(world.y), 0, (int)building->getDecksHigh() - 1);
			int x = min(gPaint.anchorX, endX);
			int y = min(gPaint.anchorY, endY);
			uint32_t width = (uint32_t)(abs(endX - gPaint.anchorX) + 1);
			PaintRectangle result{ false, (uint32_t)x, (uint32_t)y, width, 2, {} };
			if (abs(endY - gPaint.anchorY) != 1)
				result.diagnostic = "Drag between endpoints on adjacent levels";
			else if (width < 2)
				result.diagnostic = "A Staircase must be at least two cells wide";
			else
			{
				int lowerX = gPaint.anchorY == y ? gPaint.anchorX : endX;
				int riseSide = lowerX == x ? CORE_SIDE_RIGHT : CORE_SIDE_LEFT;
				result.valid = building->canAddStaircase(gUISettings.visibleLayer, result.y, result.x, result.width,
					riseSide, &result.diagnostic);
			}
			return result;
		}
		if (gPaint.tool == PaintTool::Stairwell)
		{
			bool leftward = floor(world.x) < gPaint.anchorX;
			int x = leftward ? gPaint.anchorX - 1 : gPaint.anchorX;
			if (x + 2 > (int)building->getCellsWide()) x = gPaint.anchorX - 1;
			int endY = clamp((int)floor(world.y), 0, (int)building->getDecksHigh() - 1);
			int y = min(gPaint.anchorY, endY);
			uint32_t height = (uint32_t)(abs(endY - gPaint.anchorY) + 1);
			PaintRectangle result{ false, (uint32_t)max(0, x), (uint32_t)y, 2, height, {} };
			if (x < 0 || x + 2 > (int)building->getCellsWide())
				result.diagnostic = "The Stairwell is outside the Building bounds";
			else
				result.valid = building->canAddStairwell(gUISettings.visibleLayer, result.y, result.x, result.height,
					&result.diagnostic);
			return result;
		}
		if (gPaint.tool == PaintTool::Ladder)
		{
			int endY = clamp((int)floor(world.y), 0, (int)building->getDecksHigh() - 1);
			int y = min(gPaint.anchorY, endY);
			uint32_t height = (uint32_t)(abs(endY - gPaint.anchorY) + 1);
			PaintRectangle result{ false, (uint32_t)gPaint.anchorX, (uint32_t)y, 1, height, {} };
			result.valid = building->canAddLadder(gUISettings.visibleLayer, result.y, result.x, result.height,
				&result.diagnostic);
			return result;
		}
		if (gPaint.tool == PaintTool::Background)
		{
			// Unlike a Room, a Background does not shrink to the largest free block:
			// the full dragged rectangle is the request, and any occupied cell inside
			// it refuses the paint. canAddBackground() owns the rule and the
			// diagnostic, so the preview turns red where the refusal lands.
			int endX = clamp((int)floor(world.x), 0, (int)building->getCellsWide() - 1);
			int endY = clamp((int)floor(world.y), 0, (int)building->getDecksHigh() - 1);
			int x = min(gPaint.anchorX, endX);
			int y = min(gPaint.anchorY, endY);
			PaintRectangle result{ false, (uint32_t)x, (uint32_t)y,
				(uint32_t)(abs(endX - gPaint.anchorX) + 1),
				(uint32_t)(abs(endY - gPaint.anchorY) + 1), {} };
			result.valid = building->canAddBackground(gPaint.layer, result.y, result.x,
				result.width, result.height, &result.diagnostic);
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
			// The shaft is painted on the drag's Layer; its landings are read from the
			// Layer directly in front, which is where the corridor rows live.
			uint32_t stops = 0;
			for (auto const& row : building->getLiftLandingRows(gPaint.layer, best.y, best.x,
				best.width, best.height))
			{
				if (!row.location || !row.fullyOverlapping || row.obstructed
					|| !row.location->isCorridor())
					continue;
				if (!row.callButtonSpace)
				{
					best.valid = false;
					best.diagnostic = "There is no corridor space for a Lift call button";
					return best;
				}
				++stops;
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
		gPegman.pastedAgent.cancel();
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
		// The Shuttle sits one Layer behind the Layer the Door is authored on.
		gShuttleDoorCandidates = building->getShuttleStopCandidatesForDoor(gUISettings.visibleLayer + 1, target.cellY, target.cellX);
		if (!gShuttleDoorCandidates.empty())
		{
			gSelectedShuttleDoorCandidate = 0;
			gOpenShuttleStopPopup = true;
			return;
		}
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto created = building->addSectorDoor(gUISettings.visibleLayer, target.cellY, target.cellX);
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

	void placeBulkheadDoor(shared_ptr<core::Building> const& building, PegmanTarget const& target)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto created = building->addSectorBulkheadDoor(gUISettings.visibleLayer,
				target.cellY, target.cellX, CORE_SIDE_LEFT);
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

	void placePlatformLift(shared_ptr<core::Building> const& building, PegmanTarget const& target)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			core::Building::CreateLiftOptions options;
			options.cellsWide = 1;
			for (auto const& candidate : building->getPlatformLiftStopCandidates(
				target.sector->getIndex(), (uint32_t)target.localX))
			{
				options.stopOffsets = { 0, candidate.deckOffset };
				string diagnostic;
				if (!building->canAddPlatformLift(target.sector->getIndex(),
					(uint32_t)target.localX, options, &diagnostic)) continue;
				auto created = building->addSectorPlatformLift(target.sector->getIndex(), 0,
					(uint32_t)target.localX, options);
				building->finishBuild();
				setSelectionMode(UISettings::SelectionMode::Object);
				gSelectedAgent = nullptr; gSelectedSector.reset();
				gSelectedSectorObject = created.lift.sector->getObject(created.lift.index);
				commitDocumentEdit(std::move(undo));
				return;
			}
			throw runtime_error("No eligible Walkway exists above this PlatformLift position");
		}
		catch (core::Exception const& error)
		{ core::addLogMessage("PlatformLift editor", 0, core::LogLevel::Error, error.getMessage()); }
		catch (std::exception const& error)
		{ core::addLogMessage("PlatformLift editor", 0, core::LogLevel::Error, error.what()); }
	}

	void placeForceBridge(shared_ptr<core::Building> const& building, PegmanTarget const& target)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			core::Building::CreateForceBridgeOptions options;
			options.width = target.cellsWide;
			auto created = building->addSectorForceBridge(target.sector->getIndex(),
				target.deckOffset, (uint32_t)target.localX, options);
			building->finishBuild();
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = created.forceBridge.sector->getObject(created.forceBridge.index);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Force Bridge editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Force Bridge editor", 0, core::LogLevel::Error, error.what());
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
			core::AgentId placed{};
			string diagnostic;
			// A pasted Agent lands with the Agent group its clipboard payload
			// named; one dragged off the palette lands with none. Both land
			// through the same single-edit placement, so neither can leave a
			// half-written document behind, and a placement that is refused
			// says so instead of taking the crash silently.
			auto const landed = gPegman.pastedAgent.armed()
				? commitPendingAgentPlacement(gPegman.pastedAgent, building, placed, diagnostic)
				: commitAgentPlacement(building,
					AgentClipboardPayload{ nextAgentName(building), 0, true, nullopt },
					gPegman.sector, gPegman.deckOffset, gPegman.localX, placed, diagnostic);
			if (!landed)
			{
				reportEditorError("Agent editor", diagnostic);
			}
			else
			{
				setSelectionMode(UISettings::SelectionMode::Object);
				gSelectedAgent = building->lookupAgent(placed).entity;
				gSelectedSector.reset();
				gSelectedSectorObject.reset();
			}
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
		// A Corridor is a Location and may be painted on any Layer; Transits need a
		// Layer in front to land on, so they cannot be painted on the front-most one.
		if ((gPaint.tool == PaintTool::Ladder || gPaint.tool == PaintTool::Stairwell
			|| gPaint.tool == PaintTool::Lift || gPaint.tool == PaintTool::Shuttle)
			&& gUISettings.visibleLayer == 0)
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

		auto const traySize = paletteTraySize();
		auto const trayHomeTopLeft = canvasPos + canvasSize
			- ImVec2(PaletteInset, PaletteInset) - traySize;
		auto const trayTopLeft = paletteClampTopLeft(canvasPos, canvasSize,
			trayHomeTopLeft + gPaletteTrayOffset);
		auto const trayBottomRight = trayTopLeft + traySize;
		// The grip is the tray minus its buttons: the padding and the slot gaps.
		bool overTray = gWorldHovered && pointInRect(io.MousePos, trayTopLeft, trayBottomRight);
		bool overGrip = overTray && !paletteButtonAt(trayTopLeft, io.MousePos);
		auto roomMin = paletteSlotMin(trayTopLeft, PaletteSlot::Room);
		auto facadeMin = paletteSlotMin(trayTopLeft, PaletteSlot::Facade);
		auto corridorMin = paletteSlotMin(trayTopLeft, PaletteSlot::Corridor);
		auto backgroundMin = paletteSlotMin(trayTopLeft, PaletteSlot::Background);
		auto ladderMin = paletteSlotMin(trayTopLeft, PaletteSlot::Ladder);
		auto stairwellMin = paletteSlotMin(trayTopLeft, PaletteSlot::Stairwell);
		auto liftMin = paletteSlotMin(trayTopLeft, PaletteSlot::Lift);
		auto shuttleMin = paletteSlotMin(trayTopLeft, PaletteSlot::Shuttle);
		auto staircaseMin = paletteSlotMin(trayTopLeft, PaletteSlot::Staircase);
		auto agentMin = paletteSlotMin(trayTopLeft, PaletteSlot::Agent);
		auto markerMin = paletteSlotMin(trayTopLeft, PaletteSlot::Marker);
		auto doorMin = paletteSlotMin(trayTopLeft, PaletteSlot::Door);
		auto bulkheadDoorMin = paletteSlotMin(trayTopLeft, PaletteSlot::BulkheadDoor);
		auto windowMin = paletteSlotMin(trayTopLeft, PaletteSlot::Window);
		auto walkwayMin = paletteSlotMin(trayTopLeft, PaletteSlot::Walkway);
		auto forceBridgeMin = paletteSlotMin(trayTopLeft, PaletteSlot::ForceBridge);
		auto roomLadderMin = paletteSlotMin(trayTopLeft, PaletteSlot::RoomLadder);
		auto platformLiftMin = paletteSlotMin(trayTopLeft, PaletteSlot::PlatformLift);
		auto roomMax = paletteSlotMax(trayTopLeft, PaletteSlot::Room);
		auto facadeMax = paletteSlotMax(trayTopLeft, PaletteSlot::Facade);
		auto corridorMax = paletteSlotMax(trayTopLeft, PaletteSlot::Corridor);
		auto backgroundMax = paletteSlotMax(trayTopLeft, PaletteSlot::Background);
		auto ladderMax = paletteSlotMax(trayTopLeft, PaletteSlot::Ladder);
		auto stairwellMax = paletteSlotMax(trayTopLeft, PaletteSlot::Stairwell);
		auto liftMax = paletteSlotMax(trayTopLeft, PaletteSlot::Lift);
		auto shuttleMax = paletteSlotMax(trayTopLeft, PaletteSlot::Shuttle);
		auto staircaseMax = paletteSlotMax(trayTopLeft, PaletteSlot::Staircase);
		auto windowMax = paletteSlotMax(trayTopLeft, PaletteSlot::Window);
		auto doorMax = paletteSlotMax(trayTopLeft, PaletteSlot::Door);
		auto bulkheadDoorMax = paletteSlotMax(trayTopLeft, PaletteSlot::BulkheadDoor);
		auto agentMax = paletteSlotMax(trayTopLeft, PaletteSlot::Agent);
		auto markerMax = paletteSlotMax(trayTopLeft, PaletteSlot::Marker);
		auto walkwayMax = paletteSlotMax(trayTopLeft, PaletteSlot::Walkway);
		auto forceBridgeMax = paletteSlotMax(trayTopLeft, PaletteSlot::ForceBridge);
		auto roomLadderMax = paletteSlotMax(trayTopLeft, PaletteSlot::RoomLadder);
		auto platformLiftMax = paletteSlotMax(trayTopLeft, PaletteSlot::PlatformLift);
		// The tray border lights up while the grip is under the cursor, so the
		// palette shows that it can be picked up.
		auto const trayBorder = gTrayDrag.dragging ? yellow
			: overGrip ? IM_COL32(251, 188, 4, 150) : borderColour;
		drawList->AddRectFilled(trayTopLeft, trayBottomRight, trayColour, 5.0f);
		drawList->AddRect(trayTopLeft, trayBottomRight, trayBorder, 5.0f);

		bool roomHovered = gWorldHovered && pointInRect(io.MousePos, roomMin, roomMax);
		bool facadeHovered = gWorldHovered && pointInRect(io.MousePos, facadeMin, facadeMax);
		bool corridorHovered = gWorldHovered && pointInRect(io.MousePos, corridorMin, corridorMax);
		bool backgroundHovered = gWorldHovered && pointInRect(io.MousePos, backgroundMin, backgroundMax);
		bool ladderHovered = gWorldHovered && pointInRect(io.MousePos, ladderMin, ladderMax);
		bool stairwellHovered = gWorldHovered && pointInRect(io.MousePos, stairwellMin, stairwellMax);
		bool liftHovered = gWorldHovered && pointInRect(io.MousePos, liftMin, liftMax);
		bool shuttleHovered = gWorldHovered && pointInRect(io.MousePos, shuttleMin, shuttleMax);
		bool staircaseHovered = gWorldHovered && pointInRect(io.MousePos, staircaseMin, staircaseMax);
		bool backOnlyDisabled = gUISettings.visibleLayer == 0;
		if (overTray) paletteConsumedMouse = true;

		// Picking the palette up: any part of the tray that is not a button
		// drags the whole tray, which stays inside the view canvas.
		if (overGrip && gPegman.phase == PalettePhase::Home && !gViewPan.dragging
			&& io.MouseClicked[0])
		{
			gTrayDrag.dragging = true;
			gTrayDrag.grab = io.MousePos - trayTopLeft;
		}
		if (gTrayDrag.dragging)
		{
			paletteConsumedMouse = true;
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
			gPaletteTrayOffset = paletteClampTopLeft(canvasPos, canvasSize,
				io.MousePos - gTrayDrag.grab) - trayHomeTopLeft;
			if (!io.MouseDown[0]) gTrayDrag.dragging = false;
		}

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

		if (gPegman.phase == PalettePhase::Home && !gTrayDrag.dragging
			&& (roomHovered || facadeHovered || corridorHovered || backgroundHovered || ladderHovered
				|| stairwellHovered || staircaseHovered || liftHovered || shuttleHovered))
		{
			paletteConsumedMouse = true;
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (ladderHovered && backOnlyDisabled)
				ImGui::SetTooltip("Ladders can only be painted on the Back Layer");
			else if ((stairwellHovered || staircaseHovered || liftHovered || shuttleHovered) && backOnlyDisabled)
				ImGui::SetTooltip("Stairwells, Staircases, Lifts, and Shuttles can only be painted on the Back Layer");
			else
				ImGui::SetTooltip(roomHovered ? "Paint Room" : facadeHovered ? "Paint Facade"
					: corridorHovered ? "Paint Corridor"
					: backgroundHovered ? "Paint Background" : ladderHovered ? "Paint Ladder"
					: stairwellHovered ? "Paint Stairwell" : staircaseHovered ? "Paint Staircase"
					: liftHovered ? "Paint Lift" : "Paint Shuttle");

			if (io.MouseClicked[0] && !gViewPan.dragging
				&& !((ladderHovered || stairwellHovered || staircaseHovered || liftHovered || shuttleHovered)
					&& backOnlyDisabled))
			{
				auto clickedTool = roomHovered ? PaintTool::Room
					: facadeHovered ? PaintTool::Facade
					: corridorHovered ? PaintTool::Corridor
					: backgroundHovered ? PaintTool::Background
					: ladderHovered ? PaintTool::Ladder
					: stairwellHovered ? PaintTool::Stairwell
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
		drawPaintButton(facadeMin, facadeMax, "Facade", PaintTool::Facade,
			facadeHovered, false);
		drawPaintButton(corridorMin, corridorMax, "Corridor", PaintTool::Corridor,
			corridorHovered, false);
		drawPaintButton(backgroundMin, backgroundMax, "Background", PaintTool::Background,
			backgroundHovered, false);
		drawPaintButton(ladderMin, ladderMax, "Ladder", PaintTool::Ladder,
			ladderHovered, backOnlyDisabled);
		drawPaintButton(stairwellMin, stairwellMax, "Stairwell", PaintTool::Stairwell,
			stairwellHovered, backOnlyDisabled);
		drawPaintButton(liftMin, liftMax, "Lift", PaintTool::Lift, liftHovered, backOnlyDisabled);
		drawPaintButton(shuttleMin, shuttleMax, "Shuttle", PaintTool::Shuttle,
			shuttleHovered, backOnlyDisabled);
		drawPaintButton(staircaseMin, staircaseMax, "Staircase", PaintTool::Staircase,
			staircaseHovered, backOnlyDisabled);
		drawBulkheadDoorIcon(drawList, bulkheadDoorMin, bulkheadDoorMax, yellow);
		drawWindowIcon(drawList, windowMin, windowMax, yellow);
		drawWalkwayIcon(drawList, walkwayMin, walkwayMax, yellow);
		drawForceBridgeIcon(drawList, forceBridgeMin, forceBridgeMax, IM_COL32(0, 255, 0, 255));
		drawLadderIcon(drawList, roomLadderMin, roomLadderMax, yellow);
		drawPlatformLiftIcon(drawList, platformLiftMin, platformLiftMax, yellow);

		bool paintWasActive = gPaint.tool != PaintTool::None;
		if (paintWasActive && (ImGui::IsKeyPressed(ImGuiKey_Escape)
			|| (gWorldHovered && io.MouseClicked[1])))
		{
			resetPaint();
			paletteConsumedMouse = true;
		}

		if (gPaint.tool != PaintTool::None && !gPaint.dragging && gWorldHovered
			&& !overTray && !gViewPan.dragging && io.MouseClicked[0])
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
						else if (tool == PaintTool::Facade)
						{
							// Placed with the Room's validation - the drag already shrank to
							// the largest free block - and selected on release so the colour
							// picker is one click away.
							auto const index = building->addFacade(nextFacadeName(building),
								gPaint.layer, paintRectangle.y, paintRectangle.x,
								paintRectangle.width, paintRectangle.height, CORE_ROOM_MAX_HEIGHT);
							setSelectionMode(UISettings::SelectionMode::Sector);
							gSelectedSector = building->getSector(index);
						}
						else if (tool == PaintTool::Corridor)
							building->addCorridor(gPaint.layer, paintRectangle.y, paintRectangle.x,
								paintRectangle.width, 1);
						else if (tool == PaintTool::Background)
						{
							// Painted on the drag's Layer. A Background is legal on any
							// Layer, so it never takes the transit front-layer gate.
							auto const index = building->addBackground(gPaint.layer, paintRectangle.y,
								paintRectangle.x, paintRectangle.width, paintRectangle.height);
							setSelectionMode(UISettings::SelectionMode::Sector);
							gSelectedSector = building->getSector(index);
						}
						else if (tool == PaintTool::Ladder)
							building->addLadder(gUISettings.visibleLayer, paintRectangle.y, paintRectangle.x,
								{ paintRectangle.height, false, true });
						else if (tool == PaintTool::Stairwell)
						{
							int mountSide = paintRectangle.x < (uint32_t)gPaint.anchorX
								? CORE_SIDE_RIGHT : CORE_SIDE_LEFT;
							building->addStairwell(gUISettings.visibleLayer, paintRectangle.y, paintRectangle.x,
								{ paintRectangle.height, mountSide });
						}
						else if (tool == PaintTool::Staircase)
						{
							int lowerX = gPaint.anchorY == (int)paintRectangle.y
								? gPaint.anchorX
								: (gPaint.anchorX == (int)paintRectangle.x
									? (int)(paintRectangle.x + paintRectangle.width - 1) : (int)paintRectangle.x);
							int riseSide = lowerX == (int)paintRectangle.x
								? CORE_SIDE_RIGHT : CORE_SIDE_LEFT;
							building->addStaircase(gUISettings.visibleLayer, paintRectangle.y, paintRectangle.x,
								paintRectangle.width, riseSide);
						}
						else building->addLift(gUISettings.visibleLayer, paintRectangle.y, paintRectangle.x,
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
		if (gWorldHovered && gPegman.phase == PalettePhase::Home && !gTrayDrag.dragging)
		{
			if (pointInRect(io.MousePos, agentMin, agentMax)) hoveredItem = PaletteItem::Agent;
			else if (pointInRect(io.MousePos, markerMin, markerMax)) hoveredItem = PaletteItem::Marker;
			else if (pointInRect(io.MousePos, doorMin, doorMax)) hoveredItem = PaletteItem::Door;
			else if (pointInRect(io.MousePos, bulkheadDoorMin, bulkheadDoorMax)) hoveredItem = PaletteItem::BulkheadDoor;
			else if (pointInRect(io.MousePos, windowMin, windowMax)) hoveredItem = PaletteItem::Window;
			else if (pointInRect(io.MousePos, walkwayMin, walkwayMax)) hoveredItem = PaletteItem::Walkway;
			else if (pointInRect(io.MousePos, forceBridgeMin, forceBridgeMax)) hoveredItem = PaletteItem::ForceBridge;
			else if (pointInRect(io.MousePos, roomLadderMin, roomLadderMax)) hoveredItem = PaletteItem::RoomLadder;
			else if (pointInRect(io.MousePos, platformLiftMin, platformLiftMax)) hoveredItem = PaletteItem::PlatformLift;
		}
		drawList->AddRect(bulkheadDoorMin, bulkheadDoorMax,
			hoveredItem == PaletteItem::BulkheadDoor ? yellow : borderColour, 3.0f);
		drawList->AddRect(windowMin, windowMax,
			hoveredItem == PaletteItem::Window ? yellow : borderColour, 3.0f);
		drawList->AddRect(walkwayMin, walkwayMax,
			hoveredItem == PaletteItem::Walkway ? yellow : borderColour, 3.0f);
		drawList->AddRect(forceBridgeMin, forceBridgeMax,
			hoveredItem == PaletteItem::ForceBridge ? yellow : borderColour, 3.0f);
		drawList->AddRect(roomLadderMin, roomLadderMax,
			hoveredItem == PaletteItem::RoomLadder ? yellow : borderColour, 3.0f);
		drawList->AddRect(platformLiftMin, platformLiftMax,
			hoveredItem == PaletteItem::PlatformLift ? yellow : borderColour, 3.0f);
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
				: hoveredItem == PaletteItem::BulkheadDoor ? "Drag to add Bulkhead Door"
				: hoveredItem == PaletteItem::Window ? "Drag to add Window"
				: hoveredItem == PaletteItem::Walkway ? "Drag to add Walkway"
				: hoveredItem == PaletteItem::ForceBridge ? "Drag to add Force Bridge"
				: hoveredItem == PaletteItem::RoomLadder ? "Drag to add Room Ladder"
				: hoveredItem == PaletteItem::PlatformLift ? "Drag to add Platform Lift" : "Drag to add Door");
			if (io.MouseClicked[0] && !gViewPan.dragging)
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
			else if (gPegman.item == PaletteItem::BulkheadDoor)
				target = getBulkheadDoorTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::Window)
				target = getWindowTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::Walkway)
				target = getWalkwayTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::ForceBridge)
				target = getForceBridgeTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::RoomLadder)
				target = getRoomLadderTarget(building, io.MousePos, canvasPos, canvasSize);
			else if (gPegman.item == PaletteItem::PlatformLift)
				target = getPlatformLiftTarget(building, io.MousePos, canvasPos, canvasSize);
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
				else if (target && gPegman.item == PaletteItem::BulkheadDoor)
				{
					placeBulkheadDoor(building, target);
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
				else if (target && gPegman.item == PaletteItem::ForceBridge)
				{
					placeForceBridge(building, target);
					resetPegman();
				}
				else if (target && gPegman.item == PaletteItem::RoomLadder)
				{
					placeRoomLadder(building, target);
					resetPegman();
				}
				else if (target && gPegman.item == PaletteItem::PlatformLift)
				{
					placePlatformLift(building, target);
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
					? worldToScreen({ target.sector->getPosition().x + target.localX,
						target.floorY + MarkerDeckLift })
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
			else if (gPegman.item == PaletteItem::ForceBridge)
			{
				if (target.sector)
					drawForceBridgeIcon(drawList,
						worldToScreen({ (float)target.cellX, (float)target.cellY + 0.5f }),
						worldToScreen({ (float)target.cellX + target.cellsWide,
							(float)target.cellY - 0.5f }), colour);
				else drawForceBridgeIcon(drawList, io.MousePos - ImVec2(24.0f, 18.0f),
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
			else if (gPegman.item == PaletteItem::PlatformLift)
			{
				if (target.sector && target.floorY > (float)target.cellY)
					drawPlatformLiftIcon(drawList,
						worldToScreen({ (float)target.cellX, target.floorY }),
						worldToScreen({ (float)target.cellX + 1.0f, (float)target.cellY }), colour);
				else drawPlatformLiftIcon(drawList, io.MousePos - ImVec2(24.0f, 18.0f),
					io.MousePos + ImVec2(24.0f, 18.0f), colour);
			}
			else if (gPegman.item == PaletteItem::BulkheadDoor)
			{
				auto x = target.sector ? worldToScreen({ (float)target.cellX, 0.0f }).x : io.MousePos.x;
				auto top = target.sector
					? worldToScreen({ (float)target.cellX, (float)target.cellY + CORE_CORRIDOR_HEIGHT }).y
					: io.MousePos.y - CORE_DECK_HEIGHT_PIXELS * CORE_CORRIDOR_HEIGHT * 0.5f;
				auto bottom = target.sector ? worldToScreen({ (float)target.cellX, (float)target.cellY }).y
					: io.MousePos.y + CORE_DECK_HEIGHT_PIXELS * CORE_CORRIDOR_HEIGHT * 0.5f;
				drawList->AddLine({ x, top }, { x, bottom }, colour, 5.0f);
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
			|| gPaint.dragging || paintWasActive || gViewPan.dragging;
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
	// Layer labels come from the Building's editable layer names.
	string layerLabel(shared_ptr<const core::Building> const& building, uint32_t layer)
	{
		if (!building || layer >= building->getLayerCount())
			return format("Layer {}", layer);
		return building->getLayerName(layer);
	}

	constexpr size_t LayerNameBufferSize{ 64 };

	struct LayerNameEdit
	{
		std::array<char, LayerNameBufferSize> text{};
		bool editing{ false };
		std::string previous;
	};

	std::map<uint32_t, LayerNameEdit> gLayerNameEdits;

	std::string trimLayerName(std::string const& value)
	{
		auto const first = value.find_first_not_of(" \t");
		if (first == std::string::npos) return {};
		auto const last = value.find_last_not_of(" \t");
		return value.substr(first, last - first + 1);
	}

	// Inline editor for one layer's name.  The edit is committed when the field is
	// submitted with Enter or loses focus, and is undoable as a single document edit.
	void renderLayerNameEditor(shared_ptr<core::Building> const& building, uint32_t layer)
	{
		auto& edit = gLayerNameEdits[layer];

		if (!edit.editing)
		{
			auto const& name = building->getLayerName(layer);
			std::strncpy(edit.text.data(), name.c_str(), edit.text.size() - 1);
			edit.text[edit.text.size() - 1] = '\0';
		}

		ImGui::SetNextItemWidth(-1.0f);
		auto const submitted = ImGui::InputText("##layerName", edit.text.data(), edit.text.size(),
			ImGuiInputTextFlags_EnterReturnsTrue);

		if (!edit.editing)
		{
			if (submitted || ImGui::IsItemActivated())
			{
				edit.editing = true;
				edit.previous = building->getLayerName(layer);
			}
			return;
		}

		if (!submitted && ImGui::IsItemFocused()) return;

		edit.editing = false;

		auto const next = trimLayerName(edit.text.data());
		if (next.empty() || next == edit.previous) return;

		auto undo = captureDocumentSnapshot(building);
		try
		{
			building->setLayerName(layer, next);
			commitDocumentEdit(std::move(undo));
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Layers", 0, core::LogLevel::Error, error.what());
		}
	}

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
		gPendingStairwellEdit.reset();
		gPendingPlatformLiftEdit.reset();
		gPendingWalkwayEdit.reset();
		gPendingObjectMove.reset();
		gPendingLayerDelete.reset();
		gShuttleDraft.reset();
		gShuttleDoorCandidates.clear();
		gLayerNameEdits.clear();
		resetAgentGroupsPanelState();
		resetAgentTagAssignmentPanelState();
		resetTagsPanelState();
		resetBehavioursPanelState();
		gUISettings.worldPaused = false;
		if (clearHistory) gBuildingDocumentHistory.clear();
	}

	bool restoreDocumentSnapshot(shared_ptr<core::Building>& building, bool redo)
	{
		if (!building || (redo ? !gBuildingDocumentHistory.canRedo()
			: !gBuildingDocumentHistory.canUndo())) return false;

		auto current = captureDocumentSnapshot(building);
		if (!current) return false;
		try
		{
			shared_ptr<core::Building> loaded;
			auto restore = [&loaded](DocumentSnapshot const& target)
			{
				loaded = make_shared<core::Building>("Loading", 1, 1);
				auto serializer = core::YamlSerializer::fromString(target.yaml);
				serializer->deserialize();
				core::SerializationWorkData workData;
				if (!loaded->deserialize(*serializer, workData)) return false;
				core::loadAndAttachAgentTagRegistry(*loaded, gBuildingFilepath);
				core::loadAndAttachAgentBehaviourRegistry(*loaded, gBuildingFilepath);
				return true;
			};
			auto const restored = redo
				? gBuildingDocumentHistory.redo(std::move(current), restore)
				: gBuildingDocumentHistory.undo(std::move(current), restore);
			if (!restored) return false;
			if (gBuildingDocumentHistory.isModified()) loaded->markModified();

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
		return building && (building->isModified()
			|| gBuildingDocumentHistory.isModified()
			|| attachedAgentTagRegistryIsModified(building));
	}

	void reportFileError(string message)
	{
		gFileError = std::move(message);
		gOpenFileErrorPopup = true;
		core::addLogMessage("File", 0, core::LogLevel::Error, gFileError);
	}

	string currentAgentTagRegistryFilepath(
		shared_ptr<core::Building> const& building)
	{
		if (!building || !building->hasAttachedAgentTagRegistry()
			|| gBuildingFilepath.empty()) return {};
		return (filesystem::path(gBuildingFilepath).parent_path()
			/ building->getAgentTagRegistryFilename()).string();
	}

	BuildingDocumentSaveTarget currentDocumentSaveTarget(
		shared_ptr<core::Building> const& building, string buildingFilepath)
	{
		return { building, std::move(buildingFilepath),
			currentAgentTagRegistryFilepath(building), &gBuildingDocumentHistory };
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

		string diagnostic;
		if (!saveBuildingDocument(currentDocumentSaveTarget(building, filepath),
			&diagnostic))
		{
			reportFileError(std::move(diagnostic));
			return false;
		}
		gBuildingFilepath = std::move(filepath);
		return true;
	}

	bool saveAllOpenDocuments(shared_ptr<core::Building> const& building)
	{
		if (!building) return false;
		// An untitled Building still needs the ordinary Save location chooser.
		if (gBuildingFilepath.empty()) return saveBuilding(building, false);
		string diagnostic;
		if (!saveAllDocuments(
			{ currentDocumentSaveTarget(building, gBuildingFilepath) }, &diagnostic))
		{
			reportFileError(std::move(diagnostic));
			return false;
		}
		return true;
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
			auto loaded = core::loadBuildingDocument(normalized);
			auto previousRegistry = building && building->hasAttachedAgentTagRegistry()
				? building->getAgentTagRegistry() : nullptr;
			auto previousBehaviourRegistry = building
				&& building->hasAttachedAgentBehaviourRegistry()
				? building->getAgentBehaviourRegistry() : nullptr;
			building = std::move(loaded);
			if (previousRegistry) forgetAgentTagRegistryDocument(previousRegistry);
			if (previousBehaviourRegistry)
				forgetAgentBehaviourRegistryDocument(previousBehaviourRegistry);
			if (building->hasAttachedAgentTagRegistry())
				(void)agentTagRegistryDocumentHistory(building->getAgentTagRegistry());
			gBuildingFilepath = normalized;
			addRecentFile(gBuildingFilepath);
			clearDocumentState();
			gBuildingDocumentHistory.markSaved();
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

	optional<string> chooseAgentTagRegistryPath()
	{
		nfdu8char_t* selectedPathRaw{ nullptr };
		nfdu8filteritem_t const filters[] = { { "Agent tag registry", "yaml" } };
		filesystem::path const buildingPath(gBuildingFilepath);
		auto const directory = buildingPath.parent_path().string();
		auto const result = NFD_OpenDialogU8(&selectedPathRaw, filters, 1,
			directory.empty() ? nullptr : directory.c_str());
		unique_ptr<nfdu8char_t, decltype(&NFD_FreePathU8)> selectedPath(
			selectedPathRaw, NFD_FreePathU8);
		if (result == NFD_CANCEL) return nullopt;
		if (result == NFD_ERROR)
		{
			throw runtime_error(string("Could not choose an Agent tag registry: ")
				+ (NFD_GetError() ? NFD_GetError() : "unknown native dialog error"));
		}
		return string(selectedPath.get());
	}

	optional<string> chooseAgentBehaviourRegistryPath()
	{
		nfdu8char_t* selectedPathRaw{ nullptr };
		filesystem::path const buildingPath(gBuildingFilepath);
		auto const directory = buildingPath.parent_path().string();
		auto const result = NFD_PickFolderU8(
			&selectedPathRaw, directory.empty() ? nullptr : directory.c_str());
		unique_ptr<nfdu8char_t, decltype(&NFD_FreePathU8)> selectedPath(
			selectedPathRaw, NFD_FreePathU8);
		if (result == NFD_CANCEL) return nullopt;
		if (result == NFD_ERROR)
		{
			throw runtime_error(string("Could not choose an Agent behaviour registry package: ")
				+ (NFD_GetError() ? NFD_GetError() : "unknown native dialog error"));
		}
		return string(selectedPath.get());
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
			if (building && building->hasAttachedAgentTagRegistry())
				forgetAgentTagRegistryDocument(building->getAgentTagRegistry());
			if (building && building->hasAttachedAgentBehaviourRegistry())
				forgetAgentBehaviourRegistryDocument(building->getAgentBehaviourRegistry());
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

	void commitStairwellEdit(shared_ptr<core::Building> const& building,
		core::Building::StairwellEditPlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			auto newIndex = building->applyStairwellEdit(plan);
			gUISettings.worldPaused = true;
			gHoveredAgent = nullptr; gHoveredSector.reset(); gHoveredSectorObject.reset();
			gSelectedAgent = nullptr; gSelectedSectorObject.reset();
			gSelectedSector = plan.remove ? nullptr : building->getSector(newIndex);
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{ core::addLogMessage("Stairwell editor", 0, core::LogLevel::Error, error.getMessage()); }
		catch (std::exception const& error)
		{ core::addLogMessage("Stairwell editor", 0, core::LogLevel::Error, error.what()); }
		resetSectorResize();
	}

	void queueStairwellEdit(shared_ptr<core::Building> const& building,
		core::Building::StairwellEditPlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation())
		{
			gPendingStairwellEdit = plan;
			gOpenLocationEditPopup = true;
		}
		else commitStairwellEdit(building, plan);
	}

	void commitPlatformLiftEdit(shared_ptr<core::Building> const& building,
		core::Building::PlatformLiftEditPlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			gSelectedSectorObject = building->applyPlatformLiftEdit(plan);
			gHoveredSectorObject.reset();
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{ reportEditorError("PlatformLift editor", error.getMessage()); }
		catch (std::exception const& error)
		{ reportEditorError("PlatformLift editor", error.what()); }
	}

	void queuePlatformLiftEdit(shared_ptr<core::Building> const& building,
		core::Building::PlatformLiftEditPlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation()) { gPendingPlatformLiftEdit = plan; gOpenLocationEditPopup = true; }
		else commitPlatformLiftEdit(building, plan);
	}

	void commitWalkwayEdit(shared_ptr<core::Building> const& building,
		core::Building::WalkwayEditPlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			if (building->applyWalkwayEdit(plan))
			{
				gSelectedSectorObject.reset(); gHoveredSectorObject.reset();
				commitDocumentEdit(std::move(undo));
			}
		}
		catch (core::Exception const& error) { reportEditorError("Walkway editor", error.getMessage()); }
		catch (std::exception const& error) { reportEditorError("Walkway editor", error.what()); }
	}

	void queueWalkwayEdit(shared_ptr<core::Building> const& building,
		core::Building::WalkwayEditPlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation()) { gPendingWalkwayEdit = plan; gOpenLocationEditPopup = true; }
		else commitWalkwayEdit(building, plan);
	}

	void commitObjectMove(shared_ptr<core::Building> const& building,
		core::Building::ObjectMovePlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			gSelectedSectorObject = building->applyObjectMove(plan);
			gHoveredSectorObject.reset(); gSelectedSector.reset(); gSelectedAgent = nullptr;
			commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error) { reportEditorError("Object editor", error.getMessage()); }
		catch (std::exception const& error) { reportEditorError("Object editor", error.what()); }
		resetObjectMove();
	}

	void queueObjectMove(shared_ptr<core::Building> const& building,
		core::Building::ObjectMovePlan const& plan)
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;
		if (plan.requiresConfirmation()) { gPendingObjectMove = plan; gOpenLocationEditPopup = true; resetObjectMove(); }
		else commitObjectMove(building, plan);
	}

	void commitLayerDelete(shared_ptr<core::Building> const& building,
		core::Building::LayerDeletePlan const& plan)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			building->applyDeleteLayer(plan);

			// Selections and inline name edits can point at Sectors, Agents, and
			// Layers which no longer exist.
			gHoveredAgent = nullptr;
			gSelectedAgent = nullptr;
			gHoveredSector.reset();
			gSelectedSector.reset();
			gHoveredSectorObject.reset();
			gSelectedSectorObject.reset();
			gLayerNameEdits.clear();
			gUISettings.visibleLayer = std::clamp(gUISettings.visibleLayer, 0,
				static_cast<int>(building->getLayerCount()) - 1);
			commitDocumentEdit(std::move(undo));
			core::addLogMessage("Layers", 0, core::LogLevel::Info,
				format("Deleted {} and compacted the Layers behind it", plan.layerName));
		}
		catch (core::Exception const& error) { reportEditorError("Layers", error.getMessage()); }
		catch (std::exception const& error) { reportEditorError("Layers", error.what()); }
	}

	// Layer deletion is always destructive, so it is planned first and only applied
	// once the user confirms the consequence list.
	void requestLayerDelete(shared_ptr<core::Building> const& building, uint32_t layer)
	{
		if (!building) return;
		if (!building->isSimulationPaused()) building->pauseSimulation();
		gUISettings.worldPaused = true;

		auto const plan = building->planDeleteLayer(layer);
		if (!plan.valid)
		{
			reportEditorError("Layers", plan.diagnostic);
			return;
		}

		gPendingLayerDelete = plan;
		gOpenLocationEditPopup = true;
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
			auto const prompt = unsavedDocumentPromptText(
				currentDocumentSaveTarget(building, gBuildingFilepath));
			ImGui::TextUnformatted(prompt.c_str());
			if (ImGui::Button("Save All"))
			{
				if (saveAllOpenDocuments(building))
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
				if (building && building->hasAttachedAgentTagRegistry())
					forgetAgentTagRegistryDocument(building->getAgentTagRegistry());
				if (building && building->hasAttachedAgentBehaviourRegistry())
					forgetAgentBehaviourRegistryDocument(building->getAgentBehaviourRegistry());
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
				if (draft.carWidth >= 3 && draft.carWidth <= 5)
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
					// The track is drafted on the visible Layer; its carriage doors land
					// on the Layer directly in front.  Candidates are never offered on the
					// front-most Layer, so a landing Layer always exists here.
					auto const landing = static_cast<core::Building const&>(*building)
						.getLayer(core::layerInFront(gUISettings.visibleLayer));
					uint32_t selectedDoors = 0;
					for (int cell = 0; cell < draft.carWidth; ++cell)
						selectedDoors += (draft.doorMask & (1u << cell)) != 0;
					for (int car = 0; car < draft.numCars; ++car)
						for (int doorOffset = 0; doorOffset < draft.carWidth; ++doorOffset)
						{
							if ((draft.doorMask & (1u << doorOffset)) == 0) continue;
							auto doorX = draft.x + offset + car * (draft.carWidth + 1) + doorOffset;
							coverage += landing->getCellDefinition(doorX, draft.y).sectorIndex != ~0u;
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
						auto created = building->addShuttle(gUISettings.visibleLayer, draft.y, draft.x, draft.cellsWide, options);
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
			ImGui::TextUnformatted(gPendingLayerDelete
				? "Deleting this Layer will:"
				: "This edit will also:");
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
			if (gPendingStairwellEdit)
				for (auto const& consequence : gPendingStairwellEdit->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			if (gPendingPlatformLiftEdit)
				for (auto const& consequence : gPendingPlatformLiftEdit->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			if (gPendingWalkwayEdit)
				for (auto const& consequence : gPendingWalkwayEdit->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			if (gPendingObjectMove)
				for (auto const& consequence : gPendingObjectMove->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			if (gPendingLayerDelete)
				for (auto const& consequence : gPendingLayerDelete->consequences)
					ImGui::BulletText("%s", consequence.c_str());
			ImGui::Separator();
			if (ImGui::Button("OK") && building
				&& (gPendingLocationEdit || gPendingLiftEdit || gPendingShuttleEdit
					|| gPendingLadderEdit || gPendingStairwellEdit || gPendingPlatformLiftEdit
					|| gPendingWalkwayEdit || gPendingObjectMove || gPendingLayerDelete))
			{
				auto locationPlan = gPendingLocationEdit;
				auto liftPlan = gPendingLiftEdit;
				auto shuttlePlan = gPendingShuttleEdit;
				auto ladderPlan = gPendingLadderEdit;
				auto stairwellPlan = gPendingStairwellEdit;
				auto platformLiftPlan = gPendingPlatformLiftEdit;
				auto walkwayPlan = gPendingWalkwayEdit;
				auto objectMove = gPendingObjectMove;
				auto layerDelete = gPendingLayerDelete;
				gPendingLocationEdit.reset(); gPendingLiftEdit.reset(); gPendingShuttleEdit.reset();
				gPendingLadderEdit.reset(); gPendingStairwellEdit.reset();
				gPendingPlatformLiftEdit.reset(); gPendingWalkwayEdit.reset(); gPendingObjectMove.reset();
				gPendingLayerDelete.reset();
				ImGui::CloseCurrentPopup();
				if (locationPlan) commitLocationEdit(building, *locationPlan);
				else if (liftPlan) commitLiftEdit(building, *liftPlan);
				else if (shuttlePlan) commitShuttleEdit(building, *shuttlePlan);
				else if (ladderPlan) commitLadderEdit(building, *ladderPlan);
				else if (stairwellPlan) commitStairwellEdit(building, *stairwellPlan);
				else if (platformLiftPlan) commitPlatformLiftEdit(building, *platformLiftPlan);
				else if (walkwayPlan) commitWalkwayEdit(building, *walkwayPlan);
				else if (objectMove) commitObjectMove(building, *objectMove);
				else if (layerDelete) commitLayerDelete(building, *layerDelete);
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
			{
				gPendingLocationEdit.reset();
				gPendingLiftEdit.reset();
				gPendingShuttleEdit.reset();
				gPendingLadderEdit.reset();
				gPendingStairwellEdit.reset();
				gPendingPlatformLiftEdit.reset();
				gPendingWalkwayEdit.reset();
				gPendingObjectMove.reset();
				gPendingLayerDelete.reset();
				resetSectorResize();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	enum class ClipboardObjectType { Agent, Door, BulkheadDoor, Window, Marker, Walkway, ForceBridge, RoomLadder, PlatformLift };
	struct ClipboardDefinition
	{
		ClipboardObjectType type{};
		bool cut{ false };
		AgentClipboardPayload agent;
		core::Building::CreateDoorOptions door;
		core::Building::CreateBulkheadDoorOptions bulkheadDoor;
		core::Building::CreateWindowOptions window;
		core::Building::CreateForceBridgeOptions forceBridge{ 1, CORE_SIDE_LEFT, true, true, 1 };
		core::Building::CreateLadderOptions ladder{ 0, false, true };
		core::Building::CreateLiftOptions platformLift;
		uint32_t width{ 1 }, height{ 1 };
	};

	optional<core::Vector2> gLastWorldCursor;
	string gClipboardError;
	double gClipboardErrorUntil{ 0.0 };
	string gConsumedCutClipboard;

	void reportEditorError(string const& source, string message)
	{
		gClipboardError = std::move(message);
		gClipboardErrorUntil = ImGui::GetTime() + 3.0;
		core::addLogMessage(source, 0, core::LogLevel::Error, gClipboardError);
	}

	void reportClipboardError(string message)
	{
		reportEditorError("Clipboard", std::move(message));
	}

	bool hasClipboardSelection()
	{
		if (gUISettings.selectionMode != UISettings::SelectionMode::Object) return false;
		if (gSelectedAgent) return true;
		if (!gSelectedSectorObject) return false;
		auto type = gSelectedSectorObject->getObjectType();
		return type == core::SectorObjectType::Door || type == core::SectorObjectType::BulkheadDoor
			|| type == core::SectorObjectType::Window
			|| type == core::SectorObjectType::Marker || type == core::SectorObjectType::Walkway
			|| type == core::SectorObjectType::ForceBridge || type == core::SectorObjectType::Ladder
			|| type == core::SectorObjectType::Lift;
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

	char const* doorOpenStyleName(core::Door::OpenStyle style)
	{
		switch (style)
		{
		case core::Door::OpenStyle::OpenUp: return "OpenUp";
		case core::Door::OpenStyle::OpenLeft: return "OpenLeft";
		case core::Door::OpenStyle::OpenRight: return "OpenRight";
		case core::Door::OpenStyle::OpenApart: return "OpenApart";
		}
		return "OpenUp";
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
			// The Agent group crosses the clipboard by name, never by its
			// Building-local AgentGroupId: the next Building has never issued
			// that ID and could not honour it (ADR 0006).
			return makeAgentClipboardText(
				makeAgentClipboardPayload(*building,
					building->getAgentId(gSelectedAgent), name), cut);
		}
		else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Door)
		{
			auto door = static_pointer_cast<const core::DoorSectorObject>(gSelectedSectorObject)->getDoor();
			core::Building::CreateDoorOptions options;
			if (!building->getSectorDoorOptions(door->getFrontSector()->getLayerIndex(),
				gSelectedSectorObject->getCellY(), gSelectedSectorObject->getCellX(),
				door->getCellsWide(), options))
				throw runtime_error("The selected Door has no authored definition");
			output << YAML::Key << "type" << YAML::Value << "Door"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
				<< YAML::Key << "width" << YAML::Value << options.width
				<< YAML::Key << "height" << YAML::Value
				<< (options.height == core::Door::Height::Tall ? "Tall" : "Regular")
				<< YAML::Key << "controls" << YAML::Value << YAML::Flow << YAML::BeginSeq
				<< options.controls[0] << options.controls[1] << YAML::EndSeq
				<< YAML::Key << "activationMode" << YAML::Value << activationModeName(options.activationMode)
				<< YAML::Key << "holdOpenSeconds" << YAML::Value << options.holdOpenSeconds
				<< YAML::Key << "crossingLanes" << YAML::Value << options.crossingLanes
				<< YAML::Key << "openStyle" << YAML::Value << doorOpenStyleName(options.openStyle)
				<< YAML::EndMap;
		}
		else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::BulkheadDoor)
		{
			auto owner = gSelectedSectorObject->getSector();
			uint32_t index = ~0u;
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == gSelectedSectorObject) { index = i; break; }
			core::Building::CreateBulkheadDoorOptions options;
			if (index == ~0u || !building->getSectorBulkheadDoorOptions(owner->getIndex(), index, options))
				throw runtime_error("The selected Bulkhead Door has no authored definition");
			output << YAML::Key << "type" << YAML::Value << "BulkheadDoor"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
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
			// A Window is authored on the front Layer of the pair it crosses, which is
			// the Layer its definition is recorded against.
			if (!building->getSectorWindowOptions(window->getFrontLayer(),
				gSelectedSectorObject->getCellY(), gSelectedSectorObject->getCellX(),
				window->getCellsWide(), window->getDecksHigh(), options))
				throw runtime_error("The selected Window has no authored definition");
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
		else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::ForceBridge)
		{
			auto owner = gSelectedSectorObject->getSector();
			uint32_t index = ~0u;
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == gSelectedSectorObject) { index = i; break; }
			core::Building::CreateForceBridgeOptions options;
			if (index == ~0u || !building->getSectorForceBridgeOptions(owner->getIndex(), index, options))
				throw runtime_error("The selected Force Bridge has no authored definition");
			output << YAML::Key << "type" << YAML::Value << "ForceBridge"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
				<< YAML::Key << "width" << YAML::Value << options.width
				<< YAML::Key << "fromSide" << YAML::Value
				<< (options.fromSide == CORE_SIDE_LEFT ? "left" : "right")
				<< YAML::Key << "extensible" << YAML::Value << options.extensible
				<< YAML::Key << "startExtended" << YAML::Value << options.startExtended
				<< YAML::Key << "controlCount" << YAML::Value << options.controlCount
				<< YAML::EndMap;
		}
		else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Lift)
		{
			auto owner = gSelectedSectorObject->getSector();
			uint32_t index = ~0u;
			for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
				if (owner->getObject(i) == gSelectedSectorObject) { index = i; break; }
			core::Building::CreateLiftOptions options;
			if (index == ~0u || !building->getPlatformLiftOptions(owner->getIndex(), index, options))
				throw runtime_error("The selected PlatformLift has no authored definition");
			output << YAML::Key << "type" << YAML::Value << "PlatformLift"
				<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
				<< YAML::Key << "capacity" << YAML::Value << options.capacity
				<< YAML::Key << "stopDurationSeconds" << YAML::Value
				<< options.platformStopDurationSeconds
				<< YAML::EndMap;
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
			string diagnostic;
			if (!readAgentClipboardObject(object, definition.agent, diagnostic))
				throw runtime_error(diagnostic);
		}
		else if (type == "Door")
		{
			definition.type = ClipboardObjectType::Door;
			definition.door.width = requiredYaml<uint32_t>(object, "width");
			if (object["height"])
			{
				auto const height = object["height"].as<string>();
				if (height == "Regular") definition.door.height = core::Door::Height::Regular;
				else if (height == "Tall") definition.door.height = core::Door::Height::Tall;
				else throw runtime_error("Door height is invalid");
			}
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
			// A clipboard entry written before opening styles existed carries no style;
			// it pastes as OpenUp, the only style those builds could ever show.
			if (object["openStyle"]) {
				auto style = object["openStyle"].as<std::string>();
				if (style == "OpenUp") definition.door.openStyle = core::Door::OpenStyle::OpenUp;
				else if (style == "OpenLeft") definition.door.openStyle = core::Door::OpenStyle::OpenLeft;
				else if (style == "OpenRight") definition.door.openStyle = core::Door::OpenStyle::OpenRight;
				else if (style == "OpenApart") definition.door.openStyle = core::Door::OpenStyle::OpenApart;
				else throw runtime_error("Door openStyle is invalid");
			}
			else definition.door.openStyle = core::Door::OpenStyle::OpenUp;
		}
		else if (type == "BulkheadDoor")
		{
			definition.type = ClipboardObjectType::BulkheadDoor;
			auto controls = object["controls"];
			if (!controls || !controls.IsSequence() || controls.size() != 2)
				throw runtime_error("Bulkhead Door controls must contain two values");
			definition.bulkheadDoor.controls[0] = controls[0].as<bool>();
			definition.bulkheadDoor.controls[1] = controls[1].as<bool>();
			auto mode = requiredYaml<string>(object, "activationMode");
			if (mode == "Automatic") definition.bulkheadDoor.activationMode = core::DoorActivationMode::Automatic;
			else if (mode == "Manual") definition.bulkheadDoor.activationMode = core::DoorActivationMode::Manual;
			else if (mode == "RemoteControlled") definition.bulkheadDoor.activationMode = core::DoorActivationMode::RemoteControlled;
			else if (mode == "Unavailable") definition.bulkheadDoor.activationMode = core::DoorActivationMode::Unavailable;
			else throw runtime_error("Bulkhead Door activationMode is invalid");
			definition.bulkheadDoor.holdOpenSeconds = requiredYaml<float>(object, "holdOpenSeconds");
			definition.bulkheadDoor.crossingLanes = requiredYaml<uint32_t>(object, "crossingLanes");
			if (definition.bulkheadDoor.holdOpenSeconds < 0.0f)
				throw runtime_error("Bulkhead Door holdOpenSeconds cannot be negative");
			if (definition.bulkheadDoor.crossingLanes != 1)
				throw runtime_error("Bulkhead Door crossingLanes must be one");
			if (definition.bulkheadDoor.activationMode != core::DoorActivationMode::RemoteControlled
				&& (definition.bulkheadDoor.controls[0] || definition.bulkheadDoor.controls[1]))
				throw runtime_error("Bulkhead Door controls require RemoteControlled activation");
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
		else if (type == "ForceBridge")
		{
			definition.type = ClipboardObjectType::ForceBridge;
			definition.forceBridge.width = requiredYaml<uint32_t>(object, "width");
			auto side = requiredYaml<string>(object, "fromSide");
			if (side == "left") definition.forceBridge.fromSide = CORE_SIDE_LEFT;
			else if (side == "right") definition.forceBridge.fromSide = CORE_SIDE_RIGHT;
			else throw runtime_error("Force Bridge fromSide is invalid");
			definition.forceBridge.extensible = requiredYaml<bool>(object, "extensible");
			definition.forceBridge.startExtended = requiredYaml<bool>(object, "startExtended");
			definition.forceBridge.controlCount = requiredYaml<uint32_t>(object, "controlCount");
			if (definition.forceBridge.width == 0
				|| definition.forceBridge.width > CORE_FORCEBRIDGE_MAX_SIZE)
				throw runtime_error("Force Bridge width is invalid");
			if (definition.forceBridge.extensible
				&& (definition.forceBridge.controlCount < 1 || definition.forceBridge.controlCount > 2))
				throw runtime_error("An extensible Force Bridge requires one or two controls");
			if (!definition.forceBridge.extensible
				&& (definition.forceBridge.controlCount != 0 || !definition.forceBridge.startExtended))
				throw runtime_error("A non-extensible Force Bridge must be permanently extended and have no controls");
		}
		else if (type == "PlatformLift")
		{
			definition.type = ClipboardObjectType::PlatformLift;
			definition.platformLift.cellsWide = 1;
			definition.platformLift.capacity = requiredYaml<uint32_t>(object, "capacity");
			if (object["stopDurationSeconds"])
				definition.platformLift.platformStopDurationSeconds = requiredYaml<float>(
					object, "stopDurationSeconds");
			else
				// Legacy clipboard payloads had separate timings. Their maximum boarding
				// window is the closest equivalent to the new fixed stop duration.
				definition.platformLift.platformStopDurationSeconds = requiredYaml<float>(
					object, "maximumBoardingSeconds");
			if (definition.platformLift.platformStopDurationSeconds < 0.0f)
				throw runtime_error("PlatformLift stopDurationSeconds cannot be negative");
		}
		else if (type == "RoomLadder")
		{
			definition.type = ClipboardObjectType::RoomLadder;
			definition.ladder.extensible = requiredYaml<bool>(object, "extensible");
			definition.ladder.startExtended = requiredYaml<bool>(object, "startExtended");
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
			// Ticket #113: the cut takes the Agent and nothing else. Its Agent
			// group stays defined behind it, which is what lets the clipboard
			// payload it just wrote name a group the source Building still has.
			string diagnostic;
			if (!cutAgent(building, id, diagnostic))
			{
				reportEditorError("Agent editor", diagnostic);
				return false;
			}
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
			bool removed;
			if (type == core::SectorObjectType::Lift)
			{
				building->applyPlatformLiftEdit(building->planRemovePlatformLift(sector->getIndex(), i));
				removed = true;
			}
			else if (type == core::SectorObjectType::Marker)
			{
				string diagnostic;
				removed = building->removeSectorMarker(sector->getIndex(), i, &diagnostic);
				if (!removed && !diagnostic.empty()) reportEditorError("Marker editor", diagnostic);
			}
			else removed = type == core::SectorObjectType::Door
					? building->removeSectorDoor(sector->getIndex(), i)
					: type == core::SectorObjectType::BulkheadDoor
						? building->removeSectorBulkheadDoor(sector->getIndex(), i)
						: type == core::SectorObjectType::Window
						? building->removeSectorWindow(sector->getIndex(), i)
						: type == core::SectorObjectType::Ladder
							? building->removeRoomLadder(sector->getIndex(), i)
							: type == core::SectorObjectType::ForceBridge
								? building->removeSectorForceBridge(sector->getIndex(), i)
								: building->removeSectorWalkway(sector->getIndex(), i);
			if (!removed)
			{
				reportEditorError("Object editor", format("{} could not be deleted",
					selected->getDescription()));
				return false;
			}
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
			|| gPendingLadderEdit || gPendingStairwellEdit || gPendingPlatformLiftEdit
			|| gPendingWalkwayEdit || gPendingObjectMove || gPendingLayerDelete
			|| gShuttleDraft || !gShuttleDoorCandidates.empty())
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
				auto payload = definition.agent;
				if (definition.cut && !consumedCut)
				{
					auto unique = uniqueAgentName(building, payload.name);
					if (unique != payload.name) throw runtime_error("An Agent with this name already exists");
				}
				else payload.name = uniqueAgentName(building,
					consumedCut ? payload.name + " copy" : payload.name);
				float halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
				float localX = clamp(world.x - sector->getPosition().x, halfWidth,
					max(halfWidth, sector->getSize().x - halfWidth));
				// Arming judges the Agent group and Agent tag registry identity before
				// anything is deferred, so an unusable payload is refused while the
				// cursor is still where the user put it and the simulation is still as
				// they left it. Nothing is written by arming: the Agent, group and tag
				// assignments only exist if the fall is allowed to land.
				string diagnostic;
				if (!armAgentPlacement(gPegman.pastedAgent, *building, payload, sector,
					y - sector->getCellY(), localX, diagnostic))
					throw runtime_error(diagnostic);
				if (!building->isSimulationPaused()) building->pauseSimulation();
				gUISettings.worldPaused = true;
				gPegman.phase = PalettePhase::Falling;
				gPegman.item = PaletteItem::Agent;
				gPegman.sector = sector;
				gPegman.deckOffset = y - sector->getCellY();
				gPegman.localX = localX;
				gPegman.feetY = world.y;
				gPegman.floorY = static_cast<float>(y);
				gPegman.velocity = 0.0f;
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
				if (building->getLiftLandingGeometry(gUISettings.visibleLayer + 1, y, x, landingX, landingWidth))
				{
					x = landingX;
					definition.door = {};
					definition.door.width = landingWidth;
				}
				if (!building->canAddCorridorDoor(gUISettings.visibleLayer, y, x, definition.door, &diagnostic))
					throw runtime_error(diagnostic);
			}
			else if (definition.type == ClipboardObjectType::BulkheadDoor)
			{
				if (!building->canAddSectorBulkheadDoor(gUISettings.visibleLayer, y, x,
					CORE_SIDE_LEFT, definition.bulkheadDoor, &diagnostic))
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
			else if (definition.type == ClipboardObjectType::ForceBridge)
			{
				markerSector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
				auto room = dynamic_pointer_cast<const core::Location>(markerSector);
				if (!room || room->isCorridor() || !markerSector->pointInBounds(world.x, world.y))
					throw runtime_error("Force Bridges can only be placed in Rooms");
				if (!building->canAddSectorForceBridge(markerSector->getIndex(),
					y - markerSector->getCellY(), x - markerSector->getCellX(),
					definition.forceBridge, &diagnostic)) throw runtime_error(diagnostic);
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
			else if (definition.type == ClipboardObjectType::PlatformLift)
			{
				markerSector = building->getSectorAtPosition(gUISettings.visibleLayer, world.x, world.y);
				auto room = dynamic_pointer_cast<const core::Location>(markerSector);
				if (!room || room->isCorridor() || !markerSector->pointInBounds(world.x, world.y))
					throw runtime_error("PlatformLifts can only be placed in Rooms");
				auto xOffset = x - room->getCellX();
				bool valid = false;
				for (auto const& candidate : building->getPlatformLiftStopCandidates(room->getIndex(), xOffset))
				{
					definition.platformLift.stopOffsets = { 0, candidate.deckOffset };
					if (building->canAddPlatformLift(room->getIndex(), xOffset,
						definition.platformLift, &diagnostic)) { valid = true; break; }
				}
				if (!valid) throw runtime_error(diagnostic.empty()
					? "No eligible Walkway exists above this PlatformLift position" : diagnostic);
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
					auto result = building->addSectorDoor(gUISettings.visibleLayer, y, x, definition.door);
					created = result.door.sector->getObject(result.door.index);
				}
				else if (definition.type == ClipboardObjectType::BulkheadDoor)
				{
					auto result = building->addSectorBulkheadDoor(gUISettings.visibleLayer,
						y, x, CORE_SIDE_LEFT, definition.bulkheadDoor);
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
				else if (definition.type == ClipboardObjectType::ForceBridge)
				{
					auto result = building->addSectorForceBridge(markerSector->getIndex(),
						y - markerSector->getCellY(), x - markerSector->getCellX(),
						definition.forceBridge);
					created = result.forceBridge.sector->getObject(result.forceBridge.index);
				}
				else if (definition.type == ClipboardObjectType::RoomLadder)
				{
					auto result = building->addRoomLadder(markerSector->getIndex(),
						y - markerSector->getCellY(), x - markerSector->getCellX(), definition.ladder);
					created = result.ladder.sector->getObject(result.ladder.index);
				}
				else if (definition.type == ClipboardObjectType::PlatformLift)
				{
					auto result = building->addSectorPlatformLift(markerSector->getIndex(), 0,
						x - markerSector->getCellX(), definition.platformLift);
					created = result.lift.sector->getObject(result.lift.index);
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

void resetWorldSimulation(shared_ptr<core::Building> const& building)
{
	try
	{
		building->resetSimulation();
		clearDocumentState(false);
		gUISettings.worldPaused = building->isSimulationPaused();
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Simulation", 0, core::LogLevel::Error,
			"Could not reset simulation: " + string(error.what()));
	}
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
		beginAgentPathSelection(building);
	}

	// Start or stop the simulation
	if (ImGui::Shortcut(ImGuiKey_S, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused())
		{
			setWorldPaused(building, !gUISettings.worldPaused);
		}
	}

	// Reset simulation
	if (ImGui::Shortcut(ImGuiKey_R, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused())
		{
			resetWorldSimulation(building);
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
				else if (gSelectedSector->getType() == core::SectorType::Stairwell)
				{
					auto plan = building->planRemoveStairwell(gSelectedSector->getIndex());
					if (!plan.valid) core::addLogMessage("Stairwell editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueStairwellEdit(building, plan);
				}
				else if (gSelectedSector->getType() == core::SectorType::Staircase)
				{
					auto plan = building->planRemoveStaircase(gSelectedSector->getIndex());
					if (!plan.valid) core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, plan.diagnostic);
					else try
					{
						auto undo = captureDocumentSnapshot(building);
						if (!building->isSimulationPaused()) building->pauseSimulation();
						building->applyStaircaseEdit(plan); commitDocumentEdit(std::move(undo)); clearSelections();
					}
					catch (std::exception const& error) { core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, error.what()); }
				}
				else if (gSelectedSector->getType() == core::SectorType::Background)
				{
					// A Background exists to be looked into, so deleting it takes the
					// Windows looking into it with it.  The plan names them and the shared
					// confirmation popup spells the cascade out before anything is applied.
					auto plan = building->planRemoveBackground(gSelectedSector->getIndex());
					if (!plan.valid)
						core::addLogMessage("Background editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueLocationEdit(building, plan);
				}
				else if (gSelectedSector->getType() == core::SectorType::Facade)
				{
					// A Facade is occupiable like a Room, so the plan names the Agents
					// inside and the hosted objects which go with it before anything
					// is applied (ticket #53).
					auto plan = building->planRemoveFacade(gSelectedSector->getIndex());
					if (!plan.valid)
						core::addLogMessage("Facade editor", 0, core::LogLevel::Error, plan.diagnostic);
					else queueLocationEdit(building, plan);
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
					auto const removal = building->removeAgent(id);
					if (removal.removed)
					{
						if (gHoveredAgent == selected) gHoveredAgent = nullptr;
						gSelectedAgent = nullptr;
						commitDocumentEdit(std::move(undo));
					}
					else
					{
						// Ticket #57: a refusal is only useful if the user hears it.
						reportEditorError("Agent editor", removal.diagnostic);
					}
				}
			}
			else if (gSelectedSectorObject
				&& (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Marker
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Door
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::BulkheadDoor
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Window
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Walkway
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::ForceBridge
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Ladder
					|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::Lift))
			{
				uint32_t liftIndex, stopIndex;
				if (building->isLiftOwnedDoor(gSelectedSectorObject, &liftIndex, &stopIndex))
				{
					auto plan = building->planRemoveLiftStop(liftIndex, stopIndex);
					if (!plan.valid) reportEditorError("Lift editor", plan.diagnostic);
					else queueLiftEdit(building, plan);
				}
				else if (building->isShuttleOwnedDoor(gSelectedSectorObject, &liftIndex, &stopIndex))
				{
					auto plan = building->planRemoveShuttleStop(liftIndex, stopIndex);
					if (!plan.valid) reportEditorError("Shuttle editor", plan.diagnostic);
					else queueShuttleEdit(building, plan);
				}
				else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Lift)
				{
					auto room = gSelectedSectorObject->getSector();
					uint32_t index = ~0u;
					for (uint32_t i = 0; i < room->getNumObjects(); ++i)
						if (room->getObject(i) == gSelectedSectorObject) { index = i; break; }
					auto plan = building->planRemovePlatformLift(room->getIndex(), index);
					if (!plan.valid) reportEditorError("PlatformLift editor", plan.diagnostic);
					else queuePlatformLiftEdit(building, plan);
				}
				else if (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Walkway)
				{
					auto room = gSelectedSectorObject->getSector();
					uint32_t index = ~0u;
					for (uint32_t i = 0; i < room->getNumObjects(); ++i)
						if (room->getObject(i) == gSelectedSectorObject) { index = i; break; }
					auto plan = building->planRemoveSectorWalkway(room->getIndex(), index);
					if (!plan.valid) reportEditorError("Walkway editor", plan.diagnostic);
					else queueWalkwayEdit(building, plan);
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
						bool removed{ false };
						if (type == core::SectorObjectType::Marker)
						{
							string diagnostic;
							removed = building->removeSectorMarker(sector->getIndex(), i, &diagnostic);
							if (!removed && !diagnostic.empty())
								reportEditorError("Marker editor", diagnostic);
						}
						else removed = type == core::SectorObjectType::Door
								? building->removeSectorDoor(sector->getIndex(), i)
								: type == core::SectorObjectType::BulkheadDoor
									? building->removeSectorBulkheadDoor(sector->getIndex(), i)
									: type == core::SectorObjectType::Window
									? building->removeSectorWindow(sector->getIndex(), i)
									: type == core::SectorObjectType::Ladder
										? building->removeRoomLadder(sector->getIndex(), i)
										: type == core::SectorObjectType::ForceBridge
											? building->removeSectorForceBridge(sector->getIndex(), i)
											: building->removeSectorWalkway(sector->getIndex(), i);
						if (!removed)
							reportEditorError("Object editor", format("{} could not be deleted",
								selected->getDescription()));
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
					reportEditorError("Object editor", error.getMessage());
				}
				catch (std::exception const& error)
				{
					reportEditorError("Object editor", error.what());
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

	// View
	if (ImGui::Shortcut(ImGuiKey_F2, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		if (building)
		{
			auto const layerCount = building->getLayerCount();
			auto const current = static_cast<uint32_t>(std::clamp(gUISettings.visibleLayer, 0,
				static_cast<int>(layerCount) - 1));
			gUISettings.visibleLayer = static_cast<int>((current + 1) % layerCount);
		}
	}
	if (ImGui::Shortcut(ImGuiKey_F3, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		gUISettings.renderNextLayerWireframe = !gUISettings.renderNextLayerWireframe;
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
					reportEditorError("Agent path",
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
			if (ImGui::MenuItem("Save All", nullptr, false, isDocumentStale(building)))
				saveAllOpenDocuments(building);
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
				building != nullptr && gBuildingDocumentHistory.canUndo()))
				restoreDocumentSnapshot(building, false);
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false,
				building != nullptr && gBuildingDocumentHistory.canRedo()))
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
				if (ImGui::MenuItem("Sectors", 0, &selected) && selected)
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
			ImGui::MenuItem("Show next layer wireframe", "F3", &gUISettings.renderNextLayerWireframe);
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
	ImGui::BeginDisabled(building == nullptr || !gBuildingDocumentHistory.canUndo());
	if (ImGui::Button(ICON_FA_UNDO "##Undo")) restoreDocumentSnapshot(building, false);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo (Ctrl+Z)");
	ImGui::EndDisabled();

	ImGui::SameLine();
	ImGui::BeginDisabled(building == nullptr || !gBuildingDocumentHistory.canRedo());
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
	imgui::ToggleButton("ToggleNextLayerWireframe", "Next layer wireframe", &gUISettings.renderNextLayerWireframe);
	ImGui::SameLine();
	imgui::ToggleButton("ToggleGraph", "Building graph", &gUISettings.renderGraph);
	ImGui::SameLine();
	imgui::ToggleButton("Agent Debug", "Agent debug", &gUISettings.renderAgentDebug);

	ImGui::End();
}


void renderToolbar(shared_ptr<core::Building> building)
{
	if (ImGui::Button(gUISettings.worldPaused ? "Start" : "Stop"))
	{
		setWorldPaused(building, !gUISettings.worldPaused);
	}
	if (ImGui::IsItemHovered())
	{
		ImGui::SetTooltip(gUISettings.worldPaused ? "Start (S)" : "Stop (S)");
	}

		ImGui::SameLine();
		if (ImGui::Button("Reset"))
		{
			resetWorldSimulation(building);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset (R)");


		// Select visible Layer
		auto const layerCount = building ? building->getLayerCount() : 0u;

		string layersStr;

		for (uint32_t layer = 0; layer < layerCount; ++layer)
		{
			layersStr += layerLabel(building, layer);
			layersStr += '\0';
		}

		ImGui::SetNextItemWidth(128);

		if (layerCount > 0)
		{
			auto const highest = static_cast<int>(layerCount) - 1;
			gUISettings.visibleLayer = std::clamp(gUISettings.visibleLayer, 0, highest);

			auto current = gUISettings.visibleLayer;
			if (ImGui::Combo("Visible Layer", &current, layersStr.c_str(), highest + 1))
				gUISettings.visibleLayer = std::clamp(current, 0, highest);
		}

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


void renderStatusBar(shared_ptr<const core::Building> const& building)
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
			auto const shownLayer = static_cast<uint32_t>(max(gUISettings.visibleLayer, 0));
			ImGui::Text("Layer: %s", layerLabel(building, shownLayer).c_str());

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


void renderMarkerPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	auto marker = static_pointer_cast<const core::MarkerSectorObject>(object)->getMarker();
	static core::MarkerId editingId{};
	static array<char, core::Marker::MaxNameBytes + 1> name{};
	if (editingId != marker->getId())
	{
		editingId = marker->getId();
		std::strncpy(name.data(), marker->getName().c_str(), name.size() - 1);
		name.back() = '\0';
	}

	ImGui::TextUnformatted("Marker");
	ImGui::SetNextItemWidth(-1.0f);
	bool submitted = ImGui::InputText("Name", name.data(), name.size(),
		ImGuiInputTextFlags_EnterReturnsTrue);
	if (submitted || ImGui::IsItemDeactivatedAfterEdit())
	{
		auto const trimmed = core::Marker::trimName(name.data());
		if (trimmed != marker->getName())
		{
			auto undo = captureDocumentSnapshot(building);
			string diagnostic;
			if (building->renameMarker(marker->getId(), name.data(), &diagnostic))
				commitDocumentEdit(std::move(undo));
			else reportEditorError("Marker editor", diagnostic);
		}
		std::strncpy(name.data(), marker->getName().c_str(), name.size() - 1);
		name.back() = '\0';
	}
	auto position = marker->getPosition();
	position.x += marker->getOffset();
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
	ImGui::Text("Layer: %s", layerLabel(building, room->getLayerIndex()).c_str());
	ImGui::Text("Position: %u, %u", walkway->getCellX(), walkway->getCellY());
	ImGui::Text("Deck offset: %u", walkway->getCellY() - room->getCellY());
	ImGui::Separator();
	if (ImGui::Button("Delete Walkway"))
	{
		uint32_t objectIndex = ~0u;
		for (uint32_t i = 0; i < room->getNumObjects(); ++i)
			if (room->getObject(i) == object) { objectIndex = i; break; }
		auto plan = building->planRemoveSectorWalkway(room->getIndex(), objectIndex);
		if (!plan.valid) reportEditorError("Walkway editor", plan.diagnostic);
		else queueWalkwayEdit(building, plan);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Delete key");
}


void renderWindowPanel(shared_ptr<const core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
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
	ImGui::Text("Layer: %s", layerLabel(building, owner->getLayerIndex()).c_str());
	ImGui::Text("Sector: %s", owner->getDescription().c_str());
	for (uint32_t pairSide = 0; pairSide < 2; ++pairSide)
		if (auto sector = window->getSector(pairSide); sector && sector != owner)
			ImGui::Text("Connected sector: %s", sector->getDescription().c_str());
}


// The Selection panel's Background branch, for ticket #39.
//
// A Background owns exactly one editable property: its colour. The panel reports
// what it is and where it is, offers a ColorEdit3 with no alpha control - a
// Background is always fully opaque - and a Delete which fires the cascade from
// #33 through the shared confirmation popup.
//
// There is deliberately no "Agents:" line. A Background hosts no agent, and a
// permanently-zero readout reads as a broken panel rather than an empty one. No
// wall editor, no capacity, no lights: none of those mean anything here.
void renderBackgroundPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::Sector> const& sector)
{
	auto const background = dynamic_pointer_cast<const core::Background>(sector);
	if (!background) return;

	ImGui::Text("Background: %s", background->getName().c_str());
	ImGui::Text("Sector index: %u", background->getIndex());
	ImGui::Text("Layer: %s", layerLabel(building, background->getLayerIndex()).c_str());
	ImGui::Text("Position: %u, %u", background->getCellX(), background->getCellY());
	ImGui::Text("Size: %u x %u cells", background->getCellsWide(), background->getDecksHigh());

	ImGui::Separator();

	// The colour lives on the Background; the float triple is the widget's working
	// copy, re-synced whenever the selection changes so a live drag never fights
	// the value it is driving.
	static core::Building const* editedBuilding = nullptr;
	static core::Sector const* editedSector = nullptr;
	static float rgb[3] = { 0.0f, 0.0f, 0.0f };
	static optional<core::BackgroundColour> colourBeforeEdit;
	static optional<DocumentSnapshot> pendingColourUndo;
	static bool colourEditInFlight = false;

	if (editedBuilding != building.get() || editedSector != sector.get())
	{
		editedBuilding = building.get();
		editedSector = sector.get();
		// Any half-finished edit belongs to whatever was selected before, not to
		// this Background, so it is dropped rather than carried across.
		colourBeforeEdit.reset();
		pendingColourUndo.reset();
		colourEditInFlight = false;
		core::backgroundColourToFloats(background->getColour(), rgb);
	}

	auto applyColour = [&](core::BackgroundColour const& colour)
	{
		string diagnostic;
		if (building->setBackgroundColour(sector->getIndex(), colour, &diagnostic)) return true;
		core::addLogMessage("Background editor", 0, core::LogLevel::Error, diagnostic);
		editedSector = nullptr;
		return false;
	};

	// No alpha control: the widget is told so explicitly rather than left to the
	// default, because a Background which could be translucent would be a
	// different domain object, not a recolour of this one.
	bool const changed = ImGui::ColorEdit3("Colour", rgb, ImGuiColorEditFlags_NoAlpha);
	bool const finished = ImGui::IsItemDeactivatedAfterEdit();
	bool const cancelled = ImGui::IsItemDeactivated() && !finished;

	if (changed)
	{
		// The undo snapshot is taken before the first live change so the entry
		// restores the colour the panel was opened with, and is committed only when
		// the edit finishes: one undo per recolour, not one per frame.
		if (!colourEditInFlight)
		{
			colourEditInFlight = true;
			colourBeforeEdit = background->getColour();
			pendingColourUndo = captureDocumentSnapshot(building);
		}
		applyColour(core::backgroundColourFromFloats(rgb));
	}
	if (finished)
	{
		if (pendingColourUndo) commitDocumentEdit(std::move(pendingColourUndo));
		pendingColourUndo.reset();
		colourBeforeEdit.reset();
		colourEditInFlight = false;
	}
	else if (cancelled)
	{
		// Escape puts the widget back where it started; the Building follows it and
		// the half-finished undo entry is dropped.
		if (colourBeforeEdit) applyColour(*colourBeforeEdit);
		colourBeforeEdit.reset();
		colourEditInFlight = false;
	}

	ImGui::Separator();
	if (ImGui::Button("Delete Background"))
	{
		// Deleting a Background takes every Window looking into it with it. The plan
		// names them and the shared "Confirm sector edit" popup spells the cascade out
		// before anything is applied.
		auto plan = building->planRemoveBackground(sector->getIndex());
		if (!plan.valid)
			core::addLogMessage("Background editor", 0, core::LogLevel::Error, plan.diagnostic);
		else
		{
			// The Sector this panel has been driving may not survive the confirmation.
			editedSector = nullptr;
			queueLocationEdit(building, plan);
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Delete key");
}


// The Selection panel's Facade branch, for ticket #47.
//
// A Facade reads like a Room - name, place, size, agents - but its editable
// property set is the Room's minus the walls: the perimeter is open by type
// invariant (ADR 0003), so there is nothing for the wall editor to offer and
// wall commands against it refuse rather than blur the type. The colour picker
// reuses Background's picker arithmetic wholesale: the same 0..1 float triple,
// the same no-alpha widget, the same pack/unpack pair on the way back.
void renderFacadePanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::Sector> const& sector)
{
	auto const facade = dynamic_pointer_cast<const core::Facade>(sector);
	if (!facade) return;

	ImGui::Text("Facade: %s", facade->getName().c_str());
	ImGui::Text("Sector index: %u", facade->getIndex());
	ImGui::Text("Layer: %s", layerLabel(building, facade->getLayerIndex()).c_str());
	ImGui::Text("Position: %u, %u", facade->getCellX(), facade->getCellY());
	ImGui::Text("Size: %u x %u cells", facade->getCellsWide(), facade->getDecksHigh());
	ImGui::Text("Agents: %u", (uint32_t)facade->getAgents().size());

	ImGui::Separator();

	// The colour lives on the Facade; the float triple is the widget's working
	// copy, re-synced whenever the selection changes so a live drag never fights
	// the value it is driving. Same shape as the Background panel's widget.
	static core::Building const* editedBuilding = nullptr;
	static core::Sector const* editedSector = nullptr;
	static float rgb[3] = { 0.0f, 0.0f, 0.0f };
	static optional<core::BackgroundColour> colourBeforeEdit;
	static optional<DocumentSnapshot> pendingColourUndo;
	static bool colourEditInFlight = false;

	if (editedBuilding != building.get() || editedSector != sector.get())
	{
		editedBuilding = building.get();
		editedSector = sector.get();
		// Any half-finished edit belongs to whatever was selected before, not to
		// this Facade, so it is dropped rather than carried across.
		colourBeforeEdit.reset();
		pendingColourUndo.reset();
		colourEditInFlight = false;
		core::backgroundColourToFloats(facade->getColour(), rgb);
	}

	auto applyColour = [&](core::BackgroundColour const& colour)
	{
		string diagnostic;
		if (building->setFacadeColour(sector->getIndex(), colour, &diagnostic)) return true;
		core::addLogMessage("Facade editor", 0, core::LogLevel::Error, diagnostic);
		editedSector = nullptr;
		return false;
	};

	// No alpha control: a Facade is rendered as a solid opaque colour, exactly
	// like a Background, and the same NoAlpha widget serves both.
	bool const changed = ImGui::ColorEdit3("Colour", rgb, ImGuiColorEditFlags_NoAlpha);
	bool const finished = ImGui::IsItemDeactivatedAfterEdit();
	bool const cancelled = ImGui::IsItemDeactivated() && !finished;

	if (changed)
	{
		// One undo entry per recolour, taken before the first live change and
		// committed only when the edit finishes.
		if (!colourEditInFlight)
		{
			colourEditInFlight = true;
			colourBeforeEdit = facade->getColour();
			pendingColourUndo = captureDocumentSnapshot(building);
		}
		applyColour(core::backgroundColourFromFloats(rgb));
	}
	if (finished)
	{
		if (pendingColourUndo) commitDocumentEdit(std::move(pendingColourUndo));
		pendingColourUndo.reset();
		colourBeforeEdit.reset();
		colourEditInFlight = false;
	}
	else if (cancelled)
	{
		// Escape puts the widget back where it started; the Building follows it and
		// the half-finished undo entry is dropped.
		if (colourBeforeEdit) applyColour(*colourBeforeEdit);
		colourBeforeEdit.reset();
		colourEditInFlight = false;
	}

	ImGui::Separator();
	// Deliberately no wall editor here. A Facade's perimeter is open by
	// construction, so every wall command against it refuses; showing buttons
	// that can only ever be disabled would advertise an edit the type forbids.
	ImGui::TextDisabled("Facades have no walls - the perimeter is open by construction.");
	// Resizing stays out of scope for Facades (ticket #53): planResizeLocation
	// refuses one, and the canvas offers no resize affordance. The panel says
	// so rather than leaving the refusal to be discovered by a dead keypress.
	ImGui::TextDisabled("Facades cannot be resized - delete and repaint to change the footprint.");
	ImGui::Separator();
	if (ImGui::Button("Delete Facade"))
	{
		// A Facade is occupiable, so the plan names the Agents inside and every
		// hosted object which goes with it, and the shared "Confirm sector edit"
		// popup spells the cascade out before anything is applied.
		auto plan = building->planRemoveFacade(sector->getIndex());
		if (!plan.valid)
			core::addLogMessage("Facade editor", 0, core::LogLevel::Error, plan.diagnostic);
		else
		{
			// The Sector this panel has been driving may not survive the confirmation.
			editedSector = nullptr;
			queueLocationEdit(building, plan);
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Delete key");
}


void renderBulkheadDoorPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	auto doorObject = static_pointer_cast<const core::BulkheadDoorSectorObject>(object);
	auto door = doorObject->getDoor();
	auto owner = object->getSector();
	uint32_t objectIndex = ~0u;
	for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
		if (owner->getObject(i) == object) { objectIndex = i; break; }

	ImGui::TextUnformatted("Bulkhead Door");
	ImGui::Text("Threshold position: %.1f, %u", (float)object->getCellX() + 1.0f,
		object->getCellY());
	ImGui::Text("Layer: %s", layerLabel(building, owner->getLayerIndex()).c_str());
	float pct = door->getOpenPercentage() * 100.0f;
	char const* state = "Unknown";
	switch (door->getState())
	{
	case core::OpenableObject::State::Open: state = "Open"; break;
	case core::OpenableObject::State::Opening: state = "Opening"; break;
	case core::OpenableObject::State::Closed: state = "Closed"; break;
	case core::OpenableObject::State::Closing: state = "Closing"; break;
	}
	ImGui::Text("Runtime state: %s (%.2f%% open)", state, pct);
	ImGui::Text("Open/close time: %.2fs", door->getOpenCloseTime());
	ImGui::Text("Left: %s", door->getSideSector(CORE_SIDE_LEFT)->getDescription().c_str());
	ImGui::Text("Right: %s", door->getSideSector(CORE_SIDE_RIGHT)->getDescription().c_str());

	static core::Building const* editedBuilding = nullptr;
	static core::SectorObject const* editedObject = nullptr;
	static int activationMode = (int)core::DoorActivationMode::RemoteControlled;
	static bool leftControl = true, rightControl = true;
	static float holdOpenSeconds = CORE_BULKHEAD_DOOR_STAY_OPEN_TIME;
	static int crossingLanes = 1;
	core::Building::CreateBulkheadDoorOptions current;
	if ((editedBuilding != building.get() || editedObject != object.get())
		&& objectIndex != ~0u
		&& building->getSectorBulkheadDoorOptions(owner->getIndex(), objectIndex, current))
	{
		editedBuilding = building.get(); editedObject = object.get();
		activationMode = (int)current.activationMode;
		leftControl = current.controls[0]; rightControl = current.controls[1];
		holdOpenSeconds = current.holdOpenSeconds;
		crossingLanes = (int)current.crossingLanes;
	}

	auto commitActivationAndControls = [&]()
	{
		try
		{
			core::Building::CreateBulkheadDoorOptions options;
			if (!building->getSectorBulkheadDoorOptions(owner->getIndex(), objectIndex, options))
				throw runtime_error("The selected Bulkhead Door has no authored definition");
			options.activationMode = static_cast<core::DoorActivationMode>(activationMode);
			options.controls[0] = leftControl;
			options.controls[1] = rightControl;
			auto undo = captureDocumentSnapshot(building);
			gSelectedSectorObject = building->applySectorBulkheadDoorOptions(
				owner->getIndex(), objectIndex, options);
			gHoveredSectorObject.reset(); editedObject = nullptr;
			commitDocumentEdit(std::move(undo));
			return true;
		}
		catch (core::Exception const& error)
		{
			reportEditorError("Bulkhead Door editor", error.getMessage());
		}
		catch (std::exception const& error)
		{
			reportEditorError("Bulkhead Door editor", error.what());
		}
		editedObject = nullptr;
		return false;
	};

	ImGui::Separator();
	bool activationOrControlsChanged = false;
	ImGui::BeginDisabled(!building->isSimulationPaused() || objectIndex == ~0u);
	if (ImGui::Combo("Activation", &activationMode,
		"Automatic\0Manual\0Remote Controlled\0Unavailable\0"))
	{
		activationOrControlsChanged = true;
		if (activationMode != (int)core::DoorActivationMode::RemoteControlled)
			leftControl = rightControl = false;
	}
	bool remote = activationMode == (int)core::DoorActivationMode::RemoteControlled;
	ImGui::BeginDisabled(!remote);
	activationOrControlsChanged |= ImGui::Checkbox("Left control", &leftControl);
	activationOrControlsChanged |= ImGui::Checkbox("Right control", &rightControl);
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	if (activationOrControlsChanged)
	{
		commitActivationAndControls();
		return;
	}
	ImGui::InputFloat("Hold open seconds", &holdOpenSeconds, 0.25f, 1.0f, "%.2f");
	ImGui::InputInt("Crossing lanes", &crossingLanes);
	bool valid = holdOpenSeconds >= 0.0f && crossingLanes == 1
		&& (remote || (!leftControl && !rightControl));
	ImGui::BeginDisabled(!building->isSimulationPaused() || !valid || objectIndex == ~0u);
	if (ImGui::Button("Apply Bulkhead Door settings"))
	{
		try
		{
			auto undo = captureDocumentSnapshot(building);
			core::Building::CreateBulkheadDoorOptions options{
				{ leftControl, rightControl },
				static_cast<core::DoorActivationMode>(activationMode),
				holdOpenSeconds, (uint32_t)crossingLanes };
			gSelectedSectorObject = building->applySectorBulkheadDoorOptions(
				owner->getIndex(), objectIndex, options);
			gHoveredSectorObject.reset(); editedObject = nullptr;
			commitDocumentEdit(std::move(undo));
			ImGui::EndDisabled();
			return;
		}
		catch (core::Exception const& error)
		{
			reportEditorError("Bulkhead Door editor", error.getMessage());
		}
		catch (std::exception const& error)
		{
			reportEditorError("Bulkhead Door editor", error.what());
		}
	}
	ImGui::EndDisabled();
	if (!building->isSimulationPaused())
		ImGui::TextDisabled("Pause simulation to edit settings.");
	ImGui::Separator();
	if (ImGui::Button("Delete Bulkhead Door"))
	{
		try
		{
			auto undo = captureDocumentSnapshot(building);
			if (removeClipboardSelection(building)) commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			reportEditorError("Bulkhead Door editor", error.getMessage());
		}
		catch (std::exception const& error)
		{
			reportEditorError("Bulkhead Door editor", error.what());
		}
	}
	ImGui::SameLine(); ImGui::TextDisabled("Delete key");
}


void renderLiftOwnedControlPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	uint32_t bridgeSector, bridgeObject;
	if (building->isBulkheadDoorOwnedControl(object, &bridgeSector, &bridgeObject))
	{
		ImGui::TextUnformatted("Bulkhead Door control");
		ImGui::Text("Position: %u, %u", object->getCellX(), object->getCellY());
		ImGui::Separator();
		ImGui::Text("Owned by Bulkhead Door in Location %u", bridgeSector);
		ImGui::TextDisabled("This control is managed by its Bulkhead Door and is read-only.");
		if (ImGui::Button("Select Bulkhead Door"))
		{
			auto sector = building->getSector(bridgeSector);
			if (sector && bridgeObject < sector->getNumObjects())
				gSelectedSectorObject = sector->getObject(bridgeObject);
		}
		return;
	}
	if (building->isForceBridgeOwnedControl(object, &bridgeSector, &bridgeObject))
	{
		ImGui::TextUnformatted("Force Bridge control");
		ImGui::Text("Position: %u, %u", object->getCellX(), object->getCellY());
		ImGui::Separator();
		ImGui::Text("Owned by Force Bridge in Room %u", bridgeSector);
		ImGui::TextDisabled("This control is managed by its Force Bridge and is read-only.");
		if (ImGui::Button("Select Force Bridge"))
		{
			auto sector = building->getSector(bridgeSector);
			if (sector && bridgeObject < sector->getNumObjects())
				gSelectedSectorObject = sector->getObject(bridgeObject);
		}
		return;
	}
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

void renderForceBridgePanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	auto fbObject = static_pointer_cast<const core::ForceBridgeSectorObject>(object);
	auto forceBridge = fbObject->getForceBridge();
	auto room = object->getSector();
	uint32_t objectIndex = ~0u;
	for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		if (room->getObject(i) == object) { objectIndex = i; break; }

	ImGui::TextUnformatted("Force Bridge");
	ImGui::Text("Room: %s", room->getName().c_str());
	ImGui::Text("Room index: %u", room->getIndex());
	ImGui::Text("Layer: %s", layerLabel(building, room->getLayerIndex()).c_str());
	ImGui::Text("Position: %u, %u", object->getCellX(), object->getCellY());
	ImGui::Text("Deck offset: %u", object->getCellY() - room->getCellY());
	float pct = forceBridge->getExtendedPercentage() * 100.0f;
	char const* state = "Unknown";
	switch (forceBridge->getState())
	{
	case core::ExtensibleObject::State::Extended: state = "Extended"; break;
	case core::ExtensibleObject::State::Extending: state = "Extending"; break;
	case core::ExtensibleObject::State::Retracted: state = "Retracted"; break;
	case core::ExtensibleObject::State::Retracting: state = "Retracting"; break;
	}
	ImGui::Text("Runtime state: %s (%.2f%%)", state, pct);
	ImGui::Text("Extend/retract time: %.2fs", forceBridge->getExtendRetractTime());

	static core::Building const* editedBuilding = nullptr;
	static core::SectorObject const* editedObject = nullptr;
	static int width = 1, side = 0, controls = 1, previousControls = 1;
	static bool extensible = true, initiallyExtended = true;
	core::Building::CreateForceBridgeOptions current;
	if ((editedBuilding != building.get() || editedObject != object.get())
		&& objectIndex != ~0u
		&& building->getSectorForceBridgeOptions(room->getIndex(), objectIndex, current))
	{
		editedBuilding = building.get(); editedObject = object.get();
		width = (int)current.width;
		side = current.fromSide == CORE_SIDE_LEFT ? 0 : 1;
		extensible = current.extensible;
		initiallyExtended = current.startExtended;
		controls = (int)current.controlCount;
		if (controls > 0) previousControls = controls;
	}

	auto commitSettings = [&]()
	{
		try
		{
			auto undo = captureDocumentSnapshot(building);
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			core::Building::CreateForceBridgeOptions options{ (uint32_t)width,
				side == 0 ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT, extensible,
				extensible ? initiallyExtended : true, extensible ? (uint32_t)controls : 0u };
			gSelectedSectorObject = building->applySectorForceBridgeOptions(
				room->getIndex(), objectIndex, options);
			gHoveredSectorObject.reset();
			editedObject = nullptr;
			commitDocumentEdit(std::move(undo));
			return true;
		}
		catch (core::Exception const& error)
		{
			core::addLogMessage("Force Bridge editor", 0, core::LogLevel::Error, error.getMessage());
		}
		catch (std::exception const& error)
		{
			core::addLogMessage("Force Bridge editor", 0, core::LogLevel::Error, error.what());
		}
		return false;
	};

	ImGui::Separator();
	ImGui::InputInt("Width", &width);
	ImGui::Combo("Extends from", &side, "Left\0Right\0");
	if (ImGui::Checkbox("Extensible", &extensible))
	{
		if (!extensible)
		{
			if (controls > 0) previousControls = controls;
			controls = 0;
			initiallyExtended = true;
		}
		else controls = clamp(previousControls, 1, 2);
	}
	ImGui::BeginDisabled(!extensible);
	bool previousInitiallyExtended = initiallyExtended;
	if (ImGui::Checkbox("Initially extended", &initiallyExtended))
	{
		if (objectIndex != ~0u && commitSettings()) return;
		initiallyExtended = previousInitiallyExtended;
	}
	int controlChoice = clamp(controls, 1, 2) - 1;
	int previousControlCount = controls;
	if (ImGui::Combo("Physical controls", &controlChoice,
		"Extension side\0Both sides\0"))
	{
		controls = controlChoice + 1;
		previousControls = controls;
		if (objectIndex != ~0u && commitSettings()) return;
		controls = previousControlCount;
		previousControls = controls;
	}
	ImGui::EndDisabled();
	bool basicValid = width >= 1 && width <= (int)CORE_FORCEBRIDGE_MAX_SIZE
		&& (!extensible || (controls >= 1 && controls <= 2));
	ImGui::BeginDisabled(!basicValid || objectIndex == ~0u);
	if (ImGui::Button("Apply Force Bridge settings") && commitSettings()) return;
	ImGui::EndDisabled();
	ImGui::Separator();
	if (ImGui::Button("Delete Force Bridge"))
	{
		try
		{
			auto undo = captureDocumentSnapshot(building);
			if (removeClipboardSelection(building)) commitDocumentEdit(std::move(undo));
		}
		catch (core::Exception const& error)
		{
			reportEditorError("Force Bridge editor", error.getMessage());
		}
		catch (std::exception const& error)
		{
			reportEditorError("Force Bridge editor", error.what());
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("Delete key");
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
	ImGui::Text("Layer: %s", layerLabel(building, room->getLayerIndex()).c_str());
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
	static int directionalBatchLimit = 4;
	core::Building::CreateLadderOptions current{};
	if ((editedBuilding != building.get() || editedObject != object.get())
		&& objectIndex != ~0u && building->getRoomLadderOptions(room->getIndex(), objectIndex, current))
	{
		editedBuilding = building.get(); editedObject = object.get();
		extensible = current.extensible;
		initiallyExtended = current.extensible ? current.startExtended : true;
		directionalBatchLimit = (int)current.directionalBatchLimit;
	}
	auto commitSettings = [&](bool desiredExtensible, bool desiredInitiallyExtended,
		int desiredBatchLimit)
	{
		auto undo = captureDocumentSnapshot(building);
		try
		{
			if (!building->isSimulationPaused()) building->pauseSimulation();
			gUISettings.worldPaused = true;
			gSelectedSectorObject = building->applyRoomLadderOptions(room->getIndex(), objectIndex,
				{ ladder->getDecksHigh(), desiredExtensible,
					desiredExtensible ? desiredInitiallyExtended : true,
					(uint32_t)desiredBatchLimit });
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
		if (commitSettings(extensible, initiallyExtended, directionalBatchLimit)) return;
		extensible = previousExtensible;
	}
	ImGui::BeginDisabled(!extensible);
	bool previousInitiallyExtended = initiallyExtended;
	if (ImGui::Checkbox("Initially extended", &initiallyExtended))
	{
		if (commitSettings(extensible, initiallyExtended, directionalBatchLimit)) return;
		initiallyExtended = previousInitiallyExtended;
	}
	ImGui::EndDisabled();
	if (!extensible) initiallyExtended = true;
	ImGui::InputInt("Directional batch limit", &directionalBatchLimit);
	ImGui::BeginDisabled(objectIndex == ~0u || directionalBatchLimit <= 0);
	if (ImGui::Button("Apply Ladder settings"))
	{
		if (commitSettings(extensible, initiallyExtended, directionalBatchLimit)) return;
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
			else reportEditorError("Room Ladder editor",
				"The selected Room Ladder could not be deleted");
		}
		catch (core::Exception const& error)
		{ reportEditorError("Room Ladder editor", error.getMessage()); }
		catch (std::exception const& error)
		{ reportEditorError("Room Ladder editor", error.what()); }
	}
	ImGui::SameLine(); ImGui::TextDisabled("Delete key");
}


void renderLiftPanel(shared_ptr<const core::Building> const& building,
	shared_ptr<const core::Lift> lift, bool includeAgentDebug = false);

void renderPlatformLiftPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::SectorObject> object)
{
	auto room = object->getSector();
	uint32_t objectIndex = ~0u;
	for (uint32_t i = 0; i < room->getNumObjects(); ++i)
		if (room->getObject(i) == object) { objectIndex = i; break; }
	core::Building::CreateLiftOptions current;
	if (objectIndex == ~0u || !building->getPlatformLiftOptions(room->getIndex(), objectIndex, current))
	{
		ImGui::TextDisabled("The selected PlatformLift has no authored definition.");
		return;
	}

	auto platformLift = static_pointer_cast<const core::LiftSectorObject>(object)->getLift();
	ImGui::TextUnformatted("Platform Lift");
	ImGui::Text("Room: %s", room->getName().c_str());
	ImGui::Text("Layer: %s", layerLabel(building, room->getLayerIndex()).c_str());
	ImGui::Text("Position: %u, %u", object->getCellX(), object->getCellY());
	ImGui::Text("Car y: %.2f", platformLift->getPosition().y);

	static core::Building const* editedBuilding = nullptr;
	static core::SectorObject const* editedObject = nullptr;
	static core::Building::CreateLiftOptions draft;
	if (editedBuilding != building.get() || editedObject != object.get())
	{
		editedBuilding = building.get(); editedObject = object.get(); draft = current;
	}
	ImGui::Separator();
	ImGui::TextUnformatted("Connected levels");
	bool ground = true;
	ImGui::BeginDisabled();
	ImGui::Checkbox("Ground (mandatory)", &ground);
	ImGui::EndDisabled();

	auto candidates = building->getPlatformLiftStopCandidates(room->getIndex(),
		object->getCellX() - room->getCellX());
	for (auto const& candidate : candidates)
	{
		bool selected = find(draft.stopOffsets.begin(), draft.stopOffsets.end(), candidate.deckOffset)
			!= draft.stopOffsets.end();
		auto tentative = draft;
		if (!selected) tentative.stopOffsets.push_back(candidate.deckOffset);
		else tentative.stopOffsets.erase(remove(tentative.stopOffsets.begin(), tentative.stopOffsets.end(),
			candidate.deckOffset), tentative.stopOffsets.end());
		bool lastWalkway = selected && draft.stopOffsets.size() <= 2;
		auto validation = lastWalkway ? core::Building::PlatformLiftEditPlan{}
			: building->planPlatformLiftEdit(room->getIndex(), objectIndex, tentative);
		bool disabled = lastWalkway || (!selected && !validation.valid);
		ImGui::PushID((int)candidate.deckOffset);
		ImGui::BeginDisabled(disabled);
		if (ImGui::Checkbox("##stop", &selected)) draft = tentative;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Text("Walkway: deck %u (global y %u)", candidate.deckOffset,
			room->getCellY() + candidate.deckOffset);
		if (disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", lastWalkway
				? "A PlatformLift requires at least one Walkway stop"
				: validation.diagnostic.c_str());
		ImGui::PopID();
	}
	if (candidates.empty()) ImGui::TextDisabled("No Walkways exist above this column.");
	ImGui::InputFloat("Stop duration (seconds)", &draft.platformStopDurationSeconds,
		0.5f, 1.0f, "%.2f");
	sort(draft.stopOffsets.begin(), draft.stopOffsets.end());
	bool changed = draft.stopOffsets != current.stopOffsets
		|| abs(draft.platformStopDurationSeconds - current.platformStopDurationSeconds) > 0.0001f;
	ImGui::BeginDisabled(!changed || draft.platformStopDurationSeconds < 0.0f);
	if (ImGui::Button("Apply PlatformLift settings"))
	{
		auto plan = building->planPlatformLiftEdit(room->getIndex(), objectIndex, draft);
		if (!plan.valid) reportEditorError("PlatformLift editor", plan.diagnostic);
		else { editedObject = nullptr; queuePlatformLiftEdit(building, plan); }
	}
	ImGui::EndDisabled();

	ImGui::Separator();
	renderLiftPanel(building, platformLift, true);

	ImGui::Separator();
	if (ImGui::Button("Delete Platform Lift"))
	{
		auto plan = building->planRemovePlatformLift(room->getIndex(), objectIndex);
		if (!plan.valid) reportEditorError("PlatformLift editor", plan.diagnostic);
		else { editedObject = nullptr; queuePlatformLiftEdit(building, plan); }
	}
	ImGui::SameLine(); ImGui::TextDisabled("Delete key");
}


void renderLiftPanel(shared_ptr<const core::Building> const& building,
	shared_ptr<const core::Lift> lift, bool includeAgentDebug)
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
	ImGui::Text("Current stop: %u", resource->liftCurrentStop);
	if (resource->liftTargetStop == ~0u) ImGui::TextUnformatted("Target stop: <none>");
	else ImGui::Text("Target stop: %u", resource->liftTargetStop);
	string queuedStops;
	for (auto stop : resource->liftScheduledStops)
	{
		if (!queuedStops.empty()) queuedStops += ", ";
		queuedStops += to_string(stop);
	}
	ImGui::Text("Queued stops: %s", queuedStops.empty() ? "<none>" : queuedStops.c_str());
	ImGui::Text("Phase: %s", phase);
	ImGui::Text("Capacity: %u (%u occupied, %u reserved)", resource->capacity,
		resource->occupantCount, resource->admissionReservationCount);

	ImGui::TextUnformatted("Agents waiting for / using lift");
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


void renderAgentView(shared_ptr<core::Building> building)
{
	ImGuiTableFlags flags =
		ImGuiTableFlags_SizingStretchSame |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_ContextMenuInBody;

	if (ImGui::BeginTable("Agents", 7, flags))
	{
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Active");
		ImGui::TableSetupColumn("Group");
		ImGui::TableSetupColumn("Behaviour");
		ImGui::TableSetupColumn("Sector");
		ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Path");
		ImGui::TableHeadersRow();

		core::Agent* newSelectedAgent{ gSelectedAgent };

		for (uint32_t l = 0; l < building->getLayerCount(); ++l)
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
					if (agent->isActive()) ImGui::Text("%s", agent->getName().c_str());
					else ImGui::TextDisabled("%s", agent->getName().c_str());

					// Active: an activated Agent is simulated, a deactivated one is not
					// (#118). Activation changes are refused while the simulation runs,
					// so the control is disabled in step with the rest of the
					// paused-only editing UI.
					ImGui::TableSetColumnIndex(1);
					ImGui::BeginDisabled(!building->isSimulationPaused());
					if (ImGui::Button(agent->isActive() ? ICON_FA_EYE : ICON_FA_EYE_SLASH,
						ImVec2(ImGui::GetFrameHeight(), 0.0f)))
					{
						string diagnostic;
						if (!building->setAgentActive(building->getAgentId(agent),
							!agent->isActive(), &diagnostic))
						{
							core::addLogMessage("Agents", 0, core::LogLevel::Warning, diagnostic);
						}
					}
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered())
					{
						if (!building->isSimulationPaused())
						{
							ImGui::SetTooltip("Pause the simulation to activate or deactivate this Agent");
						}
						else if (agent->isActive())
						{
							ImGui::SetTooltip("Deactivate this Agent: it keeps its position and route but is not simulated");
						}
						else
						{
							ImGui::SetTooltip("Activate this Agent: the simulation drives it again");
						}
					}

					// Group: the Agent group this Agent is assigned to, edited in
					// place. The cell shows the group's current name rather than a
					// copy of it, so a rename is reflected here the next frame.
					ImGui::TableSetColumnIndex(2);
					renderAgentGroupAssignmentCell(building, building->getAgentId(agent));

					// Behaviour: named picker, never a numeric registry ID.
					ImGui::TableSetColumnIndex(3);
					renderAgentBehaviourAssignmentCell(building, building->getAgentId(agent));

					// Sector
					ImGui::TableSetColumnIndex(4);
					ImGui::TextUnformatted(sector->getDescription().c_str());

					// State
					ImGui::TableSetColumnIndex(5);

					if (!agent->isActive())
					{
						ImGui::TextDisabled("Inactive");
					}
					else switch (agent->getState())
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
					ImGui::TableSetColumnIndex(6);
					
					auto const& path = agent->getPath();
					
					if (path)
					{
						ImGui::Text("%u/%zu vertices", agent->getPathTargetNodeIndex(), path->nodes.size());
					}
					else if (building->isSimulationPaused())
					{
						// Pausing tears down live traversal, so the Agent's path pointer is
						// cleared. Its retained destination still tells us where it resumes.
						core::Building::TopologyPathIntent intent;
						if (building->getPausedPathIntent(*agent, intent))
						{
							auto destination = intent.destinationSector
								? building->getSector((uint32_t)intent.destinationSector.value - 1)
								: nullptr;
							ImGui::TextDisabled("to %s (paused)",
								destination ? destination->getDescription().c_str() : "<unknown>");
						}
						else
						{
							ImGui::TextUnformatted("");
						}
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
	static void* selectedNode{ nullptr };

	static ImGuiTreeNodeFlags nodeFlags =
		ImGuiTreeNodeFlags_OpenOnArrow |
		ImGuiTreeNodeFlags_OpenOnDoubleClick |
		ImGuiTreeNodeFlags_SpanAvailWidth;

	auto const layerCount = building->getLayerCount();

	for (uint32_t layer = 0; layer < layerCount; ++layer)
	{
		auto sectors = building->getSectors(layer);

		ImGui::PushID(layer);
		if (ImGui::TreeNode("layerObjects", "%s", layerLabel(building, layer).c_str()))
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
						|| sector->getType() == core::SectorType::Stairwell
						|| sector->getType() == core::SectorType::Staircase
						|| sector->getType() == core::SectorType::Background
						|| sector->getType() == core::SectorType::Facade;
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
		ImGui::PopID();
	}

}


void renderLocationWallEditor(shared_ptr<core::Building> const& building,
	shared_ptr<const core::Sector> const& location)
{
	ImGui::Separator();
	ImGui::TextUnformatted("Shared walls");
	ImGui::TextDisabled("Open walls directly connect adjacent Rooms or Corridors.");
	if (!ImGui::BeginTable("LocationWalls", 3, ImGuiTableFlags_BordersInnerV)) return;
	ImGui::TableSetupColumn("Deck");
	ImGui::TableSetupColumn("Left");
	ImGui::TableSetupColumn("Right");
	ImGui::TableHeadersRow();
	for (uint32_t deck = 0; deck < location->getDecksHigh(); ++deck)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Text("%u (floor %u)", deck, location->getCellY() + deck);
		for (int side = CORE_SIDE_LEFT; side <= CORE_SIDE_RIGHT; ++side)
		{
			ImGui::TableSetColumnIndex(side + 1);
			auto const end = location->getEndType(deck, side);
			if (end == core::SectorEndType::BulkheadDoor)
			{
				ImGui::TextUnformatted("Bulkhead Door");
				continue;
			}
			string diagnostic;
			bool const isOpen = end == core::SectorEndType::None;
			bool const canChange = isOpen
				? building->canAddLocationWall(location->getIndex(), deck, side, &diagnostic)
				: building->canRemoveLocationWall(location->getIndex(), deck, side, &diagnostic);
			string const label = string(isOpen ? "Add wall" : "Open wall") + "##wall-"
				+ to_string(deck) + "-" + to_string(side);
			ImGui::BeginDisabled(!canChange);
			bool const clicked = ImGui::Button(label.c_str());
			ImGui::EndDisabled();
			if (!canChange && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", diagnostic.c_str());
			if (!clicked) continue;

			auto undo = captureDocumentSnapshot(building);
			try
			{
				if (!building->isSimulationPaused()) building->pauseSimulation();
				gUISettings.worldPaused = true;
				if (isOpen) building->addLocationWall(location->getIndex(), deck, side);
				else building->removeLocationWall(location->getIndex(), deck, side);
				building->finishBuild();
				commitDocumentEdit(std::move(undo));
			}
			catch (core::Exception const& error)
			{
				reportEditorError("Wall editor", error.getMessage());
			}
			catch (std::exception const& error)
			{
				reportEditorError("Wall editor", error.what());
			}
		}
	}
	ImGui::EndTable();
}


void renderSelectedObjectPanel(shared_ptr<core::Building> const& building)
{
	if ((!gSelectedSector && !gSelectedSectorObject)
		|| !ImGui::CollapsingHeader("Selection", nullptr, 0)) return;

	if (gSelectedSector)
	{
		// A Background is not a place an agent can be, so it gets its own minimal
		// panel rather than the generic Sector readout with its "Agents:" line.
		if (gSelectedSector->getType() == core::SectorType::Background)
		{
			renderBackgroundPanel(building, gSelectedSector);
			return;
		}

		// A Facade reads like a Room but owns a colour instead of walls, so it
		// takes its own panel and never reaches the wall editor below.
		if (gSelectedSector->getType() == core::SectorType::Facade)
		{
			renderFacadePanel(building, gSelectedSector);
			return;
		}

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
		case core::SectorType::Stairwell: type = "Stairwell"; break;
	case core::SectorType::Staircase: type = "Staircase"; break;
		default: break;
		}
		ImGui::Text("%s: %s", type, gSelectedSector->getName().c_str());
		ImGui::Text("Sector index: %u", gSelectedSector->getIndex());
		ImGui::Text("Layer: %s", layerLabel(building, gSelectedSector->getLayerIndex()).c_str());
		ImGui::Text("Position: %u, %u", gSelectedSector->getCellX(), gSelectedSector->getCellY());
		if (gSelectedSector->getType() == core::SectorType::Lift)
		{
			auto lift = static_pointer_cast<const core::LiftTransit>(gSelectedSector)->getLift();
			ImGui::Text("Car y: %.2f", lift->getPosition().y);
		}
		ImGui::Text("Size: %u x %u cells", gSelectedSector->getCellsWide(),
			gSelectedSector->getDecksHigh());
		ImGui::Text("Agents: %u", (uint32_t)gSelectedSector->getAgents().size());

		switch (gSelectedSector->getType())
		{
		case core::SectorType::Location:
			renderLocationWallEditor(building, gSelectedSector);
			break;

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
			static int directionalBatchLimit = 4;
			core::Building::CreateLadderOptions current{ 0, false, true };
			if ((editedBuilding != building.get() || editedSector != gSelectedSector->getIndex())
				&& building->getLadderOptions(gSelectedSector->getIndex(), current))
			{
				editedBuilding = building.get();
				editedSector = gSelectedSector->getIndex();
				extensible = current.extensible;
				initiallyExtended = current.extensible ? current.startExtended : true;
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
			ImGui::InputInt("Directional batch limit", &directionalBatchLimit);
			bool valuesValid = directionalBatchLimit > 0;
			ImGui::BeginDisabled(!valuesValid);
			if (ImGui::Button("Apply Ladder settings"))
			{
				core::Building::CreateLadderOptions options{
					gSelectedSector->getDecksHigh(), extensible,
					extensible ? initiallyExtended : true,
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

		case core::SectorType::Stairwell:
		{
			static core::Building const* editedBuilding = nullptr;
			static uint32_t editedSector = ~0u;
			static int mountSide = CORE_SIDE_LEFT;
			static int directionalCapacity = 0;
			static int directionalBatchLimit = 4;
			core::Building::CreateStairwellOptions current{ 0, CORE_SIDE_LEFT };
			if ((editedBuilding != building.get() || editedSector != gSelectedSector->getIndex())
				&& building->getStairwellOptions(gSelectedSector->getIndex(), current))
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
				core::Building::CreateStairwellOptions options{};
				if (!building->getStairwellOptions(gSelectedSector->getIndex(), options))
				{
					core::addLogMessage("Stairwell editor", 0, core::LogLevel::Error,
						"The selected Stairwell no longer has an authored definition");
				}
				else
				{
					options.mountSide = requestedSide;
					auto plan = building->planResizeStairwell(gSelectedSector->getIndex(),
						gSelectedSector->getCellX(), gSelectedSector->getCellY(), options);
					if (!plan.valid)
						core::addLogMessage("Stairwell editor", 0, core::LogLevel::Error,
							plan.diagnostic);
					else
					{
						mountSide = requestedSide;
						queueStairwellEdit(building, plan);
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
				core::Building::CreateStairwellOptions options{
					gSelectedSector->getDecksHigh(), mountSide,
					(uint32_t)directionalCapacity, (uint32_t)max(1, directionalBatchLimit) };
				auto plan = building->planResizeStairwell(gSelectedSector->getIndex(),
					gSelectedSector->getCellX(), gSelectedSector->getCellY(), options);
				if (!plan.valid)
					core::addLogMessage("Stairwell editor", 0, core::LogLevel::Error, plan.diagnostic);
				else queueStairwellEdit(building, plan);
			}
			ImGui::EndDisabled();
			ImGui::Separator();
			if (ImGui::Button("Delete Stairwell"))
			{
				auto plan = building->planRemoveStairwell(gSelectedSector->getIndex());
				if (!plan.valid)
					core::addLogMessage("Stairwell editor", 0, core::LogLevel::Error, plan.diagnostic);
				else queueStairwellEdit(building, plan);
			}
			ImGui::SameLine();
			ImGui::TextDisabled("Delete key");
			break;
		}

		case core::SectorType::Staircase:
		{
			static core::Building const* editedBuilding = nullptr;
			static uint32_t editedSector = ~0u;
			static int x = 0;
			static int y = 0;
			static int width = 2;
			static int riseSide = CORE_SIDE_RIGHT;
			static float speed = 0.0f;
			core::Building::CreateStaircaseOptions current;
			if ((editedBuilding != building.get() || editedSector != gSelectedSector->getIndex())
				&& building->getStaircaseOptions(gSelectedSector->getIndex(), current))
			{
				editedBuilding = building.get(); editedSector = gSelectedSector->getIndex();
				x = (int)gSelectedSector->getCellX(); y = (int)gSelectedSector->getCellY();
				width = (int)current.cellsWide; riseSide = current.riseSide; speed = current.speed;
			}
			ImGui::InputInt("X", &x);
			ImGui::InputInt("Lower level", &y);
			ImGui::InputInt("Width", &width);
			int sideIndex = riseSide == CORE_SIDE_LEFT ? 0 : 1;
			if (ImGui::Combo("Rises toward", &sideIndex, "Left\0Right\0"))
				riseSide = sideIndex == 0 ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT;
			ImGui::InputFloat("Speed", &speed, 0.1f, 1.0f, "%.2f");
			ImGui::TextDisabled("0 = stairs, + = up, - = down");
			auto apply = [&](bool remove)
			{
				auto plan = remove ? building->planRemoveStaircase(gSelectedSector->getIndex())
					: building->planResizeStaircase(gSelectedSector->getIndex(),
						(uint32_t)max(0, x), (uint32_t)max(0, y),
						{ (uint32_t)max(0, width), riseSide, speed });
				if (!plan.valid) { core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, plan.diagnostic); return; }
				try
				{
					auto undo = captureDocumentSnapshot(building);
					if (!building->isSimulationPaused()) building->pauseSimulation();
					auto index = building->applyStaircaseEdit(plan);
					commitDocumentEdit(std::move(undo));
					if (remove) clearSelections(); else gSelectedSector = building->getSector(index);
				}
				catch (std::exception const& error) { core::addLogMessage("Staircase editor", 0, core::LogLevel::Error, error.what()); }
			};
			ImGui::BeginDisabled(width < 2);
			if (ImGui::Button("Apply Staircase settings")) { apply(false); return; }
			ImGui::EndDisabled();
			ImGui::Separator();
			if (ImGui::Button("Delete Staircase")) { apply(true); return; }
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
			renderBulkheadDoorPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::Door:
			renderDoorPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::InteractionPoint:
			renderLiftOwnedControlPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::ForceBridge:
			renderForceBridgePanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::Ladder:
			renderLadderPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::Lift:
			renderPlatformLiftPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::Marker:
			renderMarkerPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::Window:
			renderWindowPanel(building, gSelectedSectorObject);
			break;

		case core::SectorObjectType::Walkway:
			renderWalkwayPanel(building, gSelectedSectorObject);
			break;

		default:
			break;
		}
	}
}


void renderSelectedAgentPanel(shared_ptr<core::Building> building)
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

	// Activation (#118): a deactivated Agent keeps its authored position and
	// route but no tick acts on it. The change is refused while the simulation
	// runs, so the button is disabled in step with the rest of the
	// paused-only editing UI.
	ImGui::BeginDisabled(!building->isSimulationPaused());
	if (ImGui::Button(gSelectedAgent->isActive() ? "Deactivate" : "Activate"))
	{
		string diagnostic;
		if (!building->setAgentActive(id, !gSelectedAgent->isActive(), &diagnostic))
		{
			core::addLogMessage("Agents", 0, core::LogLevel::Warning, diagnostic);
		}
	}
	ImGui::EndDisabled();
	if (!building->isSimulationPaused() && ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("Pause the simulation to activate or deactivate an Agent");
	}

	renderAgentEffectiveProperties(building, id);
	renderAgentTagAssignmentChecklist(building, id);
	renderAgentBehaviourConfigurationPanel(building, id);

	if (gSelectingAgentPathDestination)
	{
		ImGui::TextColored({ 1.0f, 0.75f, 0.1f, 1.0f },
			"Select a destination vertex (Escape to cancel)");
		if (ImGui::Button("Cancel path selection")) endAgentPathSelection();
	}
	else
	{
		auto const behaviourOwnsMovement = building->agentBehaviourOwnsMovement(id);
		ImGui::BeginDisabled(behaviourOwnsMovement);
		if (ImGui::Button("Select path destination (Ctrl+P)"))
			beginAgentPathSelection(building);
		ImGui::EndDisabled();
		if (behaviourOwnsMovement && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("The enabled Agent behaviour owns movement");
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
	if (!gSelectedAgent->isActive())
	{
		// A deactivated Agent is not simulated, so its movement state is
		// frozen history rather than something the run is doing (#118).
		state = "Inactive (not simulated)";
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
		auto const behaviourOwnsMovement = building->agentBehaviourOwnsMovement(id);
		ImGui::BeginDisabled(behaviourOwnsMovement);
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
		ImGui::EndDisabled();
	}
}


void addBackLayer(shared_ptr<core::Building> const& building)
{
	if (!building) return;

	if (building->getLayerCount() >= CORE_MAX_LAYERS)
	{
		core::addLogMessage("Layers", 0, core::LogLevel::Warning,
			format("A Building can have at most %u layers", (uint32_t)CORE_MAX_LAYERS));
		return;
	}

	auto undo = captureDocumentSnapshot(building);
	try
	{
		if (!building->isSimulationPaused()) building->pauseSimulation();

		auto const layer = building->addLayer();
		building->finishBuild();
		commitDocumentEdit(std::move(undo));
		core::addLogMessage("Layers", 0, core::LogLevel::Info,
			format("Added {} as the new back layer", building->getLayerName(layer)));
	}
	catch (core::Exception const& error)
	{
		core::addLogMessage("Layers", 0, core::LogLevel::Error, error.getMessage());
	}
	catch (std::exception const& error)
	{
		core::addLogMessage("Layers", 0, core::LogLevel::Error, error.what());
	}
}


void renderLayersPanel(shared_ptr<core::Building> const& building)
{
	if (!building) return;

	auto const layerCount = building->getLayerCount();

	// Loads, undos, and layer edits can all change the layer count from under us.
	gUISettings.visibleLayer = std::clamp(gUISettings.visibleLayer, 0,
		static_cast<int>(layerCount) - 1);

	ImGuiTableFlags flags =
		ImGuiTableFlags_SizingStretchProp |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_RowBg;

	if (ImGui::BeginTable("Layers", 3, flags))
	{
		ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 40.0f);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 40.0f);

		for (uint32_t layer = 0; layer < layerCount; ++layer)
		{
			bool const visible = static_cast<int>(layer) == gUISettings.visibleLayer;
			// The viewport draws the selected Layer solid and the Layer directly behind
			// it as a wireframe overlay. Every other Layer is hidden.
			bool const wireframeOverlay = !visible
				&& static_cast<int>(layer) == gUISettings.visibleLayer + 1
				&& gUISettings.renderNextLayerWireframe;

			ImGui::TableNextRow();
			ImGui::PushID(layer);

			if (visible)
			{
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
					ImGui::GetColorU32(ImVec4(1.0f, 0.5f, 0.0f, 0.35f)));
			}
			else if (wireframeOverlay)
			{
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
					ImGui::GetColorU32(ImVec4(0.45f, 0.55f, 1.0f, 0.20f)));
			}

			ImGui::TableSetColumnIndex(0);
			ImGui::PushStyleColor(ImGuiCol_Button, visible
				? ImGui::GetColorU32(ImVec4(1.0f, 0.5f, 0.0f, 0.75f))
				: ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.0f)));
			if (ImGui::Button(visible ? ICON_FA_EYE : ICON_FA_EYE_SLASH,
				ImVec2(ImGui::GetFrameHeight(), 0.0f)))
			{
				gUISettings.visibleLayer = static_cast<int>(layer);
			}
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
			{
				if (visible)
				{
					ImGui::SetTooltip("Viewing this layer");
				}
				else if (wireframeOverlay)
				{
					ImGui::SetTooltip("%s\nDrawn as a wireframe overlay behind the selected layer",
						building->getLayerName(layer).c_str());
				}
				else
				{
					ImGui::SetTooltip("View %s", building->getLayerName(layer).c_str());
				}
			}

			ImGui::TableSetColumnIndex(1);
			renderLayerNameEditor(building, layer);

			// A Building keeps at least two Layers, so deletion is only offered while
			// it has more than two.
			ImGui::TableSetColumnIndex(2);
			bool const canDelete = layerCount > 2;
			ImGui::BeginDisabled(!canDelete);
			if (ImGui::Button(ICON_FA_TRASH, ImVec2(ImGui::GetFrameHeight(), 0.0f)))
			{
				requestLayerDelete(building, layer);
			}
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered())
			{
				if (canDelete)
					ImGui::SetTooltip("Delete %s and everything on it", building->getLayerName(layer).c_str());
				else
					ImGui::SetTooltip("A Building must keep at least two layers");
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	ImGui::Spacing();

	bool const atMaximum = layerCount >= static_cast<uint32_t>(CORE_MAX_LAYERS);

	ImGui::BeginDisabled(atMaximum);
	if (ImGui::Button(ICON_FA_PLUS " Add Layer"))
		addBackLayer(building);
	ImGui::EndDisabled();
	if (atMaximum && ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("A Building can have at most %u layers",
			(uint32_t)CORE_MAX_LAYERS);
	}

	ImGui::SameLine();
	ImGui::TextDisabled("%u of %u layers", layerCount, (uint32_t)CORE_MAX_LAYERS);
}


void renderBuildingPanel(shared_ptr<core::Building> building)
{
	if (ImGui::CollapsingHeader("Tags"))
	{
		// Persist a newly created or selected registry reference immediately, so
		// close/reopen needs no second manual save after attachment.
		if (renderTagsPanel(building, gBuildingFilepath,
			[] { return chooseAgentTagRegistryPath(); }))
			saveBuilding(building, false);
	}

	if (ImGui::CollapsingHeader("Behaviours"))
	{
		// Behaviour registry packages are inspected and reloaded here; their
		// definitions are authored beside the Building and never executed or
		// edited by the panel.
		ImGui::PushID("AgentBehaviourRegistry");
		if (renderBehavioursPanel(building, gBuildingFilepath,
			[] { return chooseAgentBehaviourRegistryPath(); }))
			saveBuilding(building, false);
		ImGui::PopID();
	}

	if (ImGui::CollapsingHeader("Objects"))
	{
		renderObjectView(building);
	}

	if (ImGui::CollapsingHeader("Agents"))
	{
		// Groups are defined here, above the Agents they will classify, so the
		// definitions read before the rows that use them.
		renderAgentGroupsPanel(building);
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


void renderPathingPanel(shared_ptr<core::Building> const& building,
	shared_ptr<const core::Agent> /* agent */)
{
		string selectedVertexText = format("Selected vertex: {}", gSelectedVertex ? gSelectedVertex->getDescription() : "<none>");
		string hoveredVertexText = format("Hovered vertex: {}", gHoveredVertex ? gHoveredVertex->getDescription() : "<none>");

		ImGui::TextUnformatted(selectedVertexText.c_str());
		ImGui::TextUnformatted(hoveredVertexText.c_str());

		shared_ptr<core::Path> path = gSelectedAgent ? gSelectedAgent->getPath() : nullptr;

		if (path)
		{
			auto agentIsIdle = gSelectedAgent->getState() == core::Agent::State::Idle;
			auto const id = building ? building->getAgentId(gSelectedAgent) : core::AgentId{};
			auto const behaviourOwnsMovement = building && id
				&& building->agentBehaviourOwnsMovement(id);
			ImGui::BeginDisabled(behaviourOwnsMovement);

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
			ImGui::EndDisabled();

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
	if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen))
		renderLayersPanel(building);
	if (ImGui::CollapsingHeader("Building", ImGuiTreeNodeFlags_DefaultOpen))
		renderBuildingPanel(building);
	if (ImGui::CollapsingHeader("Path finding"))
		renderPathingPanel(building, pathingAgent);
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

	// The Sector types the editor can resize by dragging one of their edges.
	bool isSectorTypeResizable(core::SectorType type)
	{
		return type == core::SectorType::Location
			|| type == core::SectorType::Background
			|| type == core::SectorType::Lift
			|| type == core::SectorType::Shuttle
			|| type == core::SectorType::Ladder
			|| type == core::SectorType::Stairwell;
	}

	ResizeEdge hoveredResizeEdge(shared_ptr<const core::Sector> const& sector, ImVec2 mouse)
	{
		if (!sector || !isSectorTypeResizable(sector->getType())) return ResizeEdge::None;
		auto topLeft = worldToScreen({ (float)sector->getCellX(),
			(float)(sector->getCellY() + sector->getDecksHigh()) });
		auto bottomRight = worldToScreen({ (float)(sector->getCellX() + sector->getCellsWide()),
			(float)sector->getCellY() });
		constexpr float tolerance = 6.0f;
		struct Candidate { ResizeEdge edge; float distance; };
		vector<Candidate> candidates;
		if (sector->getType() != core::SectorType::Ladder
			&& sector->getType() != core::SectorType::Stairwell
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
		if (!gAgentMove.dragging && gWorldHovered && !gViewPan.dragging && gHoveredAgent
			&& io.MouseClicked[0])
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

	// A Window resizes from any of its four edges.
	ResizeEdge hoveredWindowResizeEdge(shared_ptr<const core::SectorObject> const& object,
		ImVec2 mouse)
	{
		if (!object || object->getObjectType() != core::SectorObjectType::Window)
			return ResizeEdge::None;
		auto topLeft = worldToScreen({ (float)object->getCellX(),
			(float)object->getCellY() + object->getSize().y });
		auto bottomRight = worldToScreen({
			(float)object->getCellX() + object->getSize().x, (float)object->getCellY() });
		constexpr float handleRadius = 6.0f;
		if (mouse.x < topLeft.x - handleRadius || mouse.x > bottomRight.x + handleRadius
			|| mouse.y < topLeft.y - handleRadius || mouse.y > bottomRight.y + handleRadius)
			return ResizeEdge::None;
		struct Candidate { ResizeEdge edge; float distance; };
		Candidate candidates[] = {
			{ ResizeEdge::Left, abs(mouse.x - topLeft.x) },
			{ ResizeEdge::Right, abs(mouse.x - bottomRight.x) },
			{ ResizeEdge::Top, abs(mouse.y - topLeft.y) },
			{ ResizeEdge::Bottom, abs(mouse.y - bottomRight.y) }
		};
		auto closest = min_element(begin(candidates), end(candidates),
			[](auto const& left, auto const& right) { return left.distance < right.distance; });
		return closest->distance <= handleRadius ? closest->edge : ResizeEdge::None;
	}

	// A Door resizes horizontally only. Physical regular/tall height is selected
	// in the Selection panel and does not change its one-deck grid footprint.
	ResizeEdge hoveredDoorResizeEdge(shared_ptr<const core::SectorObject> const& object,
		ImVec2 mouse)
	{
		if (!object || object->getObjectType() != core::SectorObjectType::Door)
			return ResizeEdge::None;
		auto topLeft = worldToScreen({ (float)object->getCellX(),
			(float)object->getCellY() + object->getSize().y });
		auto bottomRight = worldToScreen({
			(float)object->getCellX() + object->getSize().x, (float)object->getCellY() });
		constexpr float handleRadius = 6.0f;
		if (mouse.x < topLeft.x - handleRadius || mouse.x > bottomRight.x + handleRadius
			|| mouse.y < topLeft.y - handleRadius || mouse.y > bottomRight.y + handleRadius)
			return ResizeEdge::None;
		struct Candidate { ResizeEdge edge; float distance; };
		Candidate candidates[] = {
			{ ResizeEdge::Left, abs(mouse.x - topLeft.x) },
			{ ResizeEdge::Right, abs(mouse.x - bottomRight.x) }
		};
		auto closest = min_element(begin(candidates), end(candidates),
			[](auto const& left, auto const& right) { return left.distance < right.distance; });
		return closest->distance <= handleRadius ? closest->edge : ResizeEdge::None;
	}

	ResizeEdge hoveredObjectResizeEdge(shared_ptr<const core::SectorObject> const& object,
		ImVec2 mouse)
	{
		if (!object) return ResizeEdge::None;
		if (object->getObjectType() == core::SectorObjectType::Window)
			return hoveredWindowResizeEdge(object, mouse);
		if (object->getObjectType() == core::SectorObjectType::Door)
			return hoveredDoorResizeEdge(object, mouse);
		return ResizeEdge::None;
	}

	// The cells the selection occupies when the editor can resize it by
	// dragging: a resizable Sector, or a Window or Door SectorObject. False
	// when the selection cannot be resized, or is not on the Layer being drawn.
	bool selectedResizeFootprint(uint32_t& cellX, uint32_t& cellY, uint32_t& cellsWide,
		uint32_t& decksHigh)
	{
		if (gUISettings.selectionMode == UISettings::SelectionMode::Sector)
		{
			if (!gSelectedSector
				|| !shouldDrawCanvasSectorEditOverlay(gSelectedSector->getLayerIndex(),
					(uint32_t)gUISettings.visibleLayer)
				|| !isSectorTypeResizable(gSelectedSector->getType()))
				return false;
			cellX = gSelectedSector->getCellX();
			cellY = gSelectedSector->getCellY();
			cellsWide = gSelectedSector->getCellsWide();
			decksHigh = gSelectedSector->getDecksHigh();
			return true;
		}
		if (gUISettings.selectionMode != UISettings::SelectionMode::Object
			|| !gSelectedSectorObject)
			return false;
		auto const objectType = gSelectedSectorObject->getObjectType();
		if (objectType != core::SectorObjectType::Window
			&& objectType != core::SectorObjectType::Door)
			return false;
		// A Window or Door is reachable from either Layer of the pair it crosses.
		auto const visible = (uint32_t)gUISettings.visibleLayer;
		uint32_t frontLayer = gSelectedSectorObject->getSector()->getLayerIndex();
		uint32_t backLayer = frontLayer;
		if (objectType == core::SectorObjectType::Window)
		{
			auto const window = static_pointer_cast<const core::WindowSectorObject>(
				gSelectedSectorObject)->getWindow();
			if (!window) return false;
			frontLayer = window->getFrontLayer();
			backLayer = window->getBackLayer();
		}
		else
		{
			auto const door = static_pointer_cast<const core::DoorSectorObject>(
				gSelectedSectorObject)->getDoor();
			if (!door) return false;
			frontLayer = door->getFrontLayer();
			backLayer = door->getBackLayer();
		}
		if (gSelectedSectorObject->getSector()->getLayerIndex() != visible
			&& frontLayer != visible && backLayer != visible)
			return false;
		cellX = gSelectedSectorObject->getCellX();
		cellY = gSelectedSectorObject->getCellY();
		cellsWide = (uint32_t)ceil(gSelectedSectorObject->getSize().x);
		decksHigh = (uint32_t)ceil(gSelectedSectorObject->getSize().y);
		return true;
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
			// A Window is reachable from either Layer of the pair it crosses.
			objectOnVisibleLayer = objectOnVisibleLayer
				|| window->getFrontLayer() == (uint32_t)gUISettings.visibleLayer
				|| window->getBackLayer() == (uint32_t)gUISettings.visibleLayer;
		}
		else if (gSelectedSectorObject
			&& gSelectedSectorObject->getObjectType() == core::SectorObjectType::Door)
		{
			auto door = static_pointer_cast<const core::DoorSectorObject>(
				gSelectedSectorObject)->getDoor();
			// A Door is reachable from either Layer of the pair it crosses.
			objectOnVisibleLayer = door && (objectOnVisibleLayer
				|| door->getFrontLayer() == (uint32_t)gUISettings.visibleLayer
				|| door->getBackLayer() == (uint32_t)gUISettings.visibleLayer);
		}
		if (gUISettings.selectionMode != UISettings::SelectionMode::Object
			|| !gSelectedSectorObject || !objectOnVisibleLayer)
		{
			resetObjectMove();
			return;
		}

		if (building->isLiftOwnedDoor(gSelectedSectorObject)
			|| building->isBulkheadDoorOwnedControl(gSelectedSectorObject)
			|| building->isLiftOwnedControl(gSelectedSectorObject)
			|| building->isShuttleOwnedDoor(gSelectedSectorObject)
			|| building->isShuttleOwnedControl(gSelectedSectorObject)
			|| building->isForceBridgeOwnedControl(gSelectedSectorObject))
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

		auto resizeEdge = gObjectMove.dragging ? gObjectMove.edge
			: gWorldHovered ? hoveredObjectResizeEdge(gSelectedSectorObject, io.MousePos)
				: ResizeEdge::None;
		if (resizeEdge == ResizeEdge::Left || resizeEdge == ResizeEdge::Right)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		else if (resizeEdge == ResizeEdge::Top || resizeEdge == ResizeEdge::Bottom)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		if (!gObjectMove.dragging && gWorldHovered && !gViewPan.dragging
			&& (gHoveredSectorObject == gSelectedSectorObject || resizeEdge != ResizeEdge::None)
			&& io.MouseClicked[0])
			beginObjectMove(building, gSelectedSectorObject, objectIndex,
				resizeEdge == ResizeEdge::None ? ResizeEdge::Move : resizeEdge);
		if (!gObjectMove.dragging) return;

		if (gObjectMove.edge == ResizeEdge::Move)
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
		int targetWidth = (int)gObjectMove.originalWidth;
		int targetHeight = (int)gObjectMove.originalHeight;
		bool const resizing = gObjectMove.edge != ResizeEdge::Move;
		bool const doorResize = resizing
			&& gSelectedSectorObject->getObjectType() == core::SectorObjectType::Door;
		// A regular Door may span at most two cells and always has a one-deck
		// footprint; physical regular/tall height is not changed by dragging.
		int const maxResizeWidth = doorResize ? 2 : (int)building->getCellsWide();
		int const maxResizeHeight = (int)building->getDecksHigh();
		if (gObjectMove.edge == ResizeEdge::Left)
		{
			auto right = (int)gObjectMove.originalX + (int)gObjectMove.originalWidth;
			targetX = clamp((int)gObjectMove.originalX + deltaX,
				max(0, right - maxResizeWidth), right - 1);
			targetY = (int)gObjectMove.originalY;
			targetWidth = right - targetX;
		}
		else if (gObjectMove.edge == ResizeEdge::Right)
		{
			auto right = clamp((int)gObjectMove.originalX + (int)gObjectMove.originalWidth
				+ deltaX, (int)gObjectMove.originalX + 1,
				min(maxResizeWidth + (int)gObjectMove.originalX, (int)building->getCellsWide()));
			targetX = (int)gObjectMove.originalX;
			targetY = (int)gObjectMove.originalY;
			targetWidth = right - targetX;
		}
		else if (gObjectMove.edge == ResizeEdge::Bottom)
		{
			auto top = (int)gObjectMove.originalY + (int)gObjectMove.originalHeight;
			targetX = (int)gObjectMove.originalX;
			targetY = clamp((int)gObjectMove.originalY + deltaY,
				max(0, top - maxResizeHeight), top - 1);
			targetHeight = top - targetY;
		}
		else if (gObjectMove.edge == ResizeEdge::Top)
		{
			auto top = clamp((int)gObjectMove.originalY + (int)gObjectMove.originalHeight
				+ deltaY, (int)gObjectMove.originalY + 1,
				min(maxResizeHeight + (int)gObjectMove.originalY, (int)building->getDecksHigh()));
			targetX = (int)gObjectMove.originalX;
			targetY = (int)gObjectMove.originalY;
			targetHeight = top - targetY;
		}
		bool const targetInWorld = targetX >= 0 && targetY >= 0
			&& targetX < (int)building->getCellsWide() && targetY < (int)building->getDecksHigh();
		if (!targetInWorld)
		{
			gObjectMove.preview.valid = false;
			gObjectMove.preview.diagnostic = "Drop the object inside the world";
		}
		else if (gObjectMove.preview.x != (uint32_t)targetX
			|| gObjectMove.preview.y != (uint32_t)targetY
			|| (resizing && (gObjectMove.preview.previewWidth != (uint32_t)targetWidth
				|| gObjectMove.preview.previewHeight != (uint32_t)targetHeight))
			|| gObjectMove.preview.diagnostic == "Drop the object inside the world")
		{
			gObjectMove.preview = doorResize
				? building->planResizeSectorDoor(owner->getIndex(), objectIndex,
					(uint32_t)targetX, (uint32_t)targetY,
					(uint32_t)targetWidth, (uint32_t)targetHeight)
				: resizing
					? building->planResizeSectorWindow(owner->getIndex(), objectIndex,
						(uint32_t)targetX, (uint32_t)targetY,
						(uint32_t)targetWidth, (uint32_t)targetHeight)
					: building->planMoveSectorObject(owner->getIndex(), objectIndex,
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
				&& gObjectMove.preview.y == gObjectMove.originalY
				&& gObjectMove.preview.previewWidth == gObjectMove.originalWidth
				&& gObjectMove.preview.previewHeight == gObjectMove.originalHeight)
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
			queueObjectMove(building, gObjectMove.preview);
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

		// A Background takes the same rectangle gesture as a Location, but its plan
		// has to come from the Background editor so the Windows which lose it are
		// named in the consequences rather than silently dropped by the replay.
		auto const background = gSelectedSector->getType() == core::SectorType::Background;
		auto previewRectangle = [&building, background](uint32_t sectorIndex,
			uint32_t left, uint32_t bottom, uint32_t width, uint32_t height)
		{
			return background
				? building->planResizeBackground(sectorIndex, left, bottom, width, height)
				: building->planResizeLocation(sectorIndex, left, bottom, width, height);
		};

		auto hoverEdge = gSectorResize.dragging ? gSectorResize.edge
			: gHoveredSector == gSelectedSector
				? hoveredResizeEdge(gSelectedSector, io.MousePos) : ResizeEdge::None;
		if (hoverEdge == ResizeEdge::Left || hoverEdge == ResizeEdge::Right)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		else if (hoverEdge == ResizeEdge::Top || hoverEdge == ResizeEdge::Bottom)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
		else if (hoverEdge == ResizeEdge::Move)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

		if (!gSectorResize.dragging && gWorldHovered && !gViewPan.dragging
			&& hoverEdge != ResizeEdge::None && io.MouseClicked[0])
		{
			bool const selectedLift = gSelectedSector->getType() == core::SectorType::Lift;
			bool const selectedShuttle = gSelectedSector->getType() == core::SectorType::Shuttle;
			bool const selectedLadder = gSelectedSector->getType() == core::SectorType::Ladder;
			bool const selectedStairwell = gSelectedSector->getType() == core::SectorType::Stairwell;
			if (hoverEdge != ResizeEdge::Move && !selectedLift && !selectedShuttle)
			{
				if (!building->isSimulationPaused()) building->pauseSimulation();
				gUISettings.worldPaused = true;
			}
			gSectorResize.dragging = true;
			gSectorResize.lift = selectedLift;
			gSectorResize.shuttle = selectedShuttle;
			gSectorResize.ladder = selectedLadder;
			gSectorResize.stairwell = selectedStairwell;
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
			else if (gSectorResize.stairwell)
			{
				core::Building::CreateStairwellOptions options{
					gSectorResize.originalHeight, CORE_SIDE_LEFT };
				building->getStairwellOptions(gSelectedSector->getIndex(), options);
				gSectorResize.stairwellPreview = building->planResizeStairwell(
					gSelectedSector->getIndex(), gSectorResize.originalX,
					gSectorResize.originalY, options);
			}
			else
				gSectorResize.preview = previewRectangle(gSelectedSector->getIndex(),
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
			 top - ((gSectorResize.ladder || gSectorResize.stairwell) ? 2 : 1)); break;
		case ResizeEdge::Top: moving = &top; desired = clamp(top + deltaY,
			bottom + ((gSectorResize.ladder || gSectorResize.stairwell) ? 2 : 1),
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
		else if (gSectorResize.stairwell)
		{
			if (gSectorResize.stairwellPreview.x != (uint32_t)left
				|| gSectorResize.stairwellPreview.y != (uint32_t)bottom
				|| gSectorResize.stairwellPreview.options.decksHigh != (uint32_t)(top - bottom))
			{
				auto options = gSectorResize.stairwellPreview.options;
				options.decksHigh = (uint32_t)(top - bottom);
				gSectorResize.stairwellPreview = building->planResizeStairwell(
					gSelectedSector->getIndex(), (uint32_t)left, (uint32_t)bottom, options);
			}
		}
		else if (gSectorResize.preview.x != (uint32_t)left
			|| gSectorResize.preview.y != (uint32_t)bottom
			|| gSectorResize.preview.cellsWide != (uint32_t)(right - left)
			|| gSectorResize.preview.decksHigh != (uint32_t)(top - bottom))
		{
			gSectorResize.preview = previewRectangle(gSelectedSector->getIndex(),
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
			else if (gSectorResize.stairwell && !gSectorResize.stairwellPreview.valid)
			{
				core::addLogMessage("Stairwell editor", 0, core::LogLevel::Error,
					gSectorResize.stairwellPreview.diagnostic);
				resetSectorResize();
			}
			else if (!gSectorResize.lift && !gSectorResize.shuttle && !gSectorResize.ladder
				&& !gSectorResize.stairwell && !gSectorResize.preview.valid)
			{
				core::addLogMessage("Sector editor", 0, core::LogLevel::Error,
					gSectorResize.preview.diagnostic);
				resetSectorResize();
			}
			else if (gSectorResize.lift) queueLiftEdit(building, gSectorResize.liftPreview);
			else if (gSectorResize.shuttle) queueShuttleEdit(building, gSectorResize.shuttlePreview);
			else if (gSectorResize.ladder) queueLadderEdit(building, gSectorResize.ladderPreview);
			else if (gSectorResize.stairwell) queueStairwellEdit(building, gSectorResize.stairwellPreview);
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
			&& !gPendingStairwellEdit)
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
			// The interior of a selected Sector needs no box of its own: the
			// 1px footprint box drawn below already marks the cells a drag
			// works on.
			case ResizeEdge::Move:
			case ResizeEdge::None: break;
			}
		}

		// The whole footprint of the selected resizable object is boxed in
		// yellow while the cursor is inside it, showing the cells a drag works
		// on.
		if (gWorldHovered && !gSectorResize.dragging && !gObjectMove.dragging
			&& !gAgentMove.dragging)
		{
			uint32_t cellX{ 0 }, cellY{ 0 }, cellsWide{ 0 }, decksHigh{ 0 };
			if (selectedResizeFootprint(cellX, cellY, cellsWide, decksHigh))
			{
				auto const world = screenToWorld(ImGui::GetIO().MousePos);
				auto const hoverX = (int)floor(world.x);
				auto const hoverY = (int)floor(world.y);
				if (hoverX >= (int)cellX && hoverX < (int)(cellX + cellsWide)
					&& hoverY >= (int)cellY && hoverY < (int)(cellY + decksHigh))
					drawList->AddRect(
						worldToScreen({ (float)cellX, (float)(cellY + decksHigh) }),
						worldToScreen({ (float)(cellX + cellsWide), (float)cellY }),
						IM_COL32(255, 255, 0, 255), 0.0f, 0, 1.0f);
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
		else if (gSectorResize.stairwell && (gSectorResize.dragging
			|| gSectorResize.stairwellPreview.decksHigh))
		{
			auto const& plan = gSectorResize.stairwellPreview;
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
		else if (gPendingStairwellEdit)
		{
			auto const& plan = *gPendingStairwellEdit;
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

	// Drag-scrolling the view. The gesture is claimed by the modifiers held
	// when the button goes down, so the click never reaches the world or the
	// palette underneath it. The world is grabbed rather than pushed: dragging
	// right reveals what lies to the left. An axis with nowhere to go simply
	// does not move.
	auto const& io = ImGui::GetIO();
	bool const canPan = horizontalScrollMax > 0.0f || verticalScrollMax > 0.0f;
	bool const panGesture = io.KeyShift
		&& (io.MouseDown[ImGuiMouseButton_Middle]
			|| (io.KeyAlt && io.MouseDown[ImGuiMouseButton_Left]));
	if (!gViewPan.dragging && gWorldHovered && canPan && panGesture
		&& (io.MouseClicked[ImGuiMouseButton_Middle]
			|| io.MouseClicked[ImGuiMouseButton_Left]))
	{
		gViewPan.dragging = true;
		gViewPan.pressPosition = io.MousePos;
		gViewPan.scrollAtPress = { scrollX, scrollY };
	}
	if (gViewPan.dragging)
	{
		if (!panGesture) gViewPan.dragging = false;
		else
		{
			auto const drag = io.MousePos - gViewPan.pressPosition;
			scrollX = clamp(gViewPan.scrollAtPress.x - drag.x, 0.0f, horizontalScrollMax);
			scrollY = clamp(gViewPan.scrollAtPress.y - drag.y, 0.0f, verticalScrollMax);
		}
	}

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
		// While picking an agent's path destination only vertices are selectable,
		// with a slightly enlarged target radius to make them easier to pick.
		float vertexRadius = RENDER_VERTEX_SIZE / (float)CORE_DECK_HEIGHT_PIXELS;
		if (gSelectingAgentPathDestination) vertexRadius *= 1.5f;
		if (gUISettings.renderGraph)
			gHoveredVertex = graph->getVertexAtPosition(gUISettings.visibleLayer, mousePos.x,
				mousePos.y, vertexRadius);
		if (!gSelectingAgentPathDestination && !gHoveredVertex)
			gHoveredAgent = building->getAgentAtPosition(gUISettings.visibleLayer,
				mousePos.x, mousePos.y);
		if (!gHoveredVertex && !gHoveredAgent)
		{
			gHoveredSectorObject = markerAtScreenPosition(building, ImGui::GetIO().MousePos);
			if (!gHoveredSectorObject)
				gHoveredSectorObject = bulkheadDoorAtScreenPosition(building, ImGui::GetIO().MousePos);
			if (!gHoveredSectorObject)
				gHoveredSectorObject = ladderAtScreenPosition(building, ImGui::GetIO().MousePos);
			if (!gHoveredSectorObject)
				gHoveredSectorObject = platformLiftAtScreenPosition(building, ImGui::GetIO().MousePos);
			if (!gHoveredSectorObject)
				gHoveredSectorObject = forceBridgeAtScreenPosition(building, ImGui::GetIO().MousePos);
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
		if (!gSelectingAgentPathDestination && !gHoveredVertex && !gHoveredAgent
			&& !gHoveredSectorObject)
		{
			auto sector = building->getSectorAtPosition(gUISettings.visibleLayer,
				mousePos.x, mousePos.y);
			if (sector && isCanvasSelectableSectorType(sector->getType()))
				gHoveredSector = sector;
		}

		if (gHoveredAgent || gHoveredSector || gHoveredSectorObject || gHoveredVertex)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}

	// Set after the hover tests so a pan outranks the Hand cursor.
	if (gViewPan.dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

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
			&& gSelectedSectorObject->getObjectType() == core::SectorObjectType::BulkheadDoor)
			drawList->AddLine(
				worldToScreen({ (float)plan.x, (float)plan.y + CORE_CORRIDOR_HEIGHT }),
				worldToScreen({ (float)plan.x, (float)plan.y }), colour, 5.0f);
		else if (gSelectedSectorObject
			&& (gSelectedSectorObject->getObjectType() == core::SectorObjectType::Walkway
				|| gSelectedSectorObject->getObjectType() == core::SectorObjectType::ForceBridge))
			drawList->AddLine(worldToScreen({ (float)plan.x, (float)plan.y }),
				worldToScreen({ (float)plan.x + width, (float)plan.y }), colour, 3.0f);
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
	renderStatusBar(building);
	renderControlsWindow(building, graph, pathingAgent);
	renderWorldWindow(building, graph);
}
