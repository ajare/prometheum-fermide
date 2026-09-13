#include <cassert>
#include <cfloat>
#include <set>
#include <algorithm>

#include <Windows.h>
#include <gl/GL.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/IconsFontAwesome5.h"

#include "core/Defines.h"
#include "core/Building.h"
#include "core/Location.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/StaircaseTransit.h"
#include "core/ButtonSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/MarkerSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/Button.h"
#include "core/Marker.h"

#include "Main.h"
#include "Render.h"
#include "Helpers.h"
#include "UISettings.h"
#include "Exceptions.h"


extern UISettings gUISettings;
extern core::Agent* gHoveredAgent, *gSelectedAgent;
extern std::shared_ptr<const core::Vertex> gSelectedVertex;
extern std::shared_ptr<const core::Sector> gSelectedSector;
extern std::shared_ptr<const core::SectorObject> gHoveredSectorObject, gSelectedSectorObject;

extern GLuint gCellsTexture;
extern int gCellsTextureWidth;
extern int gCellsTextureHeight;
extern ImFont* gAgentIconFont;

using namespace std;

ImColor ForeLocationColour = ImColor(192, 192, 255);
ImColor BackLocationColour = ImColor(224, 224, 255);
ImColor LightsOffColour = ImColor(48, 48, 48);
ImColor LadderColour = ImColor(128, 128, 192);
ImColor LiftColour = ImColor(128, 128, 192);
ImColor ShuttleColour = ImColor(128, 128, 192);
ImColor StaircaseColour = ImColor(128, 128, 192);
ImColor VertexColour = ImColor(255, 128, 0);
ImColor EdgeColour = ImColor(255, 128, 0);
ImColor InterLayerEdgeColour = ImColor(255, 255, 64);
ImColor SelectedColour = ImColor(255, 255, 0);

#define RENDER_SECTOR_OBJECTS_BEHIND 1
#define RENDER_SECTOR_OBJECTS_INFRONT 2

void renderSector(shared_ptr<const core::Sector> sector, int layer, bool visibleLayer, bool wireframe, bool renderEdges, ImColor colour, ImDrawList* drawList);

void renderLadderTransit(shared_ptr<const core::LadderTransit> ladderTransit, int layer, bool visibleLayer, bool wireframe, ImColor colour, ImDrawList* drawList);

void renderLiftTransit(shared_ptr<const core::LiftTransit> liftTransit, int layer, bool visibleLayer, bool wireframe, ImColor colour, ImDrawList* drawList);

void renderShuttleTransit(shared_ptr<const core::ShuttleTransit> shuttleTransit, int layer, bool visibleLayer, bool wireframe, ImColor colour, ImDrawList* drawList);

void renderStaircaseTransit(shared_ptr<const core::StaircaseTransit> staircaseTransit, int layer, bool visibleLayer, bool wireframe, ImColor colour, ImDrawList* drawList);

void transformPosition(core::Vector2& p)
{
	p.x *= CORE_CELL_WIDTH_PIXELS;
	p.y *= CORE_DECK_HEIGHT_PIXELS;
	p.y = gUISettings.worldViewportY + gUISettings.worldViewportHeight - p.y;

	p.x += gUISettings.worldViewportX + gUISettings.xOffset;
	p.y -= gUISettings.yOffset;
}


void transformPosition(float& x, float& y)
{
	core::Vector2 p{ x, y };

	transformPosition(p);

	x = p.x;
	y = p.y;
}


void renderGrid(ImColor const& colour, float width, ImDrawList* drawList)
{
	core::Vector2 gridOffset;

	gridOffset.x = (float)fmod(-gUISettings.xOffset, CORE_CELL_WIDTH_PIXELS);
	gridOffset.y = (float)fmod(gUISettings.yOffset, CORE_DECK_HEIGHT_PIXELS);

	float xMin = gUISettings.worldViewportX;
	float yMin = gUISettings.worldViewportY;
	float xMax = xMin + gUISettings.worldViewportWidth;
	float yMax = yMin + gUISettings.worldViewportHeight;

	for (float x = xMin; x <= xMax; x += CORE_CELL_WIDTH_PIXELS)
	{
		drawList->AddLine(
			{ x - gridOffset.x, yMax },
			{ x - gridOffset.x, yMin },
			colour,
			width
		);
	}

	for (float y = yMin; y <= yMax; y += CORE_DECK_HEIGHT_PIXELS)
	{
		drawList->AddLine(
			{ xMin, yMax - (y - yMin + gridOffset.y) },
			{ xMax, yMax - (y - yMin + gridOffset.y) },
			colour,
			width
		);
	}
}


void renderGraph(shared_ptr<const core::Graph> graph, shared_ptr<const core::Building> building)
{
	if (!gUISettings.renderGraph)
	{
		return;
	}

	shared_ptr<core::Path> path = gSelectedAgent ? gSelectedAgent->getPath() : nullptr;

	auto layer = (uint32_t)gUISettings.visibleLayer;

	auto drawList = ImGui::GetWindowDrawList();

	auto const& vertices = graph->getVertices();
	auto const& edges = graph->getEdges();

	shared_ptr<const core::Vertex> closestVertex{ nullptr };

	if (gUISettings.highlightNearestVertex)
	{
		auto mousePos = getMouseWorldPosition();
		auto sector = building->getSectorAtPosition(layer, mousePos.x, mousePos.y);
	
		if (sector)
		{
			closestVertex = graph->getClosestVertexInSector(sector.get(), mousePos);
		}
	}

	// Increase the y position slightly so we can see floors, force bridges, etc
	const float yBump = -2;
	float lineThickness = -yBump + 1;

	for (auto const& edge : edges)
	{
		auto v0 = edge->getVertex(0);
		auto v1 = edge->getVertex(1);

		auto pos0 = v0->getPosition();
		auto pos1 = v1->getPosition();

		transformPosition(pos0);
		transformPosition(pos1);

		pos0.y += yBump;
		pos1.y += yBump;

		bool edgeIsInPath{ false };

		if (path)
		{
			auto edgeInPath = find_if(path->nodes.begin(), path->nodes.end(), [edge](auto node)
			{
				return node.edge && node.edge->sameAs(edge);
			});

			edgeIsInPath = edgeInPath != path->nodes.end();
		}

		auto interLayerEdgeColour = edgeIsInPath ? ImColor(255, 0, 0) : InterLayerEdgeColour;
		auto ladderMountColour = edgeIsInPath ? ImColor(255, 0, 0) : ImColor(0, 192, 255);
		auto liftMountColour = edgeIsInPath ? ImColor(255, 0, 0) : ImColor(0, 192, 255);
		auto edgeColour = edgeIsInPath ? ImColor(255, 0, 0) : EdgeColour;
		auto edgeThickness = edgeIsInPath ? 5.0f : lineThickness;

		if (edge->isInterLayer())
		{
			// If Edge is intra-Layer, then it's guaranteed that both Vertices have the same X/Y position.
			core::Vector2 posl = pos0, posr = pos0, posu = pos0;
			float sideSize = (float)(RENDER_INTER_LAYER_EDGE_SIZE / sin(60 * 3.14159 / 180.0));

			posu.y -= RENDER_INTER_LAYER_EDGE_SIZE;
			posl.x -= sideSize;
			posr.x += sideSize;

			drawList->AddTriangleFilled({ posl.x, posl.y }, { posr.x, posr.y }, { posu.x, posu.y }, interLayerEdgeColour);
		}
		else if (edge->getType() == core::EdgeType::LadderMount)
		{
			if (v0->getSector()->getLayerIndex() == layer || v1->getSector()->getLayerIndex() == layer)
			{
				core::Vector2 posl = pos0, posr = pos0, posu = pos0;
				float sideSize = (float)(RENDER_INTER_LAYER_EDGE_SIZE / sin(60 * 3.14159 / 180.0));

				posu.y -= RENDER_INTER_LAYER_EDGE_SIZE;
				posl.x -= sideSize;
				posr.x += sideSize;

				drawList->AddTriangleFilled({ posl.x, posl.y }, { posr.x, posr.y }, { posu.x, posu.y }, ladderMountColour);
			}
		}
		else if (edge->getType() == core::EdgeType::LiftMount)
		{
			if (v0->getSector()->getLayerIndex() == layer || v1->getSector()->getLayerIndex() == layer)
			{
				core::Vector2 posl = pos0, posr = pos0, posu = pos0;
				float sideSize = (float)(RENDER_INTER_LAYER_EDGE_SIZE / sin(60 * 3.14159 / 180.0));

				posu.y -= RENDER_INTER_LAYER_EDGE_SIZE;
				posl.x -= sideSize;
				posr.x += sideSize;

				drawList->AddTriangleFilled({ posl.x, posl.y }, { posr.x, posr.y }, { posu.x, posu.y }, liftMountColour);
			}
		}

		if (v0->getSector()->getLayerIndex() == layer && v1->getSector()->getLayerIndex() == layer)
		{
			drawList->AddLine({ pos0.x, pos0.y }, { pos1.x, pos1.y }, edgeColour, edgeThickness);
		}
	}

	for (auto vertex : vertices)
	{
		if (vertex->getSector()->getLayerIndex() != layer)
		{
			continue;
		}

		auto pos = vertex->getPosition();

		transformPosition(pos.x, pos.y);

		pos.y += yBump;

		bool vertexIsInPath{ false };
		bool pathCrossesLayers{ false };

		if (path)
		{
			auto vertexNodeIt = find_if(path->nodes.begin(), path->nodes.end(), [vertex](auto node)
			{
				return node.targetVertex->sameAs(vertex);
			});

			if (vertexNodeIt != path->nodes.end())
			{
				vertexIsInPath = true;

				if (vertexNodeIt->edge)
				{
					pathCrossesLayers = vertexNodeIt->edge->isInterLayer();
				}
			}
		}

		auto vertexColour = vertexIsInPath ? ImColor(255, 0, 0) : VertexColour;
		
		float outlineSize{ 2 };
		if (gSelectedVertex && vertex->sameAs(gSelectedVertex))
		{
			vertexColour = ImColor(255, 0, 0);

			drawList->AddRect(
				{ pos.x - RENDER_VERTEX_SIZE - outlineSize, pos.y - RENDER_VERTEX_SIZE - outlineSize },
				{ pos.x + RENDER_VERTEX_SIZE + outlineSize, pos.y + RENDER_VERTEX_SIZE + outlineSize },
				vertexColour
			);

			outlineSize += 2;
		}
		if (vertex == closestVertex)
		{
			vertexColour = ImColor(255, 128, 0);

			drawList->AddRect(
				{ pos.x - RENDER_VERTEX_SIZE - outlineSize, pos.y - RENDER_VERTEX_SIZE - outlineSize },
				{ pos.x + RENDER_VERTEX_SIZE + outlineSize, pos.y + RENDER_VERTEX_SIZE + outlineSize },
				vertexColour
			);

			outlineSize += 2;
		}

		drawList->AddCircleFilled({ pos.x, pos.y }, RENDER_VERTEX_SIZE, vertexColour);

	}
}


void renderDoorVertFromFloor(shared_ptr<const core::Door> door, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1, bounds2;

	door->getFullShape(bounds0, bounds2);

	bounds1 = bounds0;
	bounds1.y += door->getOpenPercentage() * CORE_DOOR_HEIGHT;

	transformPosition(bounds0);
	transformPosition(bounds1);
	transformPosition(bounds2);

	if (layer == CORE_LAYER_FORE)
	{
		auto doorColour = ImColor(64, 192, 255);
		drawList->AddRectFilled({ bounds1.x, bounds1.y }, { bounds2.x, bounds2.y }, doorColour);

		drawList->AddDrawCmd();

		// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ bounds1.x, bounds1.y }, { bounds2.x, bounds0.y }, true);

		auto backSector = door->getSector(CORE_LAYER_BACK);
		renderSector(backSector, layer, false, false, false, BackLocationColour, drawList);

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else
	{
		drawList->AddRect({ bounds1.x, bounds1.y }, { bounds2.x, bounds2.y }, ImColor(0, 0, 0));
	}
}


void renderDoorHorzFromCentre(shared_ptr<const core::Door> door, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	throw NotImplementedException("Lift-style Door rendering");
}


void renderDoorQuadIris(shared_ptr<const core::Door> door, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	throw NotImplementedException("Iris Door rendering");
}


void renderDoor(shared_ptr<const core::Door> door, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	// Don't render if we're rendering the back layer but it's not the visible one, because it
	// will be already rendered by the fore layer Location.
	if (layer != CORE_LAYER_BACK && !visibleLayer)
	{
		return;
	}

	auto openStyle = door->getOpenStyle();

	switch (openStyle)
	{
	case core::Door::OpenStyle::VertFromFloor:
		renderDoorVertFromFloor(door, layer, visibleLayer, selected, drawList);
		break;

	case core::Door::OpenStyle::HorzFromCentre:
		renderDoorHorzFromCentre(door, layer, visibleLayer, selected, drawList);
		break;

	case core::Door::OpenStyle::QuadIris:
		renderDoorQuadIris(door, layer, visibleLayer, selected, drawList);
		break;
	}

}


void renderBulkheadDoor(shared_ptr<const core::BulkheadDoor> door, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	door->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto doorColour = ImColor(128, 192, 182);
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, doorColour);
}


void renderWindowClear(shared_ptr<const core::Window> window, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	window->getFullShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	// Render Sector behind Window.
	auto backSector = window->getSector(CORE_LAYER_BACK);

	if (layer == CORE_LAYER_FORE || backSector)
	{
		drawList->AddDrawCmd();

		// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering.
		drawList->PushClipRect({ bounds0.x, bounds1.y }, { bounds1.x, bounds0.y }, true);

		if (backSector)
		{
			if (layer == CORE_LAYER_FORE)
			{
				renderSector(backSector, layer, false, false, false, BackLocationColour, drawList);
			}
			else
			{
				drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, ImColor(0, 0, 0));
			}
		}
		else
		{
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, ImColor(0, 0, 0));
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else
	{
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, ImColor(0, 0, 0));
	}
}


void renderWindowFrosted(shared_ptr<const core::Window> window, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	throw NotImplementedException("Frosted Window rendering");
}


void renderWindowTinted(shared_ptr<const core::Window> window, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	throw NotImplementedException("Tinted Window rendering");
}


void renderWindow(shared_ptr<const core::Window> window, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	// Don't render if we're rendering the back layer but it's not the visible one, because it
	// will be already rendered by the fore layer Location.
	if (layer != CORE_LAYER_BACK && !visibleLayer)
	{
		return;
	}

	auto style = window->getStyle();

	switch (style)
	{
	case core::Window::Style::Clear:
		renderWindowClear(window, layer, visibleLayer, selected, drawList);
		break;

	case core::Window::Style::Frosted:
		renderWindowFrosted(window, layer, visibleLayer, selected, drawList);
		break;

	case core::Window::Style::Tinted:
		renderWindowTinted(window, layer, visibleLayer, selected, drawList);
		break;
	}
}


void renderPhysicalControl(shared_ptr<const core::Button> button, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	if (layer != CORE_LAYER_BACK && !visibleLayer) return;

	core::Vector2 bounds0, bounds1;
	button->getFullShape(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = button->isEnabled() ? ImColor(0, 255, 128) : ImColor(192, 128, 128);
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderWalkway(shared_ptr<const core::Walkway> walkway, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	walkway->getFullShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = ImColor(64, 64, 64);
	drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y }, colour, 2.0f);
}


void renderMarker(shared_ptr<const core::Marker> marker, int layer, bool visibleLayer,
	bool selected, ImDrawList* drawList)
{
	if (!visibleLayer) return;
	constexpr float iconExtent = 18.0f;
	auto point = marker->getPosition();
	point.x += marker->getOffset();
	transformPosition(point);

	ImFont* font = gAgentIconFont ? gAgentIconFont : ImGui::GetFont();
	auto sourceSize = font->FontSize;
	auto sourceBounds = font->CalcTextSizeA(sourceSize, FLT_MAX, 0.0f, ICON_FA_MAP_MARKER_ALT);
	auto fontSize = sourceSize * iconExtent
		/ max(max(sourceBounds.x, sourceBounds.y), 1.0f);
	auto size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ICON_FA_MAP_MARKER_ALT);
	ImVec2 topLeft{ point.x - size.x * 0.5f, point.y - size.y };
	if (selected)
		drawList->AddRect({ topLeft.x - 3.0f, topLeft.y - 3.0f },
			{ topLeft.x + size.x + 3.0f, topLeft.y + size.y + 3.0f },
			SelectedColour, 2.0f, 0, 2.0f);
	drawList->AddText(font, fontSize, topLeft, ImColor(251, 188, 4), ICON_FA_MAP_MARKER_ALT);
}


void renderForceBridge(shared_ptr<const core::ForceBridge> forceBridge, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	forceBridge->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = ImColor(0, 255, 0);
	drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y }, colour, 2.0f);
}


void renderLadder(shared_ptr<const core::Ladder> ladder, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	ladder->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = LadderColour;
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderLift(shared_ptr<const core::Lift> lift, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	lift->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = LiftColour;
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderShuttle(shared_ptr<const core::Shuttle> shuttle, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	shuttle->getCurrentShape(bounds0, bounds1);

	auto numCars = shuttle->getNumCars();
	auto carWidth = shuttle->getCarWidth();

	for (uint32_t i = 0; i < numCars; ++i)
	{
		auto car0 = bounds0;
		car0.x += i * (carWidth + 1);

		auto car1 = car0;
		car1.x += carWidth;
		car1.y = bounds1.y;

		transformPosition(car0);
		transformPosition(car1);

		auto colour = ShuttleColour;
		drawList->AddRectFilled({ car0.x, car0.y }, { car1.x, car1.y }, colour);

		if (i < (numCars - 1))
		{
			auto cab0 = bounds0;
			cab0.x += i * (carWidth + 1) + carWidth;

			auto cab1 = cab0;
			cab1.x += 1;
			cab1.y = bounds1.y * 0.5f;

			transformPosition(cab0);
			transformPosition(cab1);

			drawList->AddRectFilled({ cab0.x, cab0.y }, { cab1.x, cab1.y }, colour);
		}
	}
}


void renderStaircase(shared_ptr<const core::Staircase> staircase, int layer, bool visibleLayer, bool selected, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	staircase->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	float x0 = staircase->getMountSide() == CORE_SIDE_LEFT ? 1.0f : 0.0f;
	float x1 = staircase->getMountSide() == CORE_SIDE_LEFT ? 0.0f : 1.0f;

	auto decksHigh = staircase->getDecksHigh();

	drawList->AddImage((ImTextureID)(intptr_t)gCellsTexture,
		{ bounds0.x, bounds0.y },
		{ bounds1.x, bounds1.y },
		{ x0, 0.0f },
		{ x1, (float)decksHigh }
	);
}


void renderSelected(shared_ptr<const core::Object> object, int layer, bool visibleLayer, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	object->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);
	
	bounds0.x -= 2;
	bounds0.y -= 2;
	bounds1.x += 2;
	bounds1.y += 2;

	auto colour = SelectedColour;
	drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderSectorObjects(shared_ptr<const core::Sector> sector, int layer, bool visibleLayer, bool wireframe, int flags, ImDrawList* drawList)
{
	// Sort so that Ladders and Lifts are rendered first, as these need to be behind everything else.
	auto sortedObjects = sector->getSortedObjects([](auto obj1, auto obj2)
	{
		if (obj1->getObjectType() == core::SectorObjectType::Ladder || obj1->getObjectType() == core::SectorObjectType::Lift)
		{
			if (obj2->getObjectType() == core::SectorObjectType::Ladder || obj2->getObjectType() == core::SectorObjectType::Lift)
			{
				if (obj1->getCellX() == obj1->getCellY())
				{
					return obj1->getCellY() < obj2->getCellY();
				}
				else
				{
					return obj1->getCellX() < obj2->getCellX();
				}
			}
			else
			{
				return true;
			}
		}
		else
		{
			return false;
		}
	});

	for (auto object : sortedObjects)
	{
		auto selected = object == gSelectedSectorObject;
		switch (object->getObjectType())
		{
		case core::SectorObjectType::BulkheadDoor:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderBulkheadDoor(static_pointer_cast<const core::BulkheadDoorSectorObject>(object)->getDoor(), layer, visibleLayer, selected, drawList);
			}
			break;

		case core::SectorObjectType::Door:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				renderDoor(static_pointer_cast<const core::DoorSectorObject>(object)->getDoor(), layer, visibleLayer, selected, drawList);
			}
			break;


		case core::SectorObjectType::ForceBridge:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderForceBridge(static_pointer_cast<const core::ForceBridgeSectorObject>(object)->getForceBridge(), layer, visibleLayer, selected, drawList);
			}
			break;

		case core::SectorObjectType::Ladder:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderLadder(static_pointer_cast<const core::LadderSectorObject>(object)->getLadder(), layer, visibleLayer, selected, drawList);
			}
			break;

		case core::SectorObjectType::Lift:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderLift(static_pointer_cast<const core::LiftSectorObject>(object)->getLift(), layer, visibleLayer, selected, drawList);
			}
			break;

		case core::SectorObjectType::InteractionPoint:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				auto button = static_pointer_cast<const core::Button>(object->_getObject());
				renderPhysicalControl(button, layer, visibleLayer, selected, drawList);
			}
			break;

		case core::SectorObjectType::Marker:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderMarker(static_pointer_cast<const core::MarkerSectorObject>(object)->getMarker(),
					layer, visibleLayer, selected, drawList);
			}
			break;

		case core::SectorObjectType::Walkway:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderWalkway(static_pointer_cast<const core::WalkwaySectorObject>(object)->getWalkway(), layer, visibleLayer, selected, drawList);
			}
			break;

		case core::SectorObjectType::Window:
			if (true)
			{
				auto window = static_pointer_cast<const core::WindowSectorObject>(object)->getWindow();
				bool isBackLayer = window->getSector(CORE_LAYER_BACK) == nullptr;

				if ((flags & RENDER_SECTOR_OBJECTS_BEHIND))
				{
					renderWindow(window, layer, visibleLayer, selected, drawList);
				}
			}
			break;

		default:
			break;
		}
	}
}


void renderAgent(core::Agent const* agent, ImDrawList* drawList)
{
	auto bounds = agent->getBounds();

	core::Vector2 pos0, pos1;

	bounds.getCurrentShape(pos0, pos1);

	transformPosition(pos0);
	transformPosition(pos1);

	auto colour = gSelectedAgent == agent ? ImColor(251, 188, 4) : ImColor(0.7f, 0.3f, 0.3f);
	ImFont* font = gAgentIconFont ? gAgentIconFont : ImGui::GetFont();
	float sourceSize = font->FontSize;
	auto sourceBounds = font->CalcTextSizeA(sourceSize, FLT_MAX, 0.0f, ICON_FA_MALE);
	float availableWidth = pos1.x - pos0.x;
	float availableHeight = pos0.y - pos1.y;
	float scale = min(availableWidth / max(sourceBounds.x, 1.0f),
		availableHeight / max(sourceBounds.y, 1.0f));
	float fontSize = sourceSize * scale;
	auto iconSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, ICON_FA_MALE);
	ImVec2 iconPosition{
		(pos0.x + pos1.x - iconSize.x) * 0.5f,
		pos0.y - iconSize.y
	};
	drawList->AddText(font, fontSize, iconPosition, colour, ICON_FA_MALE);

	if (gUISettings.renderAgentDebug)
	{
		ImU32 textColour = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.65f));

		switch (agent->getState())
		{
		case core::Agent::State::Idle:
			drawList->AddText({ pos0.x, pos0.y - 13 }, textColour, "IDL");
			break;

		case core::Agent::State::MovingToVertex:
			drawList->AddText({ pos0.x, pos0.y - 13 }, textColour, "MTV");
			break;

		case core::Agent::State::WaitingForTraversal:
			drawList->AddText({ pos0.x, pos0.y - 13 }, textColour, "WTP");
			break;

		case core::Agent::State::TraversingEdge:
			drawList->AddText({ pos0.x, pos0.y - 13 }, textColour, "TRE");
			break;

		case core::Agent::State::AwaitingTraversalCommit:
			drawList->AddText({ pos0.x, pos0.y - 13 }, textColour, "ATC");
			break;

		default:
			drawList->AddText({ pos0.x, pos0.y - 13 }, textColour, "???");
			break;
		}

		auto agentPath = agent->getPath();

		if (agentPath)
		{
			auto targetNodeIndex = agent->getPathTargetNodeIndex();

			auto pathDebug = format("{}/{}", targetNodeIndex, agentPath->nodes.size());
			drawList->AddText({ pos0.x, pos0.y - 26 }, textColour, pathDebug.c_str());
		}
	}
}


void renderSectorAgents(shared_ptr<const core::Sector> sector, int layer, bool visibleLayer, ImDrawList* drawList)
{
	auto const& agents = sector->getAgents();

	for (auto agent : agents)
	{
		renderAgent(agent, drawList);
	}
}


void renderSector(shared_ptr<const core::Sector> sector, int layer, bool visibleLayer, bool wireframe, bool renderEdges, ImColor colour, ImDrawList* drawList)
{
	if (!sector->areLightsOn())
	{
		colour = LightsOffColour;
	}

	// Render sector area
	core::Vector2 bounds0, bounds1;

	sector->getBounds(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	if (wireframe)
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}
	else
	{
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}

	if (gUISettings.selectionMode == UISettings::SelectionMode::Sector
		&& sector == gSelectedSector && sector->getType() == core::SectorType::Location)
	{
		ImVec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
		ImVec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
		drawList->AddRectFilled(topLeft, bottomRight, ImColor(255, 255, 0, 48));
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 2.0f);
	}

	// Render some objects before the sector-specific stuff, like windows
	// If we are on the Fore layer, then we will only be dealing with Locations, so nothing needs
	// to be rendered here
	if (layer == CORE_LAYER_BACK)
	{
		renderSectorObjects(sector, layer, visibleLayer, wireframe, RENDER_SECTOR_OBJECTS_BEHIND, drawList);
	}

	auto selected = sector == gSelectedSector;

	// Sector-specific
	switch (sector->getType())
	{
	case core::SectorType::Ladder:
		renderLadder(static_pointer_cast<const core::LadderTransit>(sector)->getLadder(), layer, visibleLayer, selected, drawList);
		break;

	case core::SectorType::Lift:
		renderLift(static_pointer_cast<const core::LiftTransit>(sector)->getLift(), layer, visibleLayer, selected, drawList);
		break;

	case core::SectorType::Shuttle:
		renderShuttle(static_pointer_cast<const core::ShuttleTransit>(sector)->getShuttle(), layer, visibleLayer, selected, drawList);
		break;

	case core::SectorType::Staircase:
		renderStaircase(static_pointer_cast<const core::StaircaseTransit>(sector)->getStaircase(), layer, visibleLayer, selected, drawList);
		break;
	}

	// Objects
	if (!wireframe)
	{
		int flags = RENDER_SECTOR_OBJECTS_INFRONT;
		
		if (layer == CORE_LAYER_FORE)
		{
			flags |= RENDER_SECTOR_OBJECTS_BEHIND;
		}

		renderSectorObjects(sector, layer, visibleLayer, wireframe, flags, drawList);
	}

	// Agents
	renderSectorAgents(sector, layer, visibleLayer, drawList);

	// Render ceiling
	if (renderEdges)
	{
		drawList->AddLine({ bounds0.x, bounds1.y }, { bounds1.x, bounds1.y }, ImColor(0, 0, 0), 2.0f);

		// Render floor
		drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y }, ImColor(0, 0, 0), 2.0f);

		// Render walls
		float height{ 0.0f };

		for (uint32_t y = 0; y < sector->getDecksHigh(); ++y)
		{
			auto deckHeight = sector->getDeckHeight(y);

			core::Vector2 wallBounds0, wallBounds1;

			sector->getBounds(wallBounds0, wallBounds1);

			wallBounds0.y += height;
			wallBounds1.y = wallBounds0.y + deckHeight;

			transformPosition(wallBounds0);
			transformPosition(wallBounds1);

			if (sector->getEndType(y, CORE_SIDE_LEFT) == core::SectorEndType::Wall)
			{
				drawList->AddLine({ wallBounds0.x, wallBounds0.y }, { wallBounds0.x, wallBounds1.y }, ImColor(0, 0, 0), 2.0f);
			}

			if (sector->getEndType(y, CORE_SIDE_RIGHT) == core::SectorEndType::Wall)
			{
				drawList->AddLine({ wallBounds1.x, wallBounds0.y }, { wallBounds1.x, wallBounds1.y }, ImColor(0, 0, 0), 2.0f);
			}

			height += deckHeight;
		}
	}
}


void renderLadderTransit(shared_ptr<const core::LadderTransit> ladderTransit, int layer, bool visibleLayer, bool wireframe, ImColor colour, bool selected, ImDrawList* drawList)
{
	// Transit Ladders go behind the Location, so we need to render in two goes.  First, clipping against
	// the upper Location, then against the lower.  And, despite being on the Back Layer, these need to be
	// rendered as part of the Fore layer.

	// Clip against upper Sector, then lower.
	if (layer == CORE_LAYER_FORE && visibleLayer)
	{
		core::Vector2 locBounds0, locBounds1;

		//
		// Upper sector
		//
		drawList->AddDrawCmd();

		auto upperSector = ladderTransit->getStop(CORE_LEVEL_HIGH).sector;

		upperSector->getBounds(locBounds0, locBounds1);

		transformPosition(locBounds0);
		transformPosition(locBounds1);

		// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ locBounds0.x, locBounds1.y }, { locBounds1.x, locBounds0.y }, true);

		renderSector(ladderTransit, layer, visibleLayer, wireframe, false, colour, drawList);
		renderLadder(ladderTransit->getLadder(), layer, visibleLayer, selected, drawList);

		drawList->PopClipRect();
		drawList->AddDrawCmd();

		//
		// Lower sector
		//
		auto lowerSector = ladderTransit->getStop(CORE_LEVEL_LOW).sector;

		lowerSector->getBounds(locBounds0, locBounds1);

		transformPosition(locBounds0);
		transformPosition(locBounds1);

		// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ locBounds0.x, locBounds1.y }, { locBounds1.x, locBounds0.y }, true);

		renderSector(ladderTransit, layer, visibleLayer, wireframe, false, colour, drawList);
		renderLadder(ladderTransit->getLadder(), layer, visibleLayer, selected, drawList);

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else
	{
		renderSector(ladderTransit, layer, visibleLayer, wireframe, true, colour, drawList);
		renderLadder(ladderTransit->getLadder(), layer, visibleLayer, selected, drawList);
	}
}


void renderLiftTransit(shared_ptr<const core::LiftTransit> liftTransit, int layer, bool visibleLayer, bool wireframe, bool selected, ImColor colour, ImDrawList* drawList)
{
	// Transit Lifts go behind the Location.  We need to render it clipped, for each Deck.
	if (layer == CORE_LAYER_FORE && visibleLayer)
	{
		for (uint32_t i = 0; i < liftTransit->getNumStops(); ++i)
		{
			auto const& stop = liftTransit->getStop(i);

			drawList->AddDrawCmd();

			uint32_t cellX = stop.sector->getCellX() + stop.sectorOffsetX;
			uint32_t cellY = stop.sector->getCellY() + stop.sectorOffsetY;

			auto x = (float)(cellX + liftTransit->getCellsWide() * 0.5f);
			auto y = (float)(cellY);
			auto width = liftTransit->getCellsWide() - CORE_LIFT_DOORWAY_BORDER * 2;

			core::Vector2 doorwayBounds0{ x - width * 0.5f, y };
			core::Vector2 doorwayBounds1{ x + width * 0.5f, y + CORE_LIFT_DOORWAY_HEIGHT };

			transformPosition(doorwayBounds0);
			transformPosition(doorwayBounds1);

			// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
			drawList->PushClipRect({ doorwayBounds0.x, doorwayBounds1.y }, { doorwayBounds1.x, doorwayBounds0.y }, true);

			renderSector(liftTransit, layer, visibleLayer, wireframe, false, colour, drawList);
			renderLift(liftTransit->getLift(), layer, visibleLayer, selected, drawList);

			drawList->PopClipRect();
			drawList->AddDrawCmd();
		}
	}
	else
	{
		renderSector(liftTransit, layer, visibleLayer, wireframe, true, colour, drawList);
		renderLift(liftTransit->getLift(), layer, visibleLayer, selected, drawList);
	}
}


void renderShuttleTransit(shared_ptr<const core::ShuttleTransit> shuttleTransit, int layer, bool visibleLayer, bool wireframe, bool selected, ImColor colour, ImDrawList* drawList)
{
	// Transit Shuttles go behind the Location.  We need to render it clipped, for each stop.
	if (layer == CORE_LAYER_FORE && visibleLayer)
	{
		for (uint32_t i = 0; i < shuttleTransit->getNumStops(); ++i)
		{
			auto const& stop = shuttleTransit->getStop(i);

			drawList->AddDrawCmd();

			uint32_t cellX = stop.sector->getCellX() + stop.sectorOffsetX;
			uint32_t cellY = stop.sector->getCellY() + stop.sectorOffsetY;

			auto x = (float)(cellX + 0.5f);
			auto y = (float)(cellY);
			auto width = 1.0f - CORE_SHUTTLE_DOORWAY_BORDER * 2.0f;

			core::Vector2 doorwayBounds0{ x - width * 0.5f, y };
			core::Vector2 doorwayBounds1{ x + width * 0.5f, y + CORE_SHUTTLE_DOORWAY_HEIGHT };

			transformPosition(doorwayBounds0);
			transformPosition(doorwayBounds1);

			// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
			drawList->PushClipRect({ doorwayBounds0.x, doorwayBounds1.y }, { doorwayBounds1.x, doorwayBounds0.y }, true);

			renderSector(shuttleTransit, layer, visibleLayer, wireframe, false, colour, drawList);
			renderShuttle(shuttleTransit->getShuttle(), layer, visibleLayer, selected, drawList);

			drawList->PopClipRect();
			drawList->AddDrawCmd();
		}
	}
	else
	{
		renderSector(shuttleTransit, layer, visibleLayer, wireframe, true, colour, drawList);
		renderShuttle(shuttleTransit->getShuttle(), layer, visibleLayer, selected, drawList);
	}
}


void renderStaircaseTransit(shared_ptr<const core::StaircaseTransit> staircaseTransit, int layer, bool visibleLayer, bool wireframe, ImColor colour, bool selected, ImDrawList* drawList)
{
	// Transit Staircases go behind the Location.  We need to render it clipped, for each Deck.
	if (layer == CORE_LAYER_FORE && visibleLayer)
	{
		for (uint32_t i = 0; i < staircaseTransit->getDecksHigh(); ++i)
		{
			drawList->AddDrawCmd();

			auto x = (float)(staircaseTransit->getCellX() + 1.0f);
			auto y = (float)(staircaseTransit->getCellY() + i);

			core::Vector2 doorwayBounds0{ x - CORE_STAIRCASE_DOORWAY_WIDTH * 0.5f, y };
			core::Vector2 doorwayBounds1{ x + CORE_STAIRCASE_DOORWAY_WIDTH * 0.5f, y + CORE_STAIRCASE_DOORWAY_HEIGHT };

			transformPosition(doorwayBounds0);
			transformPosition(doorwayBounds1);

			// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
			drawList->PushClipRect({ doorwayBounds0.x, doorwayBounds1.y }, { doorwayBounds1.x, doorwayBounds0.y }, true);

			renderSector(staircaseTransit, layer, visibleLayer, wireframe, false, colour, drawList);

			drawList->PopClipRect();
			drawList->AddDrawCmd();
		}
	}
	else
	{
		renderSector(staircaseTransit, layer, visibleLayer, wireframe, true, colour, drawList);
		renderStaircase(staircaseTransit->getStaircase(), layer, visibleLayer, selected, drawList);
	}
}


void renderSectors(shared_ptr<const core::Building> building, int layer, bool visibleLayer, bool wireframe, ImDrawList* drawList)
{
	auto sectors = building->getSectorsInBounds(layer, -gUISettings.xOffset, 0,
		gUISettings.worldViewportWidth, gUISettings.worldViewportHeight);

	ImColor colour;

	colour = layer == 0 ? ForeLocationColour : BackLocationColour;
	
	for (auto sector : sectors)
	{
		renderSector(sector, layer, visibleLayer, wireframe, true, colour, drawList);
	}

	// If Layer is 0 (Fore) then we want to render Transits, clipped against Fore Locations, unless they
	// have a Door in front of them, in which case they will already be rendered.
	if (layer == CORE_LAYER_FORE && visibleLayer)
	{
		sectors = building->getSectorsInBounds(CORE_LAYER_BACK, -gUISettings.xOffset, 0,
			gUISettings.worldViewportWidth, gUISettings.worldViewportHeight);

		for (auto sector : sectors)
		{
			auto selected = sector == gSelectedSector;

			switch (sector->getType())
			{
			case core::SectorType::Ladder:
				renderLadderTransit(static_pointer_cast<const core::LadderTransit>(sector), layer, visibleLayer, wireframe, ForeLocationColour, selected, drawList);
				break;

			case core::SectorType::Staircase:
				renderStaircaseTransit(static_pointer_cast<const core::StaircaseTransit>(sector), layer, visibleLayer, wireframe, BackLocationColour, selected, drawList);
				break;
			}
		}
	}
}


void renderBuilding(shared_ptr<const core::Building> building)
{
	auto drawList = ImGui::GetWindowDrawList();

	assert(gUISettings.visibleLayer == 0 || gUISettings.visibleLayer == 1);

	// Locations
	renderSectors(building, (uint32_t)gUISettings.visibleLayer, true, false, drawList);

	// Other Layer
	if (gUISettings.renderNonVisibleLayer)
	{
		renderSectors(building, 1 - (uint32_t)gUISettings.visibleLayer, false, true, drawList);
	}

	// Grid
	if (gUISettings.renderGrid)
	{
		renderGrid(ImColor(128, 128, 127), 1.0f, drawList);
	}
}