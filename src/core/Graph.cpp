#include <cassert>
#include <algorithm>
#include <set>

#include "core/Defines.h"
#include "core/Graph.h"
#include "core/SectorType.h"
#include "core/Building.h"
#include "core/Pathing.h"
#include "core/Exceptions.h"
#include "core/Vector2.h"

// Vertices
#include "core/BulkheadDoorVertex.h"
#include "core/DoorVertex.h"
#include "core/LadderVertex.h"
#include "core/LiftVertex.h"
#include "core/ShuttleVertex.h"
#include "core/SectorObjectVertex.h"
#include "core/SectorMarkerVertex.h"
#include "core/StaircaseLocationVertex.h"
#include "core/StaircaseVertex.h"
#include "core/WindowVertex.h"

// Edges
#include "core/BulkheadDoorEdge.h"
#include "core/WindowEdge.h"
#include "core/DoorEdge.h"
#include "core/ForceBridgeEdge.h"
#include "core/GapEdge.h"
#include "core/LadderMountEdge.h"
#include "core/LiftMountEdge.h"
#include "core/ShuttleMountEdge.h"
#include "core/StaircaseMountEdge.h"
#include "core/SectorEdge.h"

// SectorObjects
#include "core/ForceBridgeSectorObject.h"
#include "core/ControllerSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"

// Transits
#include "core/LadderTransit.h"
#include "core/StaircaseTransit.h"


namespace core
{

	using namespace std;

	Graph::Graph(Building* building)
		: mwBuilding(building)
	{
	}

	vector<shared_ptr<const Vertex>> const& Graph::getVertices() const
	{
		return mVertices;
	}

	vector<shared_ptr<const Edge>> const& Graph::getEdges() const
	{
		return mEdges;
	}
	
	shared_ptr<const Vertex> const Graph::getVertexAtPosition(uint32_t layerIndex, float x, float y, float vertexRadius) const
	{
		core::Vector2 pos{ x, y };

		for (auto const& vertex : mVertices)
		{
			if (vertex->getSector()->getLayerIndex() != layerIndex)
			{
				continue;
			}

			if (pos.distanceTo(vertex->getPosition()) < vertexRadius)
			{
				return vertex;
			}
		}

		return nullptr;
	}

	shared_ptr<const Vertex> Graph::getVertexForController(shared_ptr<Controller> controller) const
	{
		return mControllerVertexLookup.at(controller);
	}

	shared_ptr<const Edge> Graph::getEdgeForInteractableVertex(shared_ptr<const Vertex> vertex) const
	{
		return mInteractableEdgeLookup.at(vertex);
	}

	shared_ptr<const Vertex> Graph::getClosestVertexInSector(Sector const* sector, Vector2 const& pos) const
	{
		auto it = mSectorVertexLookup.find(sector);

		if (it == mSectorVertexLookup.end())
		{
			throw GraphException("Sector not found in Sector->Vertex lookup");
		}

		auto const& vertices = it->second;

		if (vertices.empty())
		{
			throw GraphException("No vertices found in Sector");
		}

		float curDistSq{ numeric_limits<float>::max() };
		shared_ptr<const Vertex> closestVertex{ nullptr };

		for (auto vertex : vertices)
		{
			auto vertexDistSq = pos.distanceToSq(vertex->getPosition());

			if (vertexDistSq < curDistSq)
			{
				curDistSq = vertexDistSq;
				closestVertex = vertex;
			}
		}

		return closestVertex;
	}

	shared_ptr<const Vertex> Graph::getVertexByIdentifier(uint32_t identifier) const
	{
		auto it = mIdentifierVertexLookup.find(identifier);

		if (it == mIdentifierVertexLookup.end())
		{
			throw GraphException(format("Could not find Vertex with identifier {}", identifier));
		}

		return it->second;
	}

	shared_ptr<Path> Graph::calculatePath(Agent const* agent, shared_ptr<const Vertex> source, shared_ptr<const Vertex> target) const
	{
		return pathing::findPath(agent, this, source, target);
	}

	shared_ptr<Path> Graph::calculatePath(Agent const* agent, shared_ptr<const Vertex> target) const
	{
		return pathing::findPath(agent, this, nullptr, target);
	}

	Log const& Graph::getBuildLog() const
	{
		return mBuildLog;
	}

	void Graph::addSectorObjectVertexLookup(shared_ptr<SectorObject> sectorObject, shared_ptr<Vertex> vertex)
	{
		auto it = mSectorObjectVertexLookup.insert(make_pair(sectorObject, vector<shared_ptr<Vertex>>()));

		it.first->second.push_back(vertex);
	}

	void Graph::addEdge(shared_ptr<Edge> edge, shared_ptr<Vertex> vertex0, shared_ptr<Vertex> vertex1, bool connectZ)
	{
		// Check that the Vertices are aligned
		auto v0p = vertex0->getPosition();
		auto v1p = vertex1->getPosition();
		if (connectZ && v0p != v1p)
		{
			string errMsg = format("Z-connected Edge Vertices not aligned: {} -> {}", vertex0->getDescription(), vertex1->getDescription());

			mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg});
			throw BuildingException(mwBuilding, errMsg);
		}

		edge->_setVertex(0, vertex0);
		edge->_setVertex(1, vertex1);
		vertex0->_addEdge(edge);
		vertex1->_addEdge(edge);

		mEdges.push_back(edge);

		mBuildLog.push_back({
			"Graph",
			~0u,
			LogLevel::Debug,
			format("Added Edge")
		});
	}

	void Graph::addCrossDeckVertex(shared_ptr<VerticalEdgeCreator> edgeCreator, shared_ptr <Vertex> vertex, map<shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices)
	{
		auto it = crossDeckVertices.insert(make_pair(edgeCreator, VertexList()));

		it.first->second.push_back(vertex);
	}

	void Graph::processCrossDeckVertices(map<shared_ptr<VerticalEdgeCreator>, VertexList> const& crossDeckVertices)
	{
		for (auto item : crossDeckVertices)
		{
			auto const& [edgeCreator, vertices] = item;

			// Create suitable Edges for the given Area
			auto numVertices = (uint32_t)vertices.size();

			for (uint32_t i = 0; i < numVertices - 1; ++i)
			{
				bool connectZ = vertices[i]->getSector()->getLayerIndex() != vertices[i + 1]->getSector()->getLayerIndex();
				addEdge(edgeCreator->createCrossDeckEdge(edgeCreator), vertices[i], vertices[i + 1], connectZ);
			}
		}
	}

	void Graph::processSectorVertices(VertexList& vertices, shared_ptr<const Sector> prevSector, uint32_t layerIndex, uint32_t y)
	{
		// If there are no vertices, then either we've just started, or we've just processed a Location which
		// doesn't have any.
		if (vertices.empty())
		{
			if (prevSector)
			{
				mBuildLog.push_back({
					"Graph",
					~0u,
					LogLevel::Warning,
					format("Sector '{}' on level {}, layer {} has no vertices.", prevSector->getName(), y, layerIndex)
				});
			}

			return;
		}

		sort(vertices.begin(), vertices.end(), [](auto a, auto b) {
			return a->getPosition().x < b->getPosition().x;
		});

		auto numVertices = (uint32_t)vertices.size();

		// If just one Vertex, then that is fine, but might be unexpected.
		if (numVertices == 1)
		{
			auto sector = vertices[0]->getSector();

			mBuildLog.push_back({
				"Graph",
				~0u,
				LogLevel::Warning,
				format("Sector '{}' has just one vertex.", sector->getDescription())
			});
		}

		for (uint32_t i = 0; i < numVertices - 1; ++i)
		{
			uint32_t j = i + 1;

			auto vertexType0 = vertices[i]->getType();
			auto vertexType1 = vertices[j]->getType();
			auto vertexSubType0 = vertices[i]->getSubType();
			auto vertexSubType1 = vertices[j]->getSubType();

			// Create Edge between i & j
			if (vertexType0 == VertexType::Location && vertexType1 == VertexType::Location)
			{
				bool connectZ = vertices[i]->getSector()->getLayerIndex() != vertices[j]->getSector()->getLayerIndex();

				if (vertexSubType0 == VertexSubType::BulkheadDoor && vertexSubType1 == VertexSubType::BulkheadDoor)
				{
					auto bulkheadVertex = dynamic_pointer_cast<BulkheadDoorVertex>(vertices[i]);
					auto bulkheadDoor = bulkheadVertex->getBulkheadDoor();
					
					addEdge(make_shared<BulkheadDoorEdge>(bulkheadDoor), vertices[i], vertices[j], connectZ);
				}
				else if (vertexSubType0 == VertexSubType::Window && vertexSubType1 == VertexSubType::Window)
				{
					auto windowVertex = dynamic_pointer_cast<WindowVertex>(vertices[i]);
					auto otherWindowVertex = dynamic_pointer_cast<WindowVertex>(vertices[j]);
					if (windowVertex->getWindow() == otherWindowVertex->getWindow()
						&& windowVertex->getWindow()->isTraversalConfigured())
					{
						addEdge(make_shared<WindowEdge>(const_pointer_cast<Window>(windowVertex->getWindow())),
							vertices[i], vertices[j], connectZ);
					}
					else
					{
						addEdge(make_shared<SectorEdge>(), vertices[i], vertices[j], connectZ);
					}
				}
				else if (vertexSubType0 == VertexSubType::ForceBridge && vertexSubType1 == VertexSubType::ForceBridge)
				{
					auto sectorVertex = dynamic_pointer_cast<SectorObjectVertex>(vertices[i]);
					auto forceBridge = dynamic_pointer_cast<ForceBridgeSectorObject>(sectorVertex->getObject());

					addEdge(make_shared<ForceBridgeEdge>(forceBridge->getForceBridge()), vertices[i], vertices[j], connectZ);
				}
				else if (vertexSubType0 == VertexSubType::Gap && vertexSubType1 == VertexSubType::Gap)
				{
					addEdge(make_shared<GapEdge>(), vertices[i], vertices[j], connectZ);
				}
				else
				{
					addEdge(make_shared<SectorEdge>(), vertices[i], vertices[j], connectZ);
				}
			}
		}

		// Add Vertices to Sector lookup
		for (auto vertex : vertices)
		{
			auto sectorPtr = vertex->getSector().get();

			if (mSectorVertexLookup.find(sectorPtr) == mSectorVertexLookup.end())
			{
				mSectorVertexLookup[sectorPtr] = VertexList();
			}

			mSectorVertexLookup[sectorPtr].push_back(vertex);
		}

		// Move processed Vertices to main list
		mVertices.insert(mVertices.end(), make_move_iterator(vertices.begin()), make_move_iterator(vertices.end()));		
		vertices.clear();
	}

	void Graph::processMarker(ObjectData const& obj, VertexList& workVertices)
	{
		ASSERT_INDEX_OK(obj.index);

		// Markers may be created on either Layer
		auto marker = obj.sectors[obj.layerIndex]->_getObject(obj.index);
		auto markerVertex = marker->createVertex(marker, obj.sectors[obj.layerIndex]);

		addSectorObjectVertexLookup(marker, markerVertex);
		
		auto vertexIdentifier = marker->getVertexIdentifier();
		if (vertexIdentifier != ~0u)
		{
			mIdentifierVertexLookup[vertexIdentifier] = markerVertex;
		}

		workVertices.push_back(markerVertex);
	}

	void Graph::processDoor(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, map<shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices)
	{
		ASSERT_INDEX_OK(obj.index);

		// Doors are only ever created on the fore Layer
		auto doorObject = obj.sectors[CORE_LAYER_FORE]->_getObject(obj.index);
		auto sector = obj.sectors[obj.layerIndex];

		// Create Vertex based on Sector type
		shared_ptr<Vertex> vertex;
		uint32_t vertexIdentifier;

		switch (sector->getType())
		{
		case SectorType::Location:
			vertex = doorObject->createVertex(doorObject, sector);

			vertexIdentifier = doorObject->getVertexIdentifier();
			if (vertexIdentifier != ~0u)
			{
				mIdentifierVertexLookup[vertexIdentifier] = vertex;
			}
			break;
		
		case SectorType::Lift:
			vertex = createLiftTransitVertex(dynamic_pointer_cast<LiftTransit>(sector), obj.x, obj.y, crossDeckVertices);
			break;

		case SectorType::Shuttle:
			vertex = createShuttleTransitVertex(dynamic_pointer_cast<ShuttleTransit>(sector), obj.x, obj.y, crossDeckVertices);
			break;

		default:
			throw UnhandledException(sector->getType(), "SectorType");
		}

		workVertices.push_back(vertex);

		// Check whether the Vertex should be connected
		auto cellPos = make_pair(obj.x, obj.y);
		auto it = interLayerVertexLookup.find(cellPos);

		if (it == interLayerVertexLookup.end())
		{
			// Not found.  Doors are added to CellDefinitions on both
			// Layers, so if we're on Layer 0 then add it to the map,
			// and if we're on Layer 1, we have an error.
			if (obj.layerIndex == CORE_LAYER_FORE)
			{
				interLayerVertexLookup[cellPos] = vertex;
			}
			else
			{
				string errMsg = format("A Door was found on Layer 1 CellDefinition at {},{} but not on Layer 0", obj.x, obj.y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}
		}
		else
		{
			// Found.  If we are on Fore Layer, then this should not exist already!
			// Else if on Back Layer, then create an Edge.
			if (obj.layerIndex == CORE_LAYER_FORE)
			{
				string errMsg = format("A Door was already set on Fore Layer CellDefinition at {},{}", obj.x, obj.y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}
			else
			{
				shared_ptr<Vertex> doorVerts[2] = { it->second, vertex };

				if (doorVerts[0]->getSector()->getLayerIndex() == CORE_LAYER_BACK)
				{
					string errMsg = format("A Door was already set on Back Layer CellDefinition at {},{}", obj.x, obj.y);

					mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
					throw BuildingException(mwBuilding, errMsg);
				}

				auto door = dynamic_pointer_cast<DoorSectorObject>(doorObject)->getDoor();
				auto connectZ = doorVerts[CORE_LAYER_FORE]->getSector()->getLayerIndex() != doorVerts[CORE_LAYER_BACK]->getSector()->getLayerIndex();
				
				addSectorObjectVertexLookup(doorObject, doorVerts[0]);
				addSectorObjectVertexLookup(doorObject, doorVerts[1]);

				addEdge(make_shared<DoorEdge>(door), doorVerts[CORE_LAYER_FORE], doorVerts[CORE_LAYER_BACK], connectZ);
			}
		}
	}

	void Graph::processWindow(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices)
	{
		ASSERT_INDEX_OK(obj.index);

		auto windowObject = obj.sectors[obj.layerIndex]->_getObject(obj.index);
		auto window = dynamic_pointer_cast<WindowSectorObject>(windowObject)->getWindow();
		auto windowVertex = windowObject->createVertex(windowObject, obj.sectors[obj.layerIndex]);
		addSectorObjectVertexLookup(windowObject, windowVertex);

		auto vertexIdentifier = windowObject->getVertexIdentifier();
		if (vertexIdentifier != ~0u) mIdentifierVertexLookup[vertexIdentifier] = windowVertex;

		// Ordinary windows remain local points of interest. A configured window
		// threshold pairs its two layer vertices and contributes conditional
		// topology through a WindowEdge.
		if (!window->isTraversalConfigured())
		{
			workVertices.push_back(windowVertex);
			return;
		}

		auto cellPos = make_pair(obj.x, obj.y);
		auto paired = interLayerVertexLookup.find(cellPos);
		if (paired == interLayerVertexLookup.end())
		{
			interLayerVertexLookup[cellPos] = windowVertex;
			workVertices.push_back(windowVertex);
			return;
		}

		auto other = paired->second;
		workVertices.push_back(windowVertex);
		addEdge(make_shared<WindowEdge>(window), other, windowVertex,
			other->getSector()->getLayerIndex() != windowVertex->getSector()->getLayerIndex());
	}

	void Graph::processController(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices)
	{
		ASSERT_INDEX_OK(obj.index);

		// Interactables may be created on either Layer
		auto ctrl = obj.sectors[obj.layerIndex]->_getObject(obj.index);
		auto ctrlVertex = ctrl->createVertex(ctrl, obj.sectors[obj.layerIndex]);

		addSectorObjectVertexLookup(ctrl, ctrlVertex);

		auto vertexIdentifier = ctrl->getVertexIdentifier();
		if (vertexIdentifier != ~0u)
		{
			mIdentifierVertexLookup[vertexIdentifier] = ctrlVertex;
		}

		// Map Interactable to its Vertex, so we can later look up the Vertex we have to path to,
		// to use an Interactable.
		auto ctrlPtr = dynamic_pointer_cast<ControllerSectorObject>(ctrl)->getController();
		mControllerVertexLookup[ctrlPtr] = ctrlVertex;

		workVertices.push_back(ctrlVertex);
	}

	void Graph::processBulkheadDoor(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices)
	{
		ASSERT_INDEX_OK(obj.index);

		// Create two Vertices for this, one on either side.  The Edge will be created later.
		auto door = obj.sectors[obj.layerIndex]->_getObject(obj.index);
	
		shared_ptr<Vertex> verts[CORE_NUM_SIDES] = {
			door->createVertex(door, obj.sectors[CORE_SIDE_LEFT]),
			door->createVertex(door, obj.sectors[CORE_SIDE_RIGHT])
		};

		addSectorObjectVertexLookup(door, verts[CORE_SIDE_LEFT]);
		addSectorObjectVertexLookup(door, verts[CORE_SIDE_RIGHT]);

		for (int i = 0; i < CORE_NUM_SIDES; ++i)
		{
			workVertices.push_back(verts[i]);
		}
	}

	void Graph::processWalkway(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices)
	{
		// Nothing to do here currently.  We don't place vertices down.
	}

	void Graph::processForceBridge(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices)
	{
		ASSERT_INDEX_OK(obj.index);

		auto forceBridge = obj.sectors[obj.layerIndex]->_getObject(obj.index);

		// Place a Vertex on either side.
		int side = CORE_SIDE_LEFT;
		workVertices.push_back(forceBridge->createVertex(forceBridge, obj.sectors[side], &side));

		addSectorObjectVertexLookup(forceBridge, workVertices.back());

		side = CORE_SIDE_RIGHT;
		workVertices.push_back(forceBridge->createVertex(forceBridge, obj.sectors[side], &side));

		addSectorObjectVertexLookup(forceBridge, workVertices.back());
	}

	void Graph::processLadderObject(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, map<shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices, int level)
	{
		ASSERT_INDEX_OK(obj.index);

		auto sector = obj.sectors[obj.layerIndex];
		auto ladderObject = dynamic_pointer_cast<LadderSectorObject>(sector->_getObject(obj.index));
		auto ladder = ladderObject->getLadder();

		auto locationVertex = ladderObject->createVertex(ladderObject, sector, &level);

		addSectorObjectVertexLookup(ladderObject, locationVertex);

		auto vertexIdentifier = ladderObject->getVertexIdentifier();
		if (vertexIdentifier != ~0u)
		{
			mIdentifierVertexLookup[vertexIdentifier] = locationVertex;
		}

		workVertices.push_back(locationVertex);

		float xOffset = (ladderObject->getCellX() - sector->getCellX()) + 0.5f;
		float yOffset = (float)(ladderObject->getCellY() - sector->getCellY());
		
		if (level == CORE_LEVEL_HIGH)
		{
			yOffset += (ladder->getDecksHigh() - 1.0f);
		}
		
		auto ladderVertex = make_shared<LadderVertex>(sector, ladder, xOffset, yOffset, level);
		auto ladderMountEdge = make_shared<LadderMountEdge>(ladder);
		auto connectZ = true;

		addEdge(ladderMountEdge, locationVertex, ladderVertex, connectZ);
	
		// Add LadderVertex to a post-processing list, to join up its vertices with a Ladder edge, later
		addCrossDeckVertex(ladderObject, ladderVertex, crossDeckVertices);
	}

	void Graph::processLiftObject(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, map<shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices, uint32_t stopOffset)
	{
		ASSERT_INDEX_OK(obj.index);

		auto sector = obj.sectors[obj.layerIndex];
		auto liftObject = dynamic_pointer_cast<LiftSectorObject>(sector->_getObject(obj.index));
		auto lift = liftObject->getLift();

		auto sectorVertex = liftObject->createVertex(liftObject, sector, &stopOffset);

		addSectorObjectVertexLookup(liftObject, sectorVertex);

		auto vertexIdentifier = liftObject->getVertexIdentifier();
		if (vertexIdentifier != ~0u)
		{
			mIdentifierVertexLookup[vertexIdentifier] = sectorVertex;
		}

		workVertices.push_back(sectorVertex);

		float xOffset = (liftObject->getCellX() - sector->getCellX()) + liftObject->getSize().x * 0.5f;
		float yOffset = (float)(liftObject->getCellY() - sector->getCellY()) + stopOffset;

		auto liftVertex = make_shared<LiftVertex>(sector, lift, xOffset, yOffset, stopOffset);
		auto liftMountEdge = make_shared<LiftMountEdge>(lift);
		auto connectZ = true;

		addEdge(liftMountEdge, sectorVertex, liftVertex, connectZ);

		// Add LiftVertex to a post-processing list, to join up its vertices with a Ladder edge, later
		addCrossDeckVertex(liftObject, liftVertex, crossDeckVertices);
	}

	void Graph::processLadderTransit(int layerIndex, uint32_t curSectorIndex, uint32_t x, uint32_t y, int level, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, map<shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices)
	{
		auto sector = mwBuilding->_getSector(curSectorIndex);
		float xOffset = (float)(x - sector->getCellX()) + 0.5f;
		float yOffset = (float)(y - sector->getCellY());

		// Fore Vertex
		if (layerIndex == CORE_LAYER_FORE)
		{
			auto locLadderVert = make_shared<SectorMarkerVertex>(sector, xOffset, yOffset);
			
			workVertices.push_back(locLadderVert);

			// Add to cross-layer lookup
			auto cellPos = make_pair(x, y);
			auto it = interLayerVertexLookup.find(cellPos);

			if (it == interLayerVertexLookup.end())
			{
				interLayerVertexLookup[cellPos] = locLadderVert;
			}
			else
			{
				string errMsg = format("A LadderTransit was already set on Layer 0 CellDefinition at {},{}", x, y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}
		}

		// Back Vertex
		if (layerIndex == CORE_LAYER_BACK)
		{
			auto ladderTransit = dynamic_pointer_cast<LadderTransit>(sector);
			auto ladder = ladderTransit->getLadder();
			auto ladderVert = make_shared<LadderVertex>(sector, ladder, xOffset, yOffset, level);
			
			workVertices.push_back(ladderVert);

			// Edge
			auto cellPos = make_pair(x, y);
			auto it = interLayerVertexLookup.find(cellPos);

			if (it != interLayerVertexLookup.end())
			{
				auto connectZ = interLayerVertexLookup[cellPos]->getSector()->getLayerIndex() != ladderVert->getSector()->getLayerIndex();
				addEdge(make_shared<LadderMountEdge>(ladder), interLayerVertexLookup[cellPos], ladderVert, connectZ);
			}
			else
			{
				string errMsg = format("A LadderTransit was not set on Layer 0 CellDefinition at {},{}", x, y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}

			// Add Ladder Vertex to lookup for joining up
			addCrossDeckVertex(ladderTransit, ladderVert, crossDeckVertices);
		}
	}

	shared_ptr<Vertex> Graph::createLiftTransitVertex(shared_ptr<LiftTransit> liftTransit, uint32_t x, uint32_t y, map<shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices)
	{
		uint32_t stopOffset = y - liftTransit->getCellY();
		float xOffset = (float)(x - liftTransit->getCellX()) + liftTransit->getCellsWide() * 0.5f;
		float yOffset = (float)stopOffset;

		auto liftVertex = make_shared<LiftVertex>(liftTransit, liftTransit->getLift(), xOffset, yOffset, stopOffset);

		// Add Ladder Vertex to lookup for joining up
		addCrossDeckVertex(liftTransit, liftVertex, crossDeckVertices);

		return liftVertex;
	}

	shared_ptr<Vertex> Graph::createShuttleTransitVertex(shared_ptr<ShuttleTransit> shuttleTransit, uint32_t x, uint32_t y, map<shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices)
	{
		uint32_t stopOffset = x - shuttleTransit->getCellX();
		float xOffset = (float)stopOffset + 0.5f;
		float yOffset = (float)(y - shuttleTransit->getCellY());

		auto shuttleVertex = make_shared<ShuttleVertex>(shuttleTransit, shuttleTransit->getShuttle(), xOffset, yOffset, stopOffset);

		// Add Ladder Vertex to lookup for joining up
		addCrossDeckVertex(shuttleTransit, shuttleVertex, crossDeckVertices);

		return shuttleVertex;
	}

	void Graph::processStaircaseTransit(int layerIndex, uint32_t curSectorIndex, uint32_t backSectorIndex, uint32_t x, uint32_t y, uint32_t deckOffset, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, map<shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices)
	{
		// Staircase Vertices are probably the most complex to place, as we have to design a useable path through the transit
		// area, whose steps are not too steep.  Also need to take into account the width of Agents as they pass through.
		
		// There are 16 square steps horizontally.  This makes 20 vertically, given it's 2 cells wide.
		// First flight has 5 steps, middle has 10, and top-most has 5 again.
		auto sector = mwBuilding->_getSector(curSectorIndex);
		float xOffset = (float)(x - sector->getCellX()) + 1;
		float yOffset = (float)(y - sector->getCellY());

		auto backSector = mwBuilding->_getSector(backSectorIndex);
		auto staircaseTransit = dynamic_pointer_cast<StaircaseTransit>(backSector);
		auto mountSide = staircaseTransit->getMountSide();

		auto xOffset0 = xOffset;

		// Fore Vertex
		if (layerIndex == CORE_LAYER_FORE)
		{
			auto locStaircaseVert = make_shared<StaircaseLocationVertex>(sector, xOffset0, yOffset);

			workVertices.push_back(locStaircaseVert);

			// Add to cross-layer lookup
			auto cellPos = make_pair(x, y);
			auto it = interLayerVertexLookup.find(cellPos);

			if (it == interLayerVertexLookup.end())
			{
				interLayerVertexLookup[cellPos] = locStaircaseVert;
			}
			else
			{
				string errMsg = format("A StaircaseTransit was already set on Layer 0 CellDefinition at {},{}", x, y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}
		}

		// Back Vertex
		if (layerIndex == CORE_LAYER_BACK)
		{
			auto staircase = staircaseTransit->getStaircase();
			auto staircaseVert0 = make_shared<StaircaseVertex>(sector, staircase, xOffset0, yOffset, deckOffset);

			workVertices.push_back(staircaseVert0);

			// Edge
			auto cellPos = make_pair(x, y);
			auto it = interLayerVertexLookup.find(cellPos);

			if (it != interLayerVertexLookup.end())
			{
				auto connectZ = interLayerVertexLookup[cellPos]->getSector()->getLayerIndex() != staircaseVert0->getSector()->getLayerIndex();
				addEdge(make_shared<StaircaseMountEdge>(staircase), interLayerVertexLookup[cellPos], staircaseVert0, connectZ);
			}
			else
			{
				string errMsg = format("A StaircaseTransit was not set on Layer 0 CellDefinition at {},{}", x, y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}

			// Add Ladder Vertex to lookup for joining up
			addCrossDeckVertex(staircaseTransit, staircaseVert0, crossDeckVertices);

			// Intermediate Vertices
			if (y < (staircaseTransit->getCellY() + staircaseTransit->getDecksHigh() - 1))
			{
				// Lower landing
				auto xOffset1 = xOffset + (mountSide == CORE_SIDE_LEFT ? 0.666f : -0.666f);

				auto staircaseVert1 = make_shared<StaircaseVertex>(sector, staircase, xOffset1, yOffset + 0.25f, deckOffset);

				mVertices.push_back(staircaseVert1);
				addCrossDeckVertex(staircaseTransit, staircaseVert1, crossDeckVertices);

				// Upper landing
				auto xOffset2 = xOffset + (mountSide == CORE_SIDE_RIGHT ? 0.666f : -0.666f);;

				auto staircaseVert2 = make_shared<StaircaseVertex>(sector, staircase, xOffset2, yOffset + 0.75f, deckOffset);

				mVertices.push_back(staircaseVert2);
				addCrossDeckVertex(staircaseTransit, staircaseVert2, crossDeckVertices);
			}
		}
	}

	bool Graph::neighbourCellInAir(CellDefinition const& cellDef, CellDefinition const& neighbourDef, uint32_t y, int toSide) const
	{
		// Check to see if we are at the edge, with air to our side.  We may want to place a Vertex at such
		// an edge.  Criteria are that current cell is walkable but neighbour isn't, and that we aren't
		// crossing through a wall.
		if (!cellDef.isTraversableOnFoot())
		{
			return false;
		}

		if (!neighbourDef.occupied())
		{
			return false;
		}

		if (neighbourDef.isTraversableOnFoot())
		{
			return false;
		}

		if (cellDef.sectorIndex != neighbourDef.sectorIndex)
		{
			auto sector0 = mwBuilding->getSector(toSide == CORE_SIDE_LEFT ? neighbourDef.sectorIndex : cellDef.sectorIndex);
			auto sector1 = mwBuilding->getSector(toSide == CORE_SIDE_LEFT ? cellDef.sectorIndex : neighbourDef.sectorIndex);
		
			if (sector0->getEndType(y - sector0->getCellY(), CORE_SIDE_RIGHT) == SectorEndType::Wall)
			{
				return false;
			}
			if (sector1->getEndType(y - sector1->getCellY(), CORE_SIDE_LEFT) == SectorEndType::Wall)
			{
				return false;
			}
		}

		return true;
	}

	bool Graph::doVerticesCrossSector(uint32_t prevIndex, uint32_t nextIndex, int layerIndex, uint32_t nextX, uint32_t y) const
	{
		if (prevIndex == nextIndex)
		{
			return prevIndex != ~0u;
		}

		auto prevSector = prevIndex != ~0u ? mwBuilding->getSector(prevIndex) : nullptr;
		auto nextSector = nextIndex != ~0u ? mwBuilding->getSector(nextIndex) : nullptr;

		// We need to process the current vertices if we reach the end of a Location and there is a wall, or
		// a void.  We also want to do this if the end is free, but the next Location's floor is not at the
		// same level as this: we can't climb up or down.
		auto endType = nextSector
			? nextSector->getEndType(y - nextSector->getCellY(), CORE_SIDE_LEFT)
			: prevSector->getEndType(y - prevSector->getCellY(), CORE_SIDE_RIGHT);

		bool endTypeIsWall = endType == SectorEndType::Wall;

		return nextSector && !endTypeIsWall;
	}

	vector<shared_ptr<VertexController>> Graph::build()
	{
		mBuildLog.clear();
		mVertices.clear();
		mEdges.clear();
		mControllerVertexLookup.clear();
		mInteractableEdgeLookup.clear();
		mSectorVertexLookup.clear();
		mSectorObjectVertexLookup.clear();

		vector<shared_ptr<VertexController>> vertexControllers;

		// List of horizontal vertices, built up as we scan a Deck left to right
		vector<shared_ptr<Vertex>> workVertices;

		// To connect Vertices between Layers, we store the relevant Vertices in a map, keyed
		// on cell position.
		PositionVertexMap interLayerVertexLookup;

		// To connect Vertices between Decks, we store each object with its Vertices, for instance
		// a Lift with each LiftVertex
		map<shared_ptr<VerticalEdgeCreator>, VertexList> crossDeckVertexLists;

		// Go through each Layer, deck by deck, building up Vertices and Edges left-to-right.
		shared_ptr<Layer> layers[2] = {
			mwBuilding->getLayer(0),
			mwBuilding->getLayer(1)
		};

		for (uint32_t layerIndex = 0; layerIndex < CORE_NUM_LAYERS; ++layerIndex)
		{
			auto layer = layers[layerIndex];
			for (uint32_t y = 0; y < mwBuilding->getDecksHigh(); ++y)
			{
				uint32_t curSectorIndex{ ~0u };
				for (uint32_t x = 0; x < mwBuilding->getCellsWide(); ++x)
				{
					auto const& cellDef = layer->getCellDefinition(x, y);
					 
					// See if we need to join up Vertices
					if (!doVerticesCrossSector(curSectorIndex, cellDef.sectorIndex, layerIndex, x, y))
					{
						auto prevSector = curSectorIndex != ~0u ? mwBuilding->getSector(curSectorIndex) : nullptr;
						processSectorVertices(workVertices, prevSector, layerIndex, y);
					}

					curSectorIndex = cellDef.sectorIndex;

					if (cellDef.sectorIndex == ~0u ||
						cellDef.floorType == CellFloorType::None)
					{
						continue;
					}

					// See if we need to put a Vertex on the left, if there is a gap.
					// We need to check that we don't pass through a wall while checking this, for instance
					// if the cell to the immediate left is part of a lift shaft.
					if (x > 0)
					{
						auto const& leftCellDef = layer->getCellDefinition(x - 1, y);

						if (neighbourCellInAir(cellDef, leftCellDef, y, CORE_SIDE_LEFT))
						{
							auto sector = mwBuilding->_getSector(cellDef.sectorIndex);
							
							// SectorObjectVertex takes an offset within the Sector
							float xOffset = (float)(x - sector->getCellX()) + CORE_AGENT_MAX_WIDTH * 0.5f;
							float yOffset = (float)(y - sector->getCellY());

							workVertices.push_back(make_shared<SectorMarkerVertex>(sector, xOffset, yOffset));
						}
					}

					//
					// Process objects
					//
					for (auto markerIndex : cellDef.markers)
					{
						auto cellDef0 = layers[0]->getCellDefinition(x, y);
						auto cellDef1 = layers[1]->getCellDefinition(x, y);

						ObjectData obj = {
							markerIndex,
							layerIndex,
							x, y,
							{
								layerIndex == 0 ? mwBuilding->_getSector(cellDef0.sectorIndex) : nullptr,
								layerIndex == 1 ? mwBuilding->_getSector(cellDef1.sectorIndex) : nullptr
							}
						};

						processMarker(obj, workVertices);
					}

					if (cellDef.sectorObjectType == SectorObjectType::Door)
					{					
						// Only process one cell, so if this Door is wider than one, just
						// process left-most
						if (x == 0 || layers[0]->getCellDefinition(x - 1, y).sectorObjectIndex != cellDef.sectorObjectIndex)
						{
							// Get both Layer Locations here, as Doors need them both
							auto cellDef0 = layers[0]->getCellDefinition(x, y);
							auto cellDef1 = layers[1]->getCellDefinition(x, y);

							ObjectData obj = {
								cellDef.sectorObjectIndex,
								layerIndex,
								x, y,
								{
									cellDef0.sectorIndex != ~0u ? mwBuilding->_getSector(cellDef0.sectorIndex) : nullptr,
									cellDef1.sectorIndex != ~0u ? mwBuilding->_getSector(cellDef1.sectorIndex) : nullptr
								}
							};

							processDoor(
								obj,
								interLayerVertexLookup,
								workVertices,
								crossDeckVertexLists
							);
						}
					}
					if (cellDef.sectorObjectType == SectorObjectType::Window)
					{
						// Only process one cell, so if this Window is wider than one, just
						// process left-most. The shared object can have a different index
						// in each sector, so compare within the layer being scanned.
						if (x == 0 || layer->getCellDefinition(x - 1, y).sectorObjectIndex != cellDef.sectorObjectIndex)
						{
							// Get both Layer Locations here, as Windows need them both
							auto cellDef0 = layers[0]->getCellDefinition(x, y);
							auto cellDef1 = layers[1]->getCellDefinition(x, y);

							ObjectData obj = {
								cellDef.sectorObjectIndex,
								layerIndex,
								x, y,
								{
									cellDef0.sectorIndex != ~0u ? mwBuilding->_getSector(cellDef0.sectorIndex) : nullptr,
									cellDef1.sectorIndex != ~0u ? mwBuilding->_getSector(cellDef1.sectorIndex) : nullptr
								}
							};

							processWindow(
								obj,
								interLayerVertexLookup,
								workVertices
							);
						}
					}

					for (int side = 0; side < 3; ++side)
					{
						if (cellDef.controllers[side] != ~0u)
						{
							auto cellDef0 = layers[0]->getCellDefinition(x, y);
							auto cellDef1 = layers[1]->getCellDefinition(x, y);

							ObjectData obj = {
								cellDef.controllers[side],
								layerIndex,
								x, y,
								{
									layerIndex == 0 ? mwBuilding->_getSector(cellDef0.sectorIndex) : nullptr,
									layerIndex == 1 ? mwBuilding->_getSector(cellDef1.sectorIndex) : nullptr
								}
							};

							processController(
								obj,
								interLayerVertexLookup,
								workVertices
							);
						}
					}

					if (cellDef.bulkheadIndices[CORE_SIDE_LEFT] != ~0u)
					{
						// Handled below.
						// TODO: should we create one vertex here?
					}
					if (cellDef.bulkheadIndices[CORE_SIDE_RIGHT] != ~0u)
					{
						// There's a bulkhead door to the right
						auto const& cellDef1 = layer->getCellDefinition(x + 1, y);
						
						ObjectData obj = {
							cellDef.bulkheadIndices[CORE_SIDE_RIGHT],
							layerIndex,
							x, y,
							{
								mwBuilding->_getSector(cellDef.sectorIndex),
								mwBuilding->_getSector(cellDef1.sectorIndex)
							}
						};

						processBulkheadDoor(
							obj,
							interLayerVertexLookup,
							workVertices
						);
					}

					// Sector objects
					if (cellDef.sectorObjectType == SectorObjectType::Ladder)
					{
						// Only add if on the lowest or upper floor
						auto thisLadderIndex = cellDef.sectorObjectIndex;

						// Check to see if we want to add a Vertex on either the lowest or highest floor
						bool lowest = y == 0 || layer->getCellDefinition(x, y - 1).sectorObjectIndex != thisLadderIndex;
						bool highest = y == (mwBuilding->getDecksHigh() - 1) || layer->getCellDefinition(x, y + 1).sectorObjectIndex != thisLadderIndex;

						if (lowest || highest)
						{
							ObjectData obj = {
								thisLadderIndex,
								layerIndex,
								x, y,
								{
									mwBuilding->_getSector(cellDef.sectorIndex),
									mwBuilding->_getSector(cellDef.sectorIndex)
								}
							};

							processLadderObject(
								obj,
								interLayerVertexLookup,
								workVertices,
								crossDeckVertexLists,
								lowest ? CORE_LEVEL_LOW : CORE_LEVEL_HIGH
							);
						}
					}
					else if (cellDef.sectorObjectType == SectorObjectType::Lift)
					{
						// Only add if there's a stop here
						auto thisLiftIndex = cellDef.sectorObjectIndex;

						auto sector = mwBuilding->_getSector(cellDef.sectorIndex);
						auto liftObject = dynamic_pointer_cast<LiftSectorObject>(sector->_getObject(thisLiftIndex));
						auto lift = liftObject->getLift();

						auto stopIndex = lift->getStopIndex(x, y);

						// Only process one cell, so if this Lift is wider than one, just process left-most
						if (stopIndex != ~0u && 
							(x == 0 || layers[0]->getCellDefinition(x - 1, y).sectorObjectIndex != cellDef.sectorObjectIndex))
						{
							ObjectData obj = {
								thisLiftIndex,
								layerIndex,
								x, y,
								{
									mwBuilding->_getSector(cellDef.sectorIndex),
									mwBuilding->_getSector(cellDef.sectorIndex)
								}
							};

							processLiftObject(
								obj,
								interLayerVertexLookup,
								workVertices,
								crossDeckVertexLists,
								y - liftObject->getCellY()
							);
						}
					}

					// See if there is a Transit on the back layer.
					auto backCellDef = layers[CORE_LAYER_BACK]->getCellDefinition(x, y);

					if (backCellDef.occupied())
					{
						auto backSector = mwBuilding->_getSector(backCellDef.sectorIndex);

						if (backSector->getType() == SectorType::Ladder)
						{
							// Want to make sure we only process the lowest and highest cells of the Ladder.
							bool lowest = y == 0 || layers[CORE_LAYER_BACK]->getCellDefinition(x, y - 1).sectorIndex != backCellDef.sectorIndex;
							bool highest = y == (mwBuilding->getDecksHigh() - 1) || layers[CORE_LAYER_BACK]->getCellDefinition(x, y + 1).sectorIndex != backCellDef.sectorIndex;

							if (lowest || highest)
							{
								processLadderTransit(layerIndex, curSectorIndex, x, y, lowest ? CORE_LEVEL_LOW : CORE_LEVEL_HIGH, interLayerVertexLookup, workVertices, crossDeckVertexLists);
							}
						}
						else if (backSector->getType() == SectorType::Staircase)
						{
							// Staircases are 2 cells wide, but we only want to process one cell for them.
							auto staircaseTransit = dynamic_pointer_cast<StaircaseTransit>(backSector);

							if (staircaseTransit->getCellX() == x)
							{
								processStaircaseTransit(layerIndex, curSectorIndex, backCellDef.sectorIndex, x, y, y - staircaseTransit->getCellY(), interLayerVertexLookup, workVertices, crossDeckVertexLists);
							}
						}
					}

					// Process floor
					if (cellDef.floorType == CellFloorType::Walkway)
					{
						ObjectData obj = {
							cellDef.floorIndex,
							layerIndex,
							x, y,
							{
								layerIndex == 0 ? mwBuilding->_getSector(cellDef.sectorIndex) : nullptr,
								layerIndex == 1 ? mwBuilding->_getSector(cellDef.sectorIndex) : nullptr
							}
						};

						processWalkway(
							obj,
							interLayerVertexLookup,
							workVertices
						);
					}
					else if (cellDef.floorType == CellFloorType::ForceBridge)
					{
						ObjectData obj = {
							cellDef.floorIndex,
							layerIndex,
							x, y,
							{
								mwBuilding->_getSector(cellDef.sectorIndex),
								mwBuilding->_getSector(cellDef.sectorIndex)
							}
						};

						processForceBridge(
							obj,
							interLayerVertexLookup,
							workVertices
						);
					}


					// See if we need to put a Vertex on the right, if there is a gap
					if (x < (layer->getCellsWide() - 1))
					{
						auto const& rightCellDef = layer->getCellDefinition(x + 1, y);

						if (neighbourCellInAir(cellDef, rightCellDef, y, CORE_SIDE_RIGHT))
						{
							auto sector = mwBuilding->_getSector(cellDef.sectorIndex);

							// SectorObjectVertex takes an offset within the Sector
							float xOffset = (float)(x - sector->getCellX()) + (1.0f - CORE_AGENT_MAX_WIDTH * 0.5f);
							float yOffset = (float)(y - sector->getCellY());

							workVertices.push_back(make_shared<SectorMarkerVertex>(sector, xOffset, yOffset));
						}
					}
				}

				// Process work vertices
				auto prevSector = curSectorIndex != ~0u ? mwBuilding->getSector(curSectorIndex) : nullptr;
				processSectorVertices(workVertices, prevSector, layerIndex, y);
			}
		}

		// Connect Layers
		processCrossDeckVertices(crossDeckVertexLists);

		// Create Vertex Controllers
		for (auto item : mSectorObjectVertexLookup)
		{
			auto const& [sectorObject, vertices] = item;

			// TODO: need to get the Vertices for any Controllers which this SectorObject requires
			//       and pass them into createVertexController, but in a structured way, so
			//       the createVertexController() implementation knows what to do with them.
			//       mControllerVertexLookup maps Controller to Vertex, so need a map from
			//       SectorObject to the Controllers which control it.

			auto vertexController = sectorObject->createVertexController(mwBuilding, vertices, mControllerVertexLookup);

			if (vertexController)
			{
				vertexControllers.push_back(vertexController);
			}
		}

		return vertexControllers;
	}

	void Graph::validateEdgeController(shared_ptr<const Edge> edge, shared_ptr<const Controller> controller)
	{
		for (uint32_t i = 0; i < 2; ++i)
		{
			auto vertex = edge->getVertex(i);

			for (auto vertexEdge : vertex->getEdges())
			{
				if (!vertexEdge->sameAs(edge))
				{
					auto otherVertex = vertexEdge->getOtherVertex(vertex);

					if (otherVertex->getSubType() == VertexSubType::Interactable)
					{
						auto otherControllerVertex = dynamic_pointer_cast<const ControllerVertex>(otherVertex);
						auto otherController = otherControllerVertex->getController();

						if (otherController == controller)
						{
							auto entry = make_pair(otherVertex, vertexEdge);
							auto res = mInteractableEdgeLookup.insert(entry);
							
							return;
						}
					}
				}
			}
		}

		auto errMsg = format("Controller '{}' for Edge '{}' needs to be next to it.", 
			controller->getDescription(), edge->getDescription());

		throw GraphException(errMsg);
	}

	void Graph::validateEdgeControllers()
	{
		array<int, 2> sides = { CORE_SIDE_LEFT, CORE_SIDE_RIGHT };
		array<uint32_t, 2> layerIndices = { CORE_LAYER_FORE, CORE_LAYER_BACK };

		for (auto edge : mEdges)
		{
			for (int side : sides)
			{
				for (uint32_t layerIndex : layerIndices)
				{
					auto controller = edge->getDependingController(side, layerIndex);

					if (controller)
					{
						validateEdgeController(edge, controller);
					}
				}
			}
		}
	}

	void Graph::validate()
	{
		validateEdgeControllers();
	}

} // core