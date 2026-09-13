#include <deque>

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

#include "core/Vector2.h"
#include "core/Button.h"
#include "core/Door.h"
#include "core/BulkheadDoor.h"
#include "core/ForceBridge.h"
#include "core/Ladder.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/Log.h"

#include "Main.h"
#include "UI.h"
#include "Render.h"
#include "UISettings.h"
#include "Helpers.h"
#include "Exceptions.h"


extern spdlog::logger* gLogger;

extern UISettings gUISettings;

core::InteractionPointId gHoveredInteractionPoint;
core::Agent *gHoveredAgent{ nullptr }, *gSelectedAgent{ nullptr };
std::shared_ptr<const core::Vertex> gHoveredVertex, gSelectedVertex;
std::shared_ptr<const core::Sector> gSelectedSector;
std::shared_ptr<const core::SectorObject> gHoveredSectorObject, gSelectedSectorObject;

static std::deque<core::LogMessage> gLogMessages;

using namespace std;


// Are we interacting with ImGui widgets or the background?
bool mouseInteractingWithBackground()
{
	auto const& io = ImGui::GetIO();
	return !io.WantCaptureMouse;
}


MouseButtonStatus getMouseButtonStatus()
{
	MouseButtonStatus status;

	auto const& io = ImGui::GetIO();

	static ImVec2 frameDragDelta[2];

	if (!io.WantCaptureMouse)
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

void handleShortcuts(shared_ptr<core::Building> building)
{
	// World pause
	if (ImGui::Shortcut(ImGuiKey_P, 0, ImGuiInputFlags_RouteGlobalLow))
	{
		if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused())
		{
			gUISettings.worldPaused = !gUISettings.worldPaused;
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
	if (mouseStatus.state[MouseButtonStatus::Left] == MouseButtonStatus::State::Clicked)
	{
		// Try and select
		if (gHoveredAgent)
		{
			gSelectedAgent = gHoveredAgent;
		}
		else if (gHoveredInteractionPoint)
		{
			if (!gUISettings.worldPaused && gSelectedAgent)
			{
				auto actor = building->getAgentId(gSelectedAgent);
				if (actor) building->requestInteraction(gHoveredInteractionPoint, actor);
			}
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

	if (mouseStatus.state[MouseButtonStatus::Right] == MouseButtonStatus::State::Clicked)
	{
		clearSelections();
	}

	if (mouseStatus.dragging[MouseButtonStatus::Left])
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

void renderMenu(shared_ptr<const core::Building> building)
{
	if (ImGui::BeginMainMenuBar())
	{
		if (ImGui::BeginMenu("File"))
		{
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
	ImGuiIO& io = ImGui::GetIO();

	auto windowFlags = 0
		| ImGuiWindowFlags_NoDecoration;

	ImGui::SetNextWindowPos(ImVec2(0, gMainMenuWindowSize.y));
	ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 35));

	if (ImGui::Begin("Toolbar", nullptr, windowFlags))
	{
		if (ImGui::Button(gUISettings.worldPaused ? "Resume" : "Pause"))
		{
			gUISettings.worldPaused = !gUISettings.worldPaused;
		}

		ImGui::SameLine();

		if (ImGui::Button("Wake Agents"))
		{
			building->wakeAllAgents();
		}

		ImGui::SameLine();

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
		ImGui::SameLine();

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
		ImGui::SameLine();
		imgui::ToggleButton("ToggleNonVisibleLayer", "Show non-visible layer", &gUISettings.renderNonVisibleLayer);

		ImGui::SameLine();
		imgui::ToggleButton("ToggleGraph", "Building graph", &gUISettings.renderGraph);

		ImGui::SameLine();
		imgui::ToggleButton("ToggleNearestVertex", "Highlight nearest vertex", &gUISettings.highlightNearestVertex);

		ImGui::SameLine();
		imgui::ToggleButton("Agent Debug", "Agent debug", &gUISettings.renderAgentDebug);

		ImGui::End();
	}
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
			// Scrollbar
			static float scrollX = 0.0f;
			float scrollMax = (float)building->getCellsWide() * (float)CORE_CELL_WIDTH_PIXELS - (float)APP_WINDOW_WIDTH;

			if (scrollMax > 0)
			{
				if (ImGui::SliderFloat("##Scroll", &scrollX, 0, scrollMax))
				{
					gUISettings.xOffset = -scrollX;
				}
			}

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

			ImGui::EndMenuBar();
		}

		ImGui::End();
	}
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
				}
			}
		}
	}
}


void renderBuildingWindow(shared_ptr<const core::Building> building)
{
	string layerNames[2] = { "Fore Layer", "Back Layer" };

	if (ImGui::Begin("Building"))
	{
		if (ImGui::CollapsingHeader("Objects", nullptr, 0))
		{
			renderObjectView(building);
		}

		if (ImGui::CollapsingHeader("Agents", nullptr, 0))
		{
			renderAgentView(building);
		}
	}

	ImGui::End();
}


void renderGraphWindow(shared_ptr<const core::Graph> graph)
{
	if (ImGui::Begin("Graph"))
	{
		if (ImGui::CollapsingHeader("Edges", nullptr, 0))
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

	ImGui::End();
}


void renderPathingWindow(shared_ptr<const core::Agent> agent)
{
	if (ImGui::Begin("Path finding"))
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

	ImGui::End();
}

void renderLogWindow()
{
	auto newMessages = core::consumeLogMessages();

	while (gLogMessages.size() + newMessages.size() > 1000)
	{
		gLogMessages.pop_front();
	}

	copy(newMessages.begin(), newMessages.end(), back_inserter(gLogMessages));

	// UI
	if (!ImGui::Begin("Log"))
	{
		ImGui::End();
		return;
	}

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

				auto text = format("{}\t{}\t\t{}", msg.sourceId, msg.source, msg.msg);

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
				ImGui::Text(msg.source.c_str(), 1, line_no);

				ImGui::TableSetColumnIndex(2);
				ImGui::Text(msg.msg.c_str(), 2, line_no);
			}
		}

		ImGui::EndTable();
	}

	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
	{
		ImGui::SetScrollHereY(1.0f);
	}

	ImGui::EndChild();

	ImGui::End();
}


void renderUI(shared_ptr<core::Building> building, shared_ptr<const core::Graph> graph, shared_ptr<core::Agent> pathingAgent)
{
	ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

	gHoveredAgent = nullptr;
	gHoveredInteractionPoint = {};
	gHoveredSectorObject = nullptr;
	gHoveredVertex = nullptr;

	if (mouseInteractingWithBackground())
	{
		auto mousePos = getMouseWorldPosition();

		switch (gUISettings.selectionMode)
		{
		case UISettings::SelectionMode::Object:
		{
			auto agent = building->getAgentAtPosition(gUISettings.visibleLayer, mousePos.x, mousePos.y);

			if (agent)
			{
				gHoveredAgent = agent;
			}
			else
			{
				shared_ptr<const core::SectorObject> sectorObj;
				auto object = building->getObjectAtPosition(gUISettings.visibleLayer,
					mousePos.x, mousePos.y, &sectorObj);
				gHoveredSectorObject = sectorObj;
				if (auto button = dynamic_pointer_cast<const core::Button>(object))
					gHoveredInteractionPoint = button->getInteractionPointId();
			}
			break;
		}

		case UISettings::SelectionMode::Vertex:
			gHoveredVertex = graph->getVertexAtPosition(gUISettings.visibleLayer, mousePos.x, mousePos.y, RENDER_VERTEX_SIZE / (float)CORE_DECK_HEIGHT_PIXELS);
			break;
		}

		if (gHoveredAgent || gHoveredSectorObject || gHoveredVertex)
		{
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		}
	}

	renderMenu(building);
	renderToolbar(building);
	renderStatusBar(building);
	renderBuildingWindow(building);
	renderPathingWindow(pathingAgent);
	renderGraphWindow(graph);
	renderLogWindow();
}