#include <algorithm>
#include <cfloat>
#include <cmath>
#include <deque>
#include <filesystem>
#include <set>

#pragma warning(push)
#pragma warning(disable: 4307)
#include <spdlog/spdlog.h>
#pragma warning(pop)
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/IconsFontAwesome5.h"

#include <nfd/nfd.h>

#include "core/Vector2.h"
#include "core/Button.h"
#include "core/Door.h"
#include "core/BulkheadDoor.h"
#include "core/ForceBridge.h"
#include "core/Ladder.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/Marker.h"
#include "core/Log.h"
#include "core/Exceptions.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

#include "Main.h"
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
std::shared_ptr<const core::Sector> gSelectedSector;
std::shared_ptr<const core::SectorObject> gHoveredSectorObject, gSelectedSectorObject;

static std::deque<core::LogMessage> gLogMessages;

using namespace std;


static bool gWorldHovered{ false };
static bool gPegmanConsumesLeftMouse{ false };

void setSelectionMode(UISettings::SelectionMode mode);

namespace
{
	constexpr float PaletteSlotSize{ 36.0f };
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
		Marker
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
	};

	struct PegmanTarget
	{
		shared_ptr<const core::Sector> sector;
		uint32_t deckOffset{ 0 };
		float localX{ 0.0f };
		float feetY{ 0.0f };
		float floorY{ 0.0f };
		string diagnostic;

		explicit operator bool() const { return sector != nullptr && diagnostic.empty(); }
	};

	PaletteDropState gPegman;

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

	shared_ptr<const core::SectorObject> markerAtScreenPosition(
		shared_ptr<const core::Building> const& building, ImVec2 position)
	{
		for (auto const& sector : building->getSectors(gUISettings.visibleLayer))
		{
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (object->getObjectType() != core::SectorObjectType::Marker) continue;
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

	void resetPegman()
	{
		gPegman.phase = PalettePhase::Home;
		gPegman.item = PaletteItem::None;
		gPegman.sector.reset();
		gPegman.velocity = 0.0f;
	}

	void setWorldPaused(shared_ptr<core::Building> const& building, bool paused)
	{
		if (paused)
		{
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

	void landPegman(shared_ptr<core::Building> const& building)
	{
		if (gUISettings.worldPaused && locationHasCapacity(gPegman.sector))
		{
			auto id = building->createAgent(nextAgentName(building), gPegman.sector->getIndex(),
				gPegman.deckOffset, gPegman.localX);
			auto created = building->lookupAgent(id).entity;
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = created;
			gSelectedSector.reset();
			gSelectedSectorObject.reset();
		}
		resetPegman();
	}

	void renderObjectPalette(shared_ptr<core::Building> const& building, ImVec2 canvasPos,
		ImVec2 canvasSize, ImDrawList* drawList)
	{
		constexpr ImU32 yellow = IM_COL32(251, 188, 4, 255);
		constexpr ImU32 red = IM_COL32(244, 67, 54, 255);
		constexpr ImU32 trayColour = IM_COL32(24, 24, 28, 210);
		constexpr ImU32 borderColour = IM_COL32(180, 180, 190, 180);
		auto const& io = ImGui::GetIO();
		gPegmanConsumesLeftMouse = false;

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
		auto traySize = ImVec2(PalettePadding * 2.0f + PaletteSlotSize * 2.0f + PaletteGap,
			PalettePadding * 2.0f + PaletteSlotSize);
		auto trayTopLeft = trayBottomRight - traySize;
		auto agentMin = trayTopLeft + ImVec2(PalettePadding, PalettePadding);
		auto markerMin = agentMin + ImVec2(PaletteSlotSize + PaletteGap, 0.0f);
		auto agentMax = agentMin + ImVec2(PaletteSlotSize, PaletteSlotSize);
		auto markerMax = markerMin + ImVec2(PaletteSlotSize, PaletteSlotSize);
		drawList->AddRectFilled(trayTopLeft, trayBottomRight, trayColour, 5.0f);
		drawList->AddRect(trayTopLeft, trayBottomRight, borderColour, 5.0f);

		PaletteItem hoveredItem = PaletteItem::None;
		if (gWorldHovered && gPegman.phase == PalettePhase::Home)
		{
			if (pointInRect(io.MousePos, agentMin, agentMax)) hoveredItem = PaletteItem::Agent;
			else if (pointInRect(io.MousePos, markerMin, markerMax)) hoveredItem = PaletteItem::Marker;
		}
		drawList->AddRect(agentMin, agentMax,
			hoveredItem == PaletteItem::Agent ? yellow : borderColour, 3.0f);
		drawList->AddRect(markerMin, markerMax,
			hoveredItem == PaletteItem::Marker ? yellow : borderColour, 3.0f);
		if (hoveredItem != PaletteItem::None)
		{
			gPegmanConsumesLeftMouse = true;
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImGui::SetTooltip(hoveredItem == PaletteItem::Agent
				? "Drag to add Agent" : "Drag to add Marker");
			if (io.MouseClicked[0])
			{
				gPegman.phase = PalettePhase::Armed;
				gPegman.item = hoveredItem;
				gPegman.pressPosition = io.MousePos;
			}
		}

		if (gPegman.phase == PalettePhase::Armed)
		{
			gPegmanConsumesLeftMouse = true;
			ImVec2 movement = io.MousePos - gPegman.pressPosition;
			if (io.MouseDown[0] && movement.x * movement.x + movement.y * movement.y
				>= io.MouseDragThreshold * io.MouseDragThreshold)
				gPegman.phase = PalettePhase::Dragging;
			else if (io.MouseReleased[0]) resetPegman();
		}

		PegmanTarget target;
		if (gPegman.phase == PalettePhase::Dragging)
		{
			gPegmanConsumesLeftMouse = true;
			target = gPegman.item == PaletteItem::Marker
				? getMarkerTarget(building, io.MousePos, canvasPos, canvasSize)
				: getPegmanTarget(building, io.MousePos, canvasPos, canvasSize);
			if (!gUISettings.worldPaused) target.diagnostic = "Pause simulation to place objects";
			if (ImGui::IsKeyPressed(ImGuiKey_Escape) || io.MouseClicked[1]) resetPegman();
			else if (io.MouseReleased[0])
			{
				if (target && gPegman.item == PaletteItem::Marker)
				{
					placeMarker(building, target);
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
	if (gUISettings.selectionMode != mode)
	{
		gUISettings.selectionMode = mode;
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
		gSelectedSectorObject = nullptr;
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
		Close
	};

	string gBuildingFilepath;
	PendingFileAction gPendingFileAction{ PendingFileAction::None };
	bool gOpenUnsavedChangesPopup{ false };
	bool gOpenNewBuildingPopup{ false };
	bool gOpenFileErrorPopup{ false };
	string gFileError;
	char gNewBuildingName[128]{ "Untitled" };
	int gNewBuildingWidth{ 48 };
	int gNewBuildingDecks{ 6 };

	void clearDocumentState()
	{
		gHoveredInteractionPoint = {};
		gHoveredAgent = nullptr;
		gSelectedAgent = nullptr;
		gHoveredVertex.reset();
		gSelectedVertex.reset();
		gSelectedSector.reset();
		gHoveredSectorObject.reset();
		gSelectedSectorObject.reset();
		resetPegman();
		gUISettings.worldPaused = false;
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

		try
		{
			auto loaded = make_shared<core::Building>("Loading", 1, 1);
			auto serializer = core::YamlSerializer::fromFile(selectedPath.get());
			serializer->deserialize();
			core::SerializationWorkData workData;
			loaded->deserialize(*serializer, workData);
			building = std::move(loaded);
			gBuildingFilepath = selectedPath.get();
			clearDocumentState();
			core::addLogMessage("File", 0, core::LogLevel::Info,
				"Opened Building from " + gBuildingFilepath);
		}
		catch (std::exception const& error)
		{
			reportFileError("Could not open Building: " + string(error.what()));
		}
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
		case PendingFileAction::Close:
			building.reset();
			gBuildingFilepath.clear();
			clearDocumentState();
			break;
		case PendingFileAction::None:
			break;
		}
	}

	void requestFileAction(PendingFileAction action, shared_ptr<core::Building>& building)
	{
		if (building && building->isModified())
		{
			gPendingFileAction = action;
			gOpenUnsavedChangesPopup = true;
			return;
		}
		executeFileAction(action, building);
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
	}
}

void handleShortcuts(shared_ptr<core::Building>& building)
{
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, 0, ImGuiInputFlags_RouteGlobalLow))
		requestFileAction(PendingFileAction::New, building);
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, 0, ImGuiInputFlags_RouteGlobalLow))
		requestFileAction(PendingFileAction::Open, building);
	if (building && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, 0, ImGuiInputFlags_RouteGlobalLow))
		saveBuilding(building, false);

	if (!building) return;

	// World pause
	if (ImGui::Shortcut(ImGuiKey_P, 0, ImGuiInputFlags_RouteGlobalLow))
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

	// Delete selected Agent. Stop any active path/traversal first so Building can
	// release its coordination state before destroying the entity.
	if (ImGui::Shortcut(ImGuiKey_Delete, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused() && gSelectedAgent)
		{
			auto id = building->getAgentId(gSelectedAgent);
			if (id)
			{
				auto selected = gSelectedAgent;
				selected->clearPath();
				if (building->removeAgent(id))
				{
					if (gHoveredAgent == selected) gHoveredAgent = nullptr;
					gSelectedAgent = nullptr;
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
		gUISettings.visibleLayer = 1 - gUISettings.visibleLayer;
	}
	if (ImGui::Shortcut(ImGuiKey_F3, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		gUISettings.renderNonVisibleLayer = !gUISettings.renderNonVisibleLayer;
	}
	if (ImGui::Shortcut(ImGuiKey_F4, 0, ImGuiInputFlags_RouteGlobalLow))
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
		// Try and select
		if (gHoveredAgent)
		{
			gSelectedAgent = gHoveredAgent;
			gSelectedSector.reset();
			gSelectedSectorObject.reset();
		}
		else if (gHoveredInteractionPoint)
		{
			if (!gUISettings.worldPaused && gSelectedAgent)
			{
				auto actor = building->getAgentId(gSelectedAgent);
				if (actor) building->requestInteraction(gHoveredInteractionPoint, actor);
			}
		}
		else if (gHoveredSectorObject)
		{
			setSelectionMode(UISettings::SelectionMode::Object);
			gSelectedAgent = nullptr;
			gSelectedSector.reset();
			gSelectedSectorObject = gHoveredSectorObject;
		}
		else if (gHoveredVertex)
		{
			if (ImGui::GetIO().KeyCtrl)
			{
				if (gSelectedAgent)
				{
					auto path = graph->calculatePath(gSelectedAgent, nullptr, gHoveredVertex);
					gSelectedAgent->setPath(path, false);
				}
			}
			else
			{
				gSelectedVertex = gHoveredVertex;
			}
		}
	}

	if (!gPegmanConsumesLeftMouse
		&& mouseStatus.state[MouseButtonStatus::Right] == MouseButtonStatus::State::Clicked)
	{
		clearSelections();
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

void handleContinuousKeyboardInput(std::shared_ptr<core::Building> building, uint64_t updateTimeMicros)
{
	const float MoveSpeed{ 500.0f };

	auto const& io = ImGui::GetIO();

	if (io.WantCaptureKeyboard)
	{
		return;
	}

	float frameTime = updateTimeMicros / 1000000.0f;

	float moveSpeed = MoveSpeed * frameTime * (io.KeyShift ? 4.0f : 1.0f);

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
		ImGui::Text(title);

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


void exitApp()
{
	throw ExitApplicationException(0, "Exit");
}


ImVec2 gMainMenuWindowSize;

void renderMenu(shared_ptr<core::Building>& building)
{
	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New", "Ctrl+N"))
				requestFileAction(PendingFileAction::New, building);
			if (ImGui::MenuItem("Open...", "Ctrl+O"))
				requestFileAction(PendingFileAction::Open, building);
			if (ImGui::MenuItem("Save", "Ctrl+S", false, building != nullptr))
				saveBuilding(building, false);
			if (ImGui::MenuItem("Save As...", nullptr, false, building != nullptr))
				saveBuilding(building, true);
			if (ImGui::MenuItem("Close", nullptr, false, building != nullptr))
				requestFileAction(PendingFileAction::Close, building);
			ImGui::Separator();
			if (ImGui::MenuItem("Exit"))
			{
				exitApp();
			}

			ImGui::EndMenu();
		}
		
		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::BeginMenu("Selection"))
			{
				bool selected = gUISettings.style == UISettings::SelectionMode::Object;

				if (ImGui::MenuItem("Objects", 0, &selected))
				{
					if (selected)
					{
						setSelectionMode(UISettings::SelectionMode::Object);
					}
				}

				selected = gUISettings.style == UISettings::SelectionMode::Vertex;

				if (ImGui::MenuItem("Vertices", 0, &selected))
				{
					if (selected)
					{
						setSelectionMode(UISettings::SelectionMode::Vertex);
					}
				}

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
			"Vertices"
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

		// View
		imgui::ToggleButton("ToggleNonVisibleLayer", "Show non-visible layer", &gUISettings.renderNonVisibleLayer);
		imgui::ToggleButton("ToggleGraph", "Building graph", &gUISettings.renderGraph);
		imgui::ToggleButton("ToggleNearestVertex", "Highlight nearest vertex", &gUISettings.highlightNearestVertex);
		imgui::ToggleButton("Agent Debug", "Agent debug", &gUISettings.renderAgentDebug);
}


void renderStatusBar(shared_ptr<const core::Building> building)
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

			ImGui::Text(mouseData.c_str());

			if (gHoveredAgent)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(128);

				string objectData = format("{}", gHoveredAgent->getDescription());

				ImGui::Text(objectData.c_str());
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

				ImGui::Text(objectData.c_str());
			}

			// Keep status information on the left and the world scroll slider after it.
			static float scrollX = 0.0f;
			float viewportWidth = gUISettings.worldViewportWidth > 0.0f
				? gUISettings.worldViewportWidth : (float)APP_WINDOW_WIDTH;
			float scrollMax = (float)building->getCellsWide() * (float)CORE_CELL_WIDTH_PIXELS - viewportWidth;

			if (scrollMax > 0)
			{
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderFloat("##Scroll", &scrollX, 0, scrollMax))
				{
					gUISettings.xOffset = -scrollX;
				}
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


void renderDoorPanel(shared_ptr<const core::SectorObject> object)
{
	auto doorObject = static_pointer_cast<const core::DoorSectorObject>(object);
	auto door = doorObject->getDoor();

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


void renderLadderPanel(shared_ptr<const core::SectorObject> object)
{
	auto ladderObject = static_pointer_cast<const core::LadderSectorObject>(object);
	auto ladder = ladderObject->getLadder();

	float pct = ladder->getExtendedPercentage() * 100;

	// State
	switch (ladder->getState())
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
	ImGui::Text("Extend/retract time: %3.2f s", ladder->getExtendRetractTime());

	// Sectors
	ImGui::Text("Decks: %d", ladder->getDecksHigh());
}


void renderLiftPanel(shared_ptr<const core::Lift> lift)
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
			ImGui::Text(key.c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::Text(value.c_str());
		}

		ImGui::EndTable();
	}
}


void renderShuttlePanel(shared_ptr<const core::Shuttle> shuttle)
{
	ImGuiTableFlags flags =
		ImGuiTableFlags_SizingStretchSame |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_BordersOuter |
		ImGuiTableFlags_BordersV |
		ImGuiTableFlags_ContextMenuInBody;

	// Internals
	auto internals = shuttle->getInternalsStrings();

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
			ImGui::Text(key.c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::Text(value.c_str());
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
					ImGui::Text(sector->getDescription().c_str());

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
						ImGui::Text("%d/%d vertices", agent->getPathTargetNodeIndex(), path->nodes.size());
					}
					else
					{
						ImGui::Text("");
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

				bool nodeOpen = ImGui::TreeNodeEx((void*)(intptr_t)sector.get(), thisNodeFlags, text.c_str());

				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
				{
					selectedNode = (void*)sector.get();
					gSelectedAgent = nullptr;
					gSelectedSector = sector;
					gSelectedSectorObject = nullptr;
				}

				if (nodeOpen)
				{
					for (uint32_t i = 0; i < numObjects; ++i)
					{
						auto object = sector->getObject(i);

						thisNodeFlags = nodeFlags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

						if (((void*)object.get()) == selectedNode)
						{
							thisNodeFlags |= ImGuiTreeNodeFlags_Selected;
						}

						auto objectText = object->getDescription();
						ImGui::TreeNodeEx((void*)(intptr_t)object.get(), thisNodeFlags, objectText.c_str());

						if (ImGui::IsItemClicked())
						{
							selectedNode = (void*)object.get();
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

	if (gSelectedSector || gSelectedSectorObject)
	{
		if (ImGui::CollapsingHeader("Selection", nullptr, 0))
		{
			if (gSelectedSector)
			{
				switch (gSelectedSector->getType())
				{
				case core::SectorType::Lift:
					renderLiftPanel(static_pointer_cast<const core::LiftTransit>(gSelectedSector)->getLift());
					break;

				case core::SectorType::Shuttle:
					renderShuttlePanel(static_pointer_cast<const core::ShuttleTransit>(gSelectedSector)->getShuttle());
					break;
				}
			}
			else if (gSelectedSectorObject)
			{
				switch (gSelectedSectorObject->getObjectType())
				{
				case core::SectorObjectType::BulkheadDoor:
					renderBulkheadDoorPanel(gSelectedSectorObject);
					break;

				case core::SectorObjectType::Door:
					renderDoorPanel(gSelectedSectorObject);
					break;

				case core::SectorObjectType::ForceBridge:
					renderForceBridgePanel(gSelectedSectorObject);
					break;

				case core::SectorObjectType::Ladder:
					renderLadderPanel(gSelectedSectorObject);
					break;

				case core::SectorObjectType::Lift:
					renderLiftPanel(static_pointer_cast<const core::LiftSectorObject>(gSelectedSectorObject)->getLift());
					break;

				case core::SectorObjectType::Marker:
					renderMarkerPanel(gSelectedSectorObject);
					break;
				}
			}
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
				gSelectedAgent->clearPath();
				gSelectedAgent->setPath(newPath, true);
			}
		}
	}
}


void renderBuildingPanel(shared_ptr<const core::Building> building)
{
	if (ImGui::CollapsingHeader("Objects"))
	{
		renderObjectView(building);
	}

	if (ImGui::CollapsingHeader("Agents"))
	{
		renderAgentView(building);
	}

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
					ImGui::Text(edge->getDescription().c_str());

					ImGui::TableSetColumnIndex(1);
					ImGui::Text(edge->getVertex(0)->getDescription().c_str());

					ImGui::TableSetColumnIndex(2);
					ImGui::Text(edge->getVertex(0)->getSpec().c_str());

					ImGui::TableSetColumnIndex(3);
					ImGui::Text(edge->getVertex(1)->getDescription().c_str());

					ImGui::TableSetColumnIndex(4);
					ImGui::Text(edge->getVertex(1)->getSpec().c_str());
				}

				ImGui::EndTable();
			}
		}
}


void renderPathingPanel(shared_ptr<const core::Agent> agent)
{
		string selectedVertexText = format("Selected vertex: {}", gSelectedVertex ? gSelectedVertex->getDescription() : "<none>");
		string hoveredVertexText = format("Hovered vertex: {}", gHoveredVertex ? gHoveredVertex->getDescription() : "<none>");

		ImGui::Text(selectedVertexText.c_str());
		ImGui::Text(hoveredVertexText.c_str());

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
					ImGui::Text(edgeText.c_str(), 0, curRow);

					// Target Vertex
					ImGui::TableSetColumnIndex(1);
					ImGui::Text(pathVertexText.c_str(), 1, curRow);

					// Vertex Action
					ImGui::TableSetColumnIndex(2);
					ImGui::Text(vertexActionText.c_str(), 1, curRow);

					// Edge weight
					ImGui::TableSetColumnIndex(3);
					ImGui::Text(curWeightText.c_str(), 2, curRow);

					// Cumulative weight
					ImGui::TableSetColumnIndex(4);
					ImGui::Text(totalWeightText.c_str(), 3, curRow);

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

				if (msg.sourceId == ~0)
				{
					ImGui::TextColored(textColour, "--");
				}
				else
				{
					ImGui::TextColored(textColour, to_string(msg.sourceId).c_str());
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

void renderWorldWindow(shared_ptr<core::Building> building, shared_ptr<const core::Graph> graph)
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("World", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();

	ImVec2 canvasPos = ImGui::GetCursorScreenPos();
	ImVec2 canvasSize = ImGui::GetContentRegionAvail();
	canvasSize.x = max(canvasSize.x, 1.0f);
	canvasSize.y = max(canvasSize.y, 1.0f);

	gUISettings.worldViewportX = canvasPos.x;
	gUISettings.worldViewportY = canvasPos.y;
	gUISettings.worldViewportWidth = canvasSize.x;
	gUISettings.worldViewportHeight = canvasSize.y;

	ImGui::InvisibleButton("##WorldCanvas", canvasSize,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	gWorldHovered = ImGui::IsItemHovered();
	ImGui::SetItemAllowOverlap();

	gHoveredAgent = nullptr;
	gHoveredInteractionPoint = {};
	gHoveredSectorObject = nullptr;
	gHoveredVertex = nullptr;

	if (gWorldHovered)
	{
		auto mousePos = getMouseWorldPosition();
		if (gUISettings.selectionMode == UISettings::SelectionMode::Object)
		{
			gHoveredAgent = building->getAgentAtPosition(gUISettings.visibleLayer, mousePos.x, mousePos.y);
			if (!gHoveredAgent)
			{
				gHoveredSectorObject = markerAtScreenPosition(building, ImGui::GetIO().MousePos);
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
		}
		else
		{
			gHoveredVertex = graph->getVertexAtPosition(gUISettings.visibleLayer, mousePos.x,
				mousePos.y, RENDER_VERTEX_SIZE / (float)CORE_DECK_HEIGHT_PIXELS);
		}

		if (gHoveredAgent || gHoveredSectorObject || gHoveredVertex)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->PushClipRect(canvasPos, canvasPos + canvasSize, true);
	renderBuilding(building);
	renderGraph(graph, building);
	renderObjectPalette(building, canvasPos, canvasSize, drawList);
	drawList->PopClipRect();

	ImGui::End();
}

void renderUI(shared_ptr<core::Building>& building, shared_ptr<core::Agent> pathingAgent)
{
	ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

	renderMenu(building);
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
