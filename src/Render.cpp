#include <array>
#include <cassert>
#include <cfloat>
#include <set>
#include <algorithm>

#if defined(_WIN32)
#include <Windows.h>
#include <gl/GL.h>
#elif defined(__linux__)
#include <GL/glew.h>
#else
#error "Unsupported platform"
#endif

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/IconsFontAwesome5.h"

#include "core/Defines.h"
#include "core/Background.h"
#include "core/Facade.h"
#include "core/Building.h"
#include "core/Location.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/ButtonSectorObject.h"
#include "core/DoorSectorObject.h"
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

//
// The Building currently being rendered. A clear Window composites the
// Backgrounds behind it from the back Layer's cell grid (#37, and #36 made
// the Window's single back Sector non-authoritative for such a span), and
// the renderSector -> renderSectorObjects -> renderWindow chain which
// reaches renderWindowClear carries no Building pointer. renderBuilding()
// sets this on entry; every render entry point goes through it.
//
static std::shared_ptr<const core::Building> gRenderBuilding;

extern ImFont* gAgentIconFont;

using namespace std;

ImColor ForeLocationColour = ImColor(192, 192, 255);
ImColor BackLocationColour = ImColor(224, 224, 255);
// The value lives in Render.h as LightsOffTint so the headless checks can read
// the same colour the viewport paints with.
ImColor LightsOffColour = ImColor(LightsOffTint.r, LightsOffTint.g, LightsOffTint.b);
ImColor LadderColour = ImColor(128, 128, 192);
ImColor LiftColour = ImColor(128, 128, 192);
ImColor ShuttleColour = ImColor(128, 128, 192);
ImColor StairwellColour = ImColor(128, 128, 192);
ImColor VertexColour = ImColor(255, 128, 0);
ImColor EdgeColour = ImColor(255, 128, 0);
ImColor InterLayerEdgeColour = ImColor(255, 255, 64);
ImColor SelectedColour = ImColor(255, 255, 0);

#define RENDER_SECTOR_OBJECTS_BEHIND 1
#define RENDER_SECTOR_OBJECTS_INFRONT 2

void renderSector(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, bool renderEdges, ImColor colour, ImDrawList* drawList);
void renderSectorAgents(shared_ptr<const core::Sector> sector, ImDrawList* drawList);

void renderTransitThroughApertures(shared_ptr<const core::Sector> const& transit, uint32_t behindLayer,
	std::vector<TransitAperture> const& apertures, ImDrawList* drawList);

void renderStaircase(shared_ptr<const core::Staircase> staircase, ImDrawList* drawList);

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


void renderSelectedQueues(shared_ptr<const core::Building> const& building, int layer,
	ImDrawList* drawList)
{
	shared_ptr<const core::Object> selectedObject;
	if (gSelectedSectorObject)
	{
		selectedObject = gSelectedSectorObject->_getObject();
	}
	else if (gSelectedSector)
	{
		switch (gSelectedSector->getType())
		{
		case core::SectorType::Ladder:
			selectedObject = static_pointer_cast<const core::LadderTransit>(gSelectedSector)->getLadder();
			break;
		case core::SectorType::Lift:
			selectedObject = static_pointer_cast<const core::LiftTransit>(gSelectedSector)->getLift();
			break;
		case core::SectorType::Shuttle:
			selectedObject = static_pointer_cast<const core::ShuttleTransit>(gSelectedSector)->getShuttle();
			break;
		case core::SectorType::Stairwell:
			selectedObject = static_pointer_cast<const core::StairwellTransit>(gSelectedSector)->getStairwell();
			break;
		case core::SectorType::Staircase:
			selectedObject = static_pointer_cast<const core::StaircaseTransit>(gSelectedSector)->getStaircase();
			break;
		default:
			break;
		}
	}

	// Lift-owned landing Doors have their own threshold resource containing the
	// physical queue geometry. Building::getTraversalResourceId() deliberately
	// resolves them to the Lift coordinator for scheduling, so select the landing
	// resource directly when rendering that Door's queue spots.
	core::TraversalResourceId resourceId;
	auto schedulingResourceId = building->getTraversalResourceId(selectedObject.get());
	if (gSelectedSectorObject
		&& gSelectedSectorObject->getObjectType() == core::SectorObjectType::Door)
	{
		resourceId = static_pointer_cast<const core::DoorSectorObject>(
			gSelectedSectorObject)->getDoor()->getTraversalResourceId();
	}
	else resourceId = schedulingResourceId;
	if (!resourceId) return;
	auto snapshot = building->getSimulationSnapshot();
	auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
		[resourceId](auto const& candidate) { return candidate.id == resourceId; });
	if (resource == snapshot.traversalResources.end()) return;
	auto coordinator = schedulingResourceId != resourceId
		? find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[schedulingResourceId](auto const& candidate) { return candidate.id == schedulingResourceId; })
		: snapshot.traversalResources.end();

	const ImColor laneColour(0, 210, 255, 210);
	const ImColor occupiedColour(255, 170, 0, 230);
	const float slotRadius = max(5.0f, CORE_AGENT_MAX_WIDTH * CORE_CELL_WIDTH_PIXELS * 0.35f);

	// Door queue geometry belongs to one approach layer, so only show the visible
	// side. A selected Ladder is different: its two approach lanes are its lower
	// and upper ends, and both must remain visible even when the Ladder itself is
	// represented by a Back-layer transit.
	vector<core::QueuePositionSnapshot> visiblePositions;
	set<uint32_t> occupiedCapacityPositions;
	bool capacityPositionsVisible = false;
	for (auto const& lane : resource->queueLanes)
	{
		if (!lane.sector) continue;
		auto sector = building->getSector((uint32_t)lane.sector.value - 1);
		if (!sector || (!resource->isLadder
			&& sector->getLayerIndex() != (uint32_t)layer)) continue;
		if (sector->getType() == core::SectorType::Lift
			&& coordinator != snapshot.traversalResources.end())
		{
			// A selected landing Door owns queue geometry at that landing. Use the
			// lane's authored Y rather than the car's current position, which may be
			// aligned with a different stop.
			capacityPositionsVisible = true;
			for (auto const& capacity : coordinator->capacityPositions)
				if (capacity.occupant || capacity.admissionReservation)
					occupiedCapacityPositions.insert(capacity.index);
		}
		auto start = lane.origin;
		auto end = lane.origin + lane.direction * lane.extent;
		transformPosition(start);
		transformPosition(end);
		drawList->AddLine({ start.x, start.y }, { end.x, end.y }, laneColour, 3.0f);
		visiblePositions.insert(visiblePositions.end(), lane.positions.begin(), lane.positions.end());
	}
	core::Vector2 objectBounds0, objectBounds1;
	selectedObject->getCurrentShape(objectBounds0, objectBounds1);
	auto distanceToObject = [&](core::Vector2 const& point)
	{
		auto dx = max(max(objectBounds0.x - point.x, 0.0f), point.x - objectBounds1.x);
		auto dy = max(max(objectBounds0.y - point.y, 0.0f), point.y - objectBounds1.y);
		return sqrt(dx * dx + dy * dy);
	};
	sort(visiblePositions.begin(), visiblePositions.end(), [&](auto const& left, auto const& right)
	{
		auto leftDistance = distanceToObject(left.position);
		auto rightDistance = distanceToObject(right.position);
		if (abs(leftDistance - rightDistance) > 0.001f) return leftDistance < rightDistance;
		return left.position.x < right.position.x;
	});
	for (uint32_t index = 0; index < visiblePositions.size(); ++index)
	{
		auto point = visiblePositions[index].position;
		transformPosition(point);
		bool const occupied = visiblePositions[index].owner
			|| (capacityPositionsVisible
				&& occupiedCapacityPositions.contains(visiblePositions[index].index));
		auto colour = occupied ? occupiedColour : laneColour;
		if (occupied)
			drawList->AddCircleFilled({ point.x, point.y }, slotRadius, ImColor(255, 170, 0, 64));
		drawList->AddCircle({ point.x, point.y }, slotRadius, colour, 0, 2.0f);
		auto label = to_string(index + 1);
		drawList->AddText({ point.x + slotRadius + 2.0f, point.y - 7.0f }, colour, label.c_str());
	}

	// Capacity and transport queues do not necessarily have external physical
	// slots. Mark their waiting agents in place, preserving the resource's order.
	vector<core::TraversalRequestId> queuedRequests;
	auto appendUnique = [&](core::TraversalRequestId request)
	{
		if (request && find(queuedRequests.begin(), queuedRequests.end(), request) == queuedRequests.end())
			queuedRequests.push_back(request);
	};
	for (auto const& lane : resource->queueLanes)
		for (auto request : lane.queue) appendUnique(request);
	for (auto request : resource->admissionQueue) appendUnique(request);
	for (auto const& zone : resource->shuttleAccessZones)
		for (auto request : zone.queue) appendUnique(request);
	for (auto request : resource->liftConfirmationQueue) appendUnique(request);

	for (uint32_t rank = 0; rank < queuedRequests.size(); ++rank)
	{
		auto request = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
			[&](auto const& candidate) { return candidate.id == queuedRequests[rank]; });
		if (request == snapshot.traversalRequests.end()) continue;
		auto agent = find_if(snapshot.agents.begin(), snapshot.agents.end(),
			[&](auto const& candidate) { return candidate.id == request->owner; });
		if (agent == snapshot.agents.end() || !agent->sectorId) continue;
		auto sector = building->getSector((uint32_t)agent->sectorId.value - 1);
		if (!sector || sector->getLayerIndex() != (uint32_t)layer) continue;
		auto point = agent->globalPosition
			+ core::Vector2{ CORE_AGENT_MAX_WIDTH * 0.5f, CORE_AGENT_MAX_HEIGHT * 0.5f };
		transformPosition(point);
		drawList->AddCircle({ point.x, point.y }, slotRadius + 3.0f, occupiedColour, 0, 3.0f);
		auto label = string("Q") + to_string(rank + 1);
		drawList->AddText({ point.x + slotRadius + 5.0f, point.y - 7.0f }, occupiedColour, label.c_str());
	}
}

void renderGrid(shared_ptr<const core::Building> const& building, ImColor const& colour,
	float width, ImDrawList* drawList)
{
	core::Vector2 topLeft{ 0.0f, (float)building->getDecksHigh() };
	core::Vector2 bottomRight{ (float)building->getCellsWide(), 0.0f };
	transformPosition(topLeft);
	transformPosition(bottomRight);

	// Include both outer edges, not just the cell separators. Building the grid
	// from world coordinates also keeps its far-right and bottom lines present
	// when the canvas is larger than the world.
	for (uint32_t x = 0; x <= building->getCellsWide(); ++x)
	{
		float screenX = topLeft.x + x * CORE_CELL_WIDTH_PIXELS;
		drawList->AddLine({ screenX, topLeft.y }, { screenX, bottomRight.y }, colour, width);
	}

	for (uint32_t y = 0; y <= building->getDecksHigh(); ++y)
	{
		float screenY = bottomRight.y - y * CORE_DECK_HEIGHT_PIXELS;
		drawList->AddLine({ topLeft.x, screenY }, { bottomRight.x, screenY }, colour, width);
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

		if (path)
		{
			auto vertexNodeIt = find_if(path->nodes.begin(), path->nodes.end(), [vertex](auto node)
			{
				return node.targetVertex->sameAs(vertex);
			});

			if (vertexNodeIt != path->nodes.end())
			{
				vertexIsInPath = true;
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


void renderDoorVertFromFloor(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, bool /* selected */, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1, bounds2;

	door->getFullShape(bounds0, bounds2);

	bounds1 = bounds0;
	bounds1.y += door->getOpenPercentage() * CORE_DOOR_HEIGHT;

	transformPosition(bounds0);
	transformPosition(bounds1);
	transformPosition(bounds2);

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = ImColor(64, 192, 255);
		drawList->AddRectFilled({ bounds1.x, bounds1.y }, { bounds2.x, bounds2.y }, doorColour);

		drawList->AddDrawCmd();

		// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ bounds1.x, bounds1.y }, { bounds2.x, bounds0.y }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds1.x, bounds1.y }, { bounds2.x, bounds2.y }, ImColor(0, 0, 0));
	}
}


void renderDoorHorzFromCentre(shared_ptr<const core::Door> /* door */, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, ImDrawList* /* drawList */)
{
	throw NotImplementedException("Lift-style Door rendering");
}


void renderDoorQuadIris(shared_ptr<const core::Door> /* door */, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, ImDrawList* /* drawList */)
{
	throw NotImplementedException("Iris Door rendering");
}


void renderDoor(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, bool selected, ImDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto openStyle = door->getOpenStyle();

	switch (openStyle)
	{
	case core::Door::OpenStyle::VertFromFloor:
		renderDoorVertFromFloor(door, layer, style, selected, drawList);
		break;

	case core::Door::OpenStyle::HorzFromCentre:
		renderDoorHorzFromCentre(door, layer, style, selected, drawList);
		break;

	case core::Door::OpenStyle::QuadIris:
		renderDoorQuadIris(door, layer, style, selected, drawList);
		break;
	}

	if (selected)
	{
		core::Vector2 bounds0, bounds1;
		door->getFullShape(bounds0, bounds1);
		transformPosition(bounds0);
		transformPosition(bounds1);
		ImVec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
		ImVec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
		drawList->AddRectFilled(topLeft, bottomRight, ImColor(255, 255, 0, 48));
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 2.0f);
	}
}


void renderBulkheadDoor(shared_ptr<const core::BulkheadDoor> door, uint32_t /* layer */, LayerRenderStyle style, bool selected, ImDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	core::Vector2 bounds0, bounds1;

	door->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto topLeft = ImVec2{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
	auto bottomRight = ImVec2{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
	auto doorColour = ImColor(128, 192, 182);
	if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect(topLeft, bottomRight, ImColor(0, 0, 0));
	}
	else
	{
		drawList->AddRectFilled(topLeft, bottomRight, doorColour);
	}
	if (selected)
	{
		core::Vector2 full0, full1;
		door->getFullShape(full0, full1);
		transformPosition(full0); transformPosition(full1);
		topLeft = { min(full0.x, full1.x), min(full0.y, full1.y) };
		bottomRight = { max(full0.x, full1.x), max(full0.y, full1.y) };
		drawList->AddRectFilled(topLeft, bottomRight, ImColor(255, 255, 0, 48));
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 2.0f);
	}
}


void renderWindowClear(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, bool /* selected */, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	window->getFullShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, ImColor(0, 0, 0));
		return;
	}

	// A Window is authored on the front Layer of its pair, so index 1 is the
	// Sector on the Layer directly behind.
	auto backSector = window->getBackSector();
	auto const backLayer = core::layerBehind(layer);

	// The aperture's rect in world (cell) units, before the screen transform.
	core::Vector2 worldMin, worldMax;
	window->getFullShape(worldMin, worldMax);

	drawList->AddDrawCmd();

	// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering.
	drawList->PushClipRect({ bounds0.x, bounds1.y }, { bounds1.x, bounds0.y }, true);

	// What the glass shows is derived from the back Layer's cell grid, not from
	// the Window's single back Sector (#36 made that reference
	// non-authoritative). Each Background the aperture looks into is drawn
	// clipped to the intersection of the Window rect and that Background's own
	// rect, so the seam between two Backgrounds lands exactly on the cell
	// boundary between them - no bleed past it, no seam line across it - and
	// stays pinned to the world as the viewport scrolls.
	auto const regions = gRenderBuilding
		? backgroundApertureRegions(*gRenderBuilding, backLayer, worldMin, worldMax)
		: std::vector<BackgroundApertureRegion>{};

	if (!regions.empty())
	{
		// Cells the span looks through which hold no Background contribute no
		// region; black stands in behind the glass there, as it does for a
		// Window with no back Sector at all. The opaque fills below cover it
		// wherever a Background is present.
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, ImColor(0, 0, 0));

		for (auto const& region : regions)
		{
			auto clip0 = region.min;
			auto clip1 = region.max;
			transformPosition(clip0);
			transformPosition(clip1);

			drawList->AddDrawCmd();
			drawList->PushClipRect(
				{ min(clip0.x, clip1.x), min(clip0.y, clip1.y) },
				{ max(clip0.x, clip1.x), max(clip0.y, clip1.y) }, true);

			// The Background fills with its own colour (apertureFillColour,
			// #34); the region clip trims that fill to this stretch of the
			// aperture.
			renderSector(region.background, backLayer, LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);

			drawList->PopClipRect();
			drawList->AddDrawCmd();
		}
	}
	else if (backSector)
	{
		// The glass shows the Background's own colour, not the generic
		// back-layer tint (#34). A back Sector that carries no colour of its
		// own keeps the tint.
		auto const ownColour = apertureFillColour(*backSector);
		auto const apertureColour = ownColour
			? ImColor(ownColour->r, ownColour->g, ownColour->b, 255)
			: BackLocationColour;

		renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
			apertureColour, drawList);
	}
	else
	{
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, ImColor(0, 0, 0));
	}

	drawList->PopClipRect();
	drawList->AddDrawCmd();
}


void renderWindowFrosted(shared_ptr<const core::Window> /* window */, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, ImDrawList* /* drawList */)
{
	throw NotImplementedException("Frosted Window rendering");
}


void renderWindowTinted(shared_ptr<const core::Window> /* window */, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, ImDrawList* /* drawList */)
{
	throw NotImplementedException("Tinted Window rendering");
}


void renderWindow(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, bool selected, ImDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto windowStyle = window->getStyle();

	switch (windowStyle)
	{
	case core::Window::Style::Clear:
		renderWindowClear(window, layer, style, selected, drawList);
		break;

	case core::Window::Style::Frosted:
		renderWindowFrosted(window, layer, style, selected, drawList);
		break;

	case core::Window::Style::Tinted:
		renderWindowTinted(window, layer, style, selected, drawList);
		break;
	}

	if (selected)
	{
		core::Vector2 bounds0, bounds1;
		window->getFullShape(bounds0, bounds1);
		transformPosition(bounds0);
		transformPosition(bounds1);
		ImVec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
		ImVec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
		drawList->AddRectFilled(topLeft, bottomRight, ImColor(255, 255, 0, 48));
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 2.0f);
	}
}


void renderPhysicalControl(shared_ptr<const core::Button> button, uint32_t /* layer */, LayerRenderStyle style, bool /* selected */, ImDrawList* drawList)
{
	if (style != LayerRenderStyle::Solid) return;

	core::Vector2 bounds0, bounds1;
	button->getFullShape(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = button->isEnabled() ? ImColor(0, 255, 128) : ImColor(192, 128, 128);
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderWalkway(shared_ptr<const core::Walkway> walkway, uint32_t /* layer */, LayerRenderStyle style,
	bool selected, ImDrawList* drawList)
{
	if (style != LayerRenderStyle::Solid) return;
	core::Vector2 bounds0, bounds1;

	walkway->getFullShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = selected ? SelectedColour : ImColor(64, 64, 64);
	drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y }, colour,
		selected ? 3.0f : 2.0f);
}


void renderMarker(shared_ptr<const core::Marker> marker, uint32_t /* layer */, LayerRenderStyle style,
	bool selected, ImDrawList* drawList)
{
	if (style != LayerRenderStyle::Solid) return;
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


void renderForceBridge(shared_ptr<const core::ForceBridge> forceBridge, uint32_t /* layer */,
	LayerRenderStyle style, bool selected, ImDrawList* drawList)
{
	if (style != LayerRenderStyle::Solid) return;
	core::Vector2 bounds0, bounds1;
	if (selected)
	{
		forceBridge->getFullShape(bounds0, bounds1);
		transformPosition(bounds0);
		transformPosition(bounds1);
		drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y },
			SelectedColour, 5.0f);
	}
	forceBridge->getCurrentShape(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);
	drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y },
		ImColor(0, 255, 0), selected ? 3.0f : 2.0f);
}


void renderLadder(shared_ptr<const core::Ladder> ladder, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	ladder->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = LadderColour;
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderLift(shared_ptr<const core::Lift> lift, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, ImDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	lift->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = LiftColour;
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderShuttle(shared_ptr<const core::Shuttle> shuttle, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, ImDrawList* drawList)
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


void renderStairwell(shared_ptr<const core::Stairwell> stairwell, uint32_t /* layer */, LayerRenderStyle /* style */, bool /* selected */, ImDrawList* drawList)
{
	core::Vector2 worldMin, worldMax;
	stairwell->getCurrentShape(worldMin, worldMax);

	auto screenMin = worldMin;
	auto screenMax = worldMax;
	transformPosition(screenMin);
	transformPosition(screenMax);
	ImVec2 topLeft{ min(screenMin.x, screenMax.x), min(screenMin.y, screenMax.y) };
	ImVec2 bottomRight{ max(screenMin.x, screenMax.x), max(screenMin.y, screenMax.y) };
	drawList->AddRectFilled(topLeft, bottomRight, ImColor(255, 255, 255));

	auto toScreen = [&](core::Vector2 point)
	{
		point += worldMin;
		transformPosition(point);
		return ImVec2{ point.x, point.y };
	};

	// Stairwell::getDeckPath is also used to place the Graph vertices. Build one
	// continuous polyline so adjacent flights share their exact deck endpoint.
	vector<ImVec2> pathPoints;
	if (stairwell->getDecksHigh() > 1)
		pathPoints.reserve(1 + (stairwell->getDecksHigh() - 1) * 3);
	for (uint32_t deck = 0; deck + 1 < stairwell->getDecksHigh(); ++deck)
	{
		auto path = stairwell->getDeckPath(deck);
		if (pathPoints.empty()) pathPoints.push_back(toScreen(path[0]));
		pathPoints.push_back(toScreen(path[1]));
		pathPoints.push_back(toScreen(path[2]));
		pathPoints.push_back(toScreen(path[3]));
	}
	if (pathPoints.size() >= 2)
	{
		drawList->AddPolyline(pathPoints.data(), (int)pathPoints.size(),
			ImColor(223, 223, 223), ImDrawFlags_None, 10.0f);
		drawList->AddPolyline(pathPoints.data(), (int)pathPoints.size(),
			ImColor(0, 0, 0), ImDrawFlags_None, 4.0f);
	}
}


void renderStaircase(shared_ptr<const core::Staircase> staircase, ImDrawList* drawList)
{
	auto const path = staircase->getPath();
	core::Vector2 origin, ignored;
	staircase->getCurrentShape(origin, ignored);
	uint32_t const count = staircase->getStepCount();
	auto toScreen = [&](float x, float y)
	{
		core::Vector2 point{ origin.x + x, origin.y + y };
		transformPosition(point);
		return ImVec2{ point.x, point.y };
	};
	auto drawSteps = [&](auto const& segments, ImU32 colour, float thickness)
	{
		for (auto const& segment : segments)
			drawList->AddPolyline(segment.data(), (int)segment.size(), colour,
				ImDrawFlags_None, thickness);
	};

	if (staircase->isEscalator())
	{
		// Each L-shaped tread/riser advances along the incline at the Escalator's
		// world speed, then wraps to the opposite endpoint.
		vector<array<ImVec2, 3>> segments;
		segments.reserve(count);
		float const step = 1.0f / (float)count;
		float const phase = staircase->getAnimationPhase();
		for (uint32_t i = 0; i < count; ++i)
		{
			float t = fmod((float)i * step + phase, 1.0f);
			if (t < 0.0f) t += 1.0f;
			float const previous = max(0.0f, t - step);
			float const x0 = path[0].x + (path[1].x - path[0].x) * previous;
			float const x1 = path[0].x + (path[1].x - path[0].x) * t;
			float const y0 = path[0].y + (path[1].y - path[0].y) * previous;
			float const y1 = path[0].y + (path[1].y - path[0].y) * t;
			segments.push_back({ toScreen(x0, y0), toScreen(x1, y0), toScreen(x1, y1) });
		}
		drawSteps(segments, IM_COL32(32, 32, 32, 255), 10.0f);
		drawSteps(segments, IM_COL32(220, 220, 220, 255), 6.0f);
		return;
	}

	vector<ImVec2> points;
	points.reserve(count * 2 + 1);
	points.push_back(toScreen(path[0].x, path[0].y));
	for (uint32_t i = 0; i < count; ++i)
	{
		float const t0 = (float)i / (float)count;
		float const t1 = (float)(i + 1) / (float)count;
		float const x1 = path[0].x + (path[1].x - path[0].x) * t1;
		float const y0 = path[0].y + (path[1].y - path[0].y) * t0;
		float const y1 = path[0].y + (path[1].y - path[0].y) * t1;
		points.push_back(toScreen(x1, y0));
		points.push_back(toScreen(x1, y1));
	}
	drawList->AddPolyline(points.data(), (int)points.size(), IM_COL32(32, 32, 32, 255),
		ImDrawFlags_None, 10.0f);
	drawList->AddPolyline(points.data(), (int)points.size(), IM_COL32(220, 220, 220, 255),
		ImDrawFlags_None, 6.0f);
}

void renderSelected(shared_ptr<const core::Object> object, int /* layer */, bool /* visibleLayer */, ImDrawList* drawList)
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


void renderSectorObjects(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, int flags, ImDrawList* drawList)
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

	// A Door or Window is authored on the front Layer of its pair, but the same
	// object is registered on the Sector on the Layer behind as well. Its aperture
	// is only drawn while rendering from the Layer it is authored on; from the
	// Layer behind it is drawn as an outline.
	auto thresholdStyle = [&](shared_ptr<const core::Sector> authoredSector)
	{
		return style == LayerRenderStyle::Solid && sector == authoredSector
			? LayerRenderStyle::Solid
			: LayerRenderStyle::Wireframe;
	};

	for (auto object : sortedObjects)
	{
		auto selected = object == gSelectedSectorObject;
		switch (object->getObjectType())
		{
		case core::SectorObjectType::BulkheadDoor:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderBulkheadDoor(static_pointer_cast<const core::BulkheadDoorSectorObject>(object)->getDoor(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Door:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				renderDoor(door, layer, thresholdStyle(door->getFrontSector()), selected, drawList);
			}
			break;


		case core::SectorObjectType::ForceBridge:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderForceBridge(static_pointer_cast<const core::ForceBridgeSectorObject>(object)->getForceBridge(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Ladder:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderLadder(static_pointer_cast<const core::LadderSectorObject>(object)->getLadder(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Lift:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderLift(static_pointer_cast<const core::LiftSectorObject>(object)->getLift(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::InteractionPoint:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				auto button = static_pointer_cast<const core::Button>(object->_getObject());
				renderPhysicalControl(button, layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Marker:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderMarker(static_pointer_cast<const core::MarkerSectorObject>(object)->getMarker(),
					layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Walkway:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderWalkway(static_pointer_cast<const core::WalkwaySectorObject>(object)->getWalkway(), layer, style, selected, drawList);
			}
			break;

		case core::SectorObjectType::Window:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto window = static_pointer_cast<const core::WindowSectorObject>(object)->getWindow();
				renderWindow(window, layer, thresholdStyle(window->getFrontSector()), selected, drawList);
			}
			break;

		default:
			break;
		}
	}
}


//
// Clipped transits from the Layer behind draw over the selected Layer's Locations.
// Redraw what the selected Layer owns and must keep in front of them: its own
// thresholds first, so a closed Door still occludes the Transit standing behind
// it; then its physical controls; then its Agents, so no control can be painted
// in front of an Agent.
//
void renderThresholdsControlsAndAgentsAboveTransit(vector<shared_ptr<const core::Sector>> const& sectors,
	uint32_t layer, ImDrawList* drawList)
{
	for (auto const& sector : sectors)
	{
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::Door)
				continue;

			// Only a Door authored on this Layer opens into the Layer behind, and so
			// covers something that Layer draws. The same Door object registered on the
			// Sector behind is seen from the other side and is not occluder here.
			auto door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			if (door->getFrontSector() != sector)
				continue;

			renderDoor(door, layer, LayerRenderStyle::Solid,
				object == gSelectedSectorObject, drawList);
		}
	}

	for (auto const& sector : sectors)
	{
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::InteractionPoint)
				continue;
			auto button = static_pointer_cast<const core::Button>(object->_getObject());
			renderPhysicalControl(button, layer, LayerRenderStyle::Solid,
				object == gSelectedSectorObject, drawList);
		}
	}

	// Controls are redrawn above clipped transits. Restore the sector's Agents
	// afterwards so no physical control can be painted in front of them.
	for (auto const& sector : sectors)
	{
		renderSectorAgents(sector, drawList);
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


void renderSectorAgents(shared_ptr<const core::Sector> sector, ImDrawList* drawList)
{
	auto const& agents = sector->getAgents();

	for (auto agent : agents)
	{
		renderAgent(agent, drawList);
	}
}


void renderSector(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, bool renderEdges, ImColor colour, ImDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	// A Background is drawn with its own raw colour. The lights-off tint is
	// bypassed: a Background has no lights to switch, and tinting it would
	// silently override the colour the user picked and make the picker
	// untrustworthy. A Facade follows the same rendering rule (ADR 0003):
	// its user-picked colour is authoritative even though, unlike a
	// Background, its lights-off state still means what it means for the
	// agents and objects inside it.
	auto const sectorType = sector->getType();
	auto const ownColour = rendersAsFlatColour(sectorType);

	if (!bypassesLightsOffTint(sectorType) && !sector->areLightsOn())
	{
		colour = LightsOffColour;
	}

	// Render sector area
	core::Vector2 bounds0, bounds1;

	sector->getBounds(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	// The generic fill below uses the Layer's colour. A Background fills with its
	// own colour in the type switch, so it is skipped here rather than being
	// painted twice. A Facade does the same: its colour is its whole surface.
	if (!ownColour)
	{
		if (style == LayerRenderStyle::Wireframe)
		{
			drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
		}
		else
		{
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
		}
	}

	// Thresholds are drawn before the sector-specific stuff so their apertures are
	// not painted over. They are never drawn for an Aperture pass, which is itself
	// the view through one, and would otherwise recurse into the same threshold.
	if (style != LayerRenderStyle::Aperture)
	{
		renderSectorObjects(sector, layer, style, RENDER_SECTOR_OBJECTS_BEHIND, drawList);
	}

	auto selected = sector == gSelectedSector;

	// Sector-specific
	switch (sector->getType())
	{
	case core::SectorType::Ladder:
		if (shouldRenderLadderGeometry(style))
			renderLadder(static_pointer_cast<const core::LadderTransit>(sector)->getLadder(),
				layer, style, selected, drawList);
		break;

	case core::SectorType::Lift:
		// Like every other Transit, a wireframe Layer contributes the Sector outline
		// only. Painting the car filled here leaked it over the selected Layer.
		if (isDrawnSolid(style))
			renderLift(static_pointer_cast<const core::LiftTransit>(sector)->getLift(),
				layer, style, selected, drawList);
		break;

	case core::SectorType::Shuttle:
		// A wireframe Layer contributes a sector outline only. Its vehicle appears
		// when the selected Layer draws the sector itself, or through an aperture.
		if (isDrawnSolid(style))
			renderShuttle(static_pointer_cast<const core::ShuttleTransit>(sector)->getShuttle(),
				layer, style, selected, drawList);
		break;

	case core::SectorType::Stairwell:
		if (shouldRenderStairwellGeometry(style))
			renderStairwell(static_pointer_cast<const core::StairwellTransit>(sector)->getStairwell(),
				layer, style, selected, drawList);
		break;

	case core::SectorType::Staircase:
		if (isDrawnSolid(style))
			renderStaircase(static_pointer_cast<const core::StaircaseTransit>(sector)->getStaircase(), drawList);
		break;

	case core::SectorType::Background:
	case core::SectorType::Facade:
		// A Background has no geometry of its own: its fill is its own opaque
		// colour. Solid and Aperture fill - the latter already clipped to the
		// Window by the caller. The wireframe overlay contributes the outline
		// only, never a fill (ADR 0002), and that outline is the narrowing #35
		// records against the umbrella's "no per-sector border": without it a
		// Background outside an aperture would say nothing about the extent of
		// the Layer behind. Hidden returned at the top. A Facade is drawn the
		// same flat way (ADR 0003); its walkability is invisible in the fill.
		{
			auto const surface = flatSurfaceColour(*sector);
			assert(surface.has_value());
			auto const fill = ImColor(surface->r, surface->g, surface->b, 255);

			if (shouldFillBackground(style))
				drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, fill);
			else if (shouldOutlineBackground(style))
				drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, fill);
		}
		break;

	default:
		break;
	}

	// Objects
	if (isDrawnSolid(style))
	{
		renderSectorObjects(sector, layer, style, RENDER_SECTOR_OBJECTS_INFRONT, drawList);
	}

	// Transit occupants obey the same aperture rule as their transit. Door and
	// Window rendering call this with a clip rectangle active from the Layer in
	// front; the wireframe overlay pass must not expose them.
	if (shouldRenderSectorAgents(sector->getType(), style))
		renderSectorAgents(sector, drawList);

	// Render ceiling
	// A Background has no floor, ceiling or walls - its colour is the whole
	// surface, and a per-sector black border would break the seamless surface
	// adjacent Backgrounds are meant to form. A Facade skips them for the
	// same flat-surface reason: its perimeter is open, so there are no walls
	// to draw, and the flat colour carries its extent (ADR 0003).
	if (renderEdges && !ownColour)
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

	// Selection is an editor overlay. Emit it last so sector-specific fills,
	// passengers, objects, floors, and walls cannot paint over the yellow border.
	// The rule is type-agnostic: a selected Background takes the highlight exactly
	// as a Location does (#35).
	if (sector == gSelectedSector && shouldHighlightSelectedSector(style,
		sector->getLayerIndex(), layer,
		gUISettings.selectionMode == UISettings::SelectionMode::Sector))
	{
		ImVec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
		ImVec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
		drawList->AddRect(topLeft, bottomRight, SelectedColour, 0.0f, 0, 3.0f);
	}
}


void renderLocationContentAboveTransit(shared_ptr<const core::Sector> const& location,
	uint32_t layer, LayerRenderStyle style, ImDrawList* drawList)
{
	if (isDrawnSolid(style))
	{
		renderSectorObjects(location, layer, LayerRenderStyle::Solid,
			RENDER_SECTOR_OBJECTS_BEHIND | RENDER_SECTOR_OBJECTS_INFRONT, drawList);
	}
	renderSectorAgents(location, drawList);
}

//
// Draws one Transit inside a single aperture of the selected Layer. Everything the
// Transit contributes - Sector fill, geometry, and its own Agents - is clipped to
// that aperture by the caller, so a Transit on the Layer behind never fills over
// the Layer in front of it.
//
void renderTransitInAperture(shared_ptr<const core::Sector> const& transit, uint32_t behindLayer,
	TransitAperture const& aperture, ImDrawList* drawList)
{
	// An aperture always looks through to the Layer behind the selection, so it
	// always carries the back-Layer colour.
	auto const colour = BackLocationColour;

	// A Staircase is drawn as its own polyline across the Location it crosses
	// rather than as a filled Sector, so it never contributes a Sector fill
	// through an aperture.
	if (transit->getType() == core::SectorType::Staircase)
	{
		renderStaircase(
			static_pointer_cast<const core::StaircaseTransit>(transit)->getStaircase(), drawList);
		renderSectorAgents(transit, drawList);
		return;
	}

	renderSector(transit, behindLayer, LayerRenderStyle::Aperture, false, colour, drawList);

	// A Ladder sits behind the contents of the Location it lands in, so those
	// contents are redrawn inside the same aperture.
	if (aperture.location && shouldRenderForeContentAfterTransit(transit->getType()))
	{
		renderLocationContentAboveTransit(aperture.location, aperture.location->getLayerIndex(),
			LayerRenderStyle::Aperture, drawList);
	}
}

//
// Draws one Transit of the Layer behind the selection, solid, through each of the
// apertures the selected Layer gives it. A Transit with no aperture is not drawn.
//
void renderTransitThroughApertures(shared_ptr<const core::Sector> const& transit,
	uint32_t behindLayer, std::vector<TransitAperture> const& apertures, ImDrawList* drawList)
{
	static_assert(shouldClipTransitToApertures(LayerRenderStyle::Aperture),
		"this is the clipped pass, so it must only draw a style which clips to apertures");

	if (!transit)
	{
		return;
	}

	for (auto const& aperture : apertures)
	{
		auto bounds0 = aperture.min;
		auto bounds1 = aperture.max;
		transformPosition(bounds0);
		transformPosition(bounds1);

		drawList->AddDrawCmd();

		// ImGui clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) },
			{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) }, true);

		renderTransitInAperture(transit, behindLayer, aperture, drawList);

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
}


//
// Draws every Sector one Layer contributes, in the given style.
//
void renderSectors(shared_ptr<const core::Building> building, uint32_t layer, LayerRenderStyle style,
	ImDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto const sectors = building->getSectorsInBounds(layer, -gUISettings.xOffset, 0,
		gUISettings.worldViewportWidth, gUISettings.worldViewportHeight);

	auto const colour = style == LayerRenderStyle::Solid ? ForeLocationColour : BackLocationColour;

	for (auto sector : sectors)
	{
		renderSector(sector, layer, style, true, colour, drawList);
	}
}


//
// Draws the Transits of the Layer directly behind the selection, solid, clipped to
// the apertures the selected Layer gives them. The selected Layer's Locations are
// the apertures onto the Layer behind, so a Transit never fills over ground the
// Layer in front of it does not open up. A Transit with a Door or clear Window in
// front of it is drawn by that threshold's own aperture as well.
//
// This pass runs whether or not the wireframe overlay is on: the overlay adds the
// Layer behind's outlines, it is not what makes that Layer visible.
//
void renderBehindLayerTransits(shared_ptr<const core::Building> building, uint32_t behindLayer,
	std::vector<std::shared_ptr<const core::Sector>> const& viewSectors, ImDrawList* drawList)
{
	auto const viewLayer = core::layerInFront(behindLayer);

	auto const transits = building->getSectorsInBounds(behindLayer, -gUISettings.xOffset, 0,
		gUISettings.worldViewportWidth, gUISettings.worldViewportHeight);

	for (auto const& transit : transits)
	{
		renderTransitThroughApertures(transit, behindLayer,
			transitApertures(transit, viewLayer, viewSectors), drawList);
	}
}


void renderBuilding(shared_ptr<const core::Building> building)
{
	// The cell-grid lookup a multi-Background aperture composites from (#37)
	// needs the Building; the sector-rendering chain does not carry one.
	gRenderBuilding = building;

	auto drawList = ImGui::GetWindowDrawList();

	auto const layerCount = building->getLayerCount();
	auto const viewLayer = static_cast<uint32_t>(clamp(gUISettings.visibleLayer, 0,
		static_cast<int>(layerCount) - 1));

	// The selected Layer's Sectors are both the apertures onto the Layer directly
	// behind and the Sectors whose controls are redrawn above clipped transits.
	std::vector<std::shared_ptr<const core::Sector>> viewSectors;

	if (viewLayer + 1 < layerCount)
	{
		viewSectors = building->getSectorsInBounds(viewLayer, -gUISettings.xOffset, 0,
			gUISettings.worldViewportWidth, gUISettings.worldViewportHeight);
	}

	for (auto const& pass : renderPasses(viewLayer, layerCount, gUISettings.renderNextLayerWireframe))
	{
		switch (pass.style)
		{
		case LayerRenderStyle::Solid:
			// The selected Layer, drawn whole.
			renderSectors(building, pass.layer, pass.style, drawList);
			break;

		case LayerRenderStyle::Aperture:
			// The Layer directly behind, drawn solid through the apertures the
			// selected Layer gives it.
			renderBehindLayerTransits(building, pass.layer, viewSectors, drawList);

			// Clipped transits intentionally draw over the selected Layer's
			// Locations. Redraw the selected Layer's thresholds, then its controls,
			// then its Agents above those transits.
			renderThresholdsControlsAndAgentsAboveTransit(viewSectors, viewLayer, drawList);
			break;

		case LayerRenderStyle::Wireframe:
			// The wireframe overlay x-rays the Layer directly behind: its whole
			// footprint is outlined over the selection. Outlines only - never a
			// fill, Transit geometry, or the Agents inside them.
			renderSectors(building, pass.layer, pass.style, drawList);
			break;

		case LayerRenderStyle::Hidden:
		default:
			break;
		}
	}

	// Queue diagnostics are selection overlays and should remain visible above
	// the selected object and agents.
	renderSelectedQueues(building, viewLayer, drawList);

	// Grid
	if (gUISettings.renderGrid)
	{
		renderGrid(building, ImColor(128, 128, 127), 1.0f, drawList);
	}
}
