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
#include "core/StairwellLocationVertex.h"
#include "core/StairwellVertex.h"
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
#include "core/StairwellMountEdge.h"
#include "core/StaircaseMountEdge.h"
#include "core/SectorEdge.h"

// SectorObjects
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"

// Transits
#include "core/LadderTransit.h"
#include "core/StairwellTransit.h"


namespace core
{

	using namespace std;

	namespace
	{
		// The order in which a cell scan contributes Vertices.  Vertices sharing a cell
		// and a slot keep their insertion order, so a row is connected in the same
		// order no matter which pass added each Vertex.
		enum VertexSlot : uint32_t
		{
			SlotGapLeft = 0,
			SlotMarker = 1,
			SlotDoor = 2,
			SlotWindow = 3,
			SlotInteractionPoint = 4,
			SlotBulkheadDoor = 5,
			SlotLadderObject = 6,
			SlotLiftObject = 7,
			SlotTransit = 8,
			SlotFloor = 9,
			SlotGapRight = 10
		};
	}

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

	void Graph::addCrossDeckVertex(shared_ptr<VerticalEdgeCreator> edgeCreator, shared_ptr <Vertex> vertex, CrossDeckVertexMap& crossDeckVertices)
	{
		auto it = crossDeckVertices.insert(make_pair(edgeCreator, VertexList()));

		it.first->second.push_back(vertex);
	}

	void Graph::processCrossDeckVertices(CrossDeckVertexMap const& crossDeckVertices)
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

		stable_sort(vertices.begin(), vertices.end(), [](auto a, auto b) {
			return a->getPosition().x < b->getPosition().x;
		});

		auto numVertices = (uint32_t)vertices.size();

		// A Force Bridge owns the only edge which may cross the centre of its
		// span. Interaction-point vertices can lie between its endpoints; ordinary
		// row adjacency must not connect through those vertices and bypass the
		// bridge's traversal resource.
		map<shared_ptr<SectorObject>, pair<float, float>> forceBridgeSpans;
		for (auto const& vertex : vertices)
		{
			if (vertex->getSubType() != VertexSubType::ForceBridge) continue;
			auto objectVertex = dynamic_pointer_cast<SectorObjectVertex>(vertex);
			if (!objectVertex) continue;
			auto [found, inserted] = forceBridgeSpans.try_emplace(objectVertex->getObject(),
				vertex->getPosition().x, vertex->getPosition().x);
			if (!inserted)
			{
				found->second.first = min(found->second.first, vertex->getPosition().x);
				found->second.second = max(found->second.second, vertex->getPosition().x);
			}
		}
		auto crossesForceBridge = [&](shared_ptr<Vertex> const& left, shared_ptr<Vertex> const& right)
		{
			for (auto const& [object, span] : forceBridgeSpans)
			{
				(void)object;
				if (span.second <= span.first) continue;
				auto midpoint = (span.first + span.second) * 0.5f;
				if (left->getPosition().x < midpoint && right->getPosition().x >= midpoint)
					return true;
			}
			return false;
		};

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
					// processForceBridge connects the two sides explicitly because
					// interaction-point vertices can appear between them in this row.
				}
				else if (vertexSubType0 == VertexSubType::Gap && vertexSubType1 == VertexSubType::Gap)
				{
					addEdge(make_shared<GapEdge>(), vertices[i], vertices[j], connectZ);
				}
				else if (!crossesForceBridge(vertices[i], vertices[j]))
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

	void Graph::processMarker(ObjectData const& obj, RowVertices& row)
	{
		ASSERT_INDEX_OK(obj.index);

		// Markers may be created on any Layer
		auto marker = obj.sector->_getObject(obj.index);
		auto markerVertex = marker->createVertex(marker, obj.sector);

		addSectorObjectVertexLookup(marker, markerVertex);
		
		auto vertexIdentifier = marker->getVertexIdentifier();
		if (vertexIdentifier != ~0u)
		{
			mIdentifierVertexLookup[vertexIdentifier] = markerVertex;
		}

		appendRowVertex(row, obj.x, SlotMarker, markerVertex);
	}

	std::shared_ptr<SectorObject> Graph::resolveSectorObject(shared_ptr<Sector> sector, uint32_t index,
		char const* what, uint32_t x, uint32_t y)
	{
		if (sector && index < sector->getNumObjects())
		{
			auto object = sector->_getObject(index);

			if (object)
			{
				return object;
			}
		}

		string msg = format("A {} at {},{} is not an object of Sector '{}', so it is skipped.",
			what, x, y, sector ? sector->getName() : "none");

		mBuildLog.push_back({ "Graph", ~0u, LogLevel::Warning, msg });
		return nullptr;
	}

	void Graph::processDoor(ObjectData const& obj, LayerPairRole role, PositionVertexMap& interLayerVertexLookup, RowVertices& row, CrossDeckVertexMap& crossDeckVertices)
	{
		ASSERT_INDEX_OK(obj.index);

		// A shared Door can have a different object index in each owning Sector.
		// Resolve it through the Layer currently being scanned.
		auto sector = obj.sector;
		auto doorObject = resolveSectorObject(sector, obj.index, "Door", obj.x, obj.y);

		if (!doorObject)
		{
			return;
		}

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

		appendRowVertex(row, obj.x, SlotDoor, vertex);

		// Check whether the Vertex should be connected
		auto cellPos = make_pair(obj.x, obj.y);
		auto it = interLayerVertexLookup.find(cellPos);

		if (role == LayerPairRole::Front)
		{
			// Doors are added to the CellDefinitions of both Layers of their pair, so
			// the front Vertex is recorded here and joined when the Layer directly
			// behind this one is scanned.
			if (it != interLayerVertexLookup.end())
			{
				string errMsg = format("A Door was already set on the front Layer of the pair at {},{}", obj.x, obj.y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}

			interLayerVertexLookup[cellPos] = vertex;
			return;
		}

		if (it == interLayerVertexLookup.end())
		{
			string errMsg = format("A Door was found on the back Layer of the pair at {},{} but not on the Layer in front", obj.x, obj.y);

			mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
			throw BuildingException(mwBuilding, errMsg);
		}

		auto frontVertex = it->second;
		auto door = dynamic_pointer_cast<DoorSectorObject>(doorObject)->getDoor();

		addSectorObjectVertexLookup(doorObject, frontVertex);
		addSectorObjectVertexLookup(doorObject, vertex);

		addEdge(make_shared<DoorEdge>(door), frontVertex, vertex,
			frontVertex->getSector()->getLayerIndex() != vertex->getSector()->getLayerIndex());
	}

	void Graph::processWindow(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, RowVertices& row)
	{
		ASSERT_INDEX_OK(obj.index);

		auto windowObject = resolveSectorObject(obj.sector, obj.index, "Window", obj.x, obj.y);

		if (!windowObject)
		{
			return;
		}

		auto window = dynamic_pointer_cast<WindowSectorObject>(windowObject)->getWindow();
		auto windowVertex = windowObject->createVertex(windowObject, obj.sector);
		addSectorObjectVertexLookup(windowObject, windowVertex);

		auto vertexIdentifier = windowObject->getVertexIdentifier();
		if (vertexIdentifier != ~0u) mIdentifierVertexLookup[vertexIdentifier] = windowVertex;

		// Ordinary windows remain local points of interest. A configured window
		// threshold pairs its two layer vertices and contributes conditional
		// topology through a WindowEdge.
		if (!window->isTraversalConfigured())
		{
			appendRowVertex(row, obj.x, SlotWindow, windowVertex);
			return;
		}

		auto cellPos = make_pair(obj.x, obj.y);
		auto paired = interLayerVertexLookup.find(cellPos);
		if (paired == interLayerVertexLookup.end())
		{
			interLayerVertexLookup[cellPos] = windowVertex;
			appendRowVertex(row, obj.x, SlotWindow, windowVertex);
			return;
		}

		auto other = paired->second;
		appendRowVertex(row, obj.x, SlotWindow, windowVertex);
		addEdge(make_shared<WindowEdge>(window), other, windowVertex,
			other->getSector()->getLayerIndex() != windowVertex->getSector()->getLayerIndex());
	}

	void Graph::processInteractionPoint(ObjectData const& obj, RowVertices& row)
	{
		ASSERT_INDEX_OK(obj.index);
		auto control = obj.sector->_getObject(obj.index);
		auto vertex = control->createVertex(control, obj.sector);
		addSectorObjectVertexLookup(control, vertex);
		if (auto identifier = control->getVertexIdentifier(); identifier != ~0u)
			mIdentifierVertexLookup[identifier] = vertex;
		appendRowVertex(row, obj.x, SlotInteractionPoint, vertex);
	}

	void Graph::processBulkheadDoor(ObjectData const& obj, RowVertices& row)
	{
		ASSERT_INDEX_OK(obj.index);

		// Create two Vertices for this, one on either side.  The Edge will be created later.
		auto door = obj.sector->_getObject(obj.index);
	
		shared_ptr<Vertex> verts[CORE_NUM_SIDES] = {
			door->createVertex(door, obj.adjacent[CORE_SIDE_LEFT]),
			door->createVertex(door, obj.adjacent[CORE_SIDE_RIGHT])
		};

		addSectorObjectVertexLookup(door, verts[CORE_SIDE_LEFT]);
		addSectorObjectVertexLookup(door, verts[CORE_SIDE_RIGHT]);

		for (int i = 0; i < CORE_NUM_SIDES; ++i)
		{
			appendRowVertex(row, obj.x, SlotBulkheadDoor, verts[i]);
		}
	}

	void Graph::processWalkway(ObjectData const& /* obj */, RowVertices& /* row */)
	{
		// Nothing to do here currently.  We don't place vertices down.
	}

	void Graph::processForceBridge(ObjectData const& obj, RowVertices& row)
	{
		ASSERT_INDEX_OK(obj.index);

		auto forceBridgeObject = obj.sector->_getObject(obj.index);

		// Place and connect both bridge vertices here. A physical control may sort
		// between them, so adjacency-based row connection cannot own this edge.
		int side = CORE_SIDE_LEFT;
		auto left = forceBridgeObject->createVertex(forceBridgeObject, obj.sector, &side);
		appendRowVertex(row, obj.x, SlotFloor, left);
		addSectorObjectVertexLookup(forceBridgeObject, left);

		side = CORE_SIDE_RIGHT;
		auto right = forceBridgeObject->createVertex(forceBridgeObject, obj.sector, &side);
		appendRowVertex(row, obj.x, SlotFloor, right);
		addSectorObjectVertexLookup(forceBridgeObject, right);


		auto sectorObject = dynamic_pointer_cast<ForceBridgeSectorObject>(forceBridgeObject);
		addEdge(make_shared<ForceBridgeEdge>(sectorObject->getForceBridge()), left, right, false);
	}

	void Graph::processLadderObject(ObjectData const& obj, RowVertices& row, CrossDeckVertexMap& crossDeckVertices, int level)
	{
		ASSERT_INDEX_OK(obj.index);

		auto sector = obj.sector;
		auto ladderObject = dynamic_pointer_cast<LadderSectorObject>(sector->_getObject(obj.index));
		auto ladder = ladderObject->getLadder();

		auto locationVertex = ladderObject->createVertex(ladderObject, sector, &level);

		addSectorObjectVertexLookup(ladderObject, locationVertex);

		auto vertexIdentifier = ladderObject->getVertexIdentifier();
		if (vertexIdentifier != ~0u)
		{
			mIdentifierVertexLookup[vertexIdentifier] = locationVertex;
		}

		appendRowVertex(row, obj.x, SlotLadderObject, locationVertex);

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

	void Graph::processLiftObject(ObjectData const& obj, RowVertices& row, CrossDeckVertexMap& crossDeckVertices, uint32_t stopOffset)
	{
		ASSERT_INDEX_OK(obj.index);

		auto sector = obj.sector;
		auto liftObject = dynamic_pointer_cast<LiftSectorObject>(sector->_getObject(obj.index));
		auto lift = liftObject->getLift();

		auto sectorVertex = liftObject->createVertex(liftObject, sector, &stopOffset);

		addSectorObjectVertexLookup(liftObject, sectorVertex);

		auto vertexIdentifier = liftObject->getVertexIdentifier();
		if (vertexIdentifier != ~0u)
		{
			mIdentifierVertexLookup[vertexIdentifier] = sectorVertex;
		}

		appendRowVertex(row, obj.x, SlotLiftObject, sectorVertex);

		float xOffset = (liftObject->getCellX() - sector->getCellX()) + liftObject->getSize().x * 0.5f;
		float yOffset = (float)(liftObject->getCellY() - sector->getCellY()) + stopOffset;

		auto liftVertex = make_shared<LiftVertex>(sector, lift, xOffset, yOffset, stopOffset);
		auto liftMountEdge = make_shared<LiftMountEdge>(lift);
		auto connectZ = true;

		addEdge(liftMountEdge, sectorVertex, liftVertex, connectZ);

		// Add LiftVertex to a post-processing list, to join up its vertices with a Ladder edge, later
		addCrossDeckVertex(liftObject, liftVertex, crossDeckVertices);
	}

	void Graph::processLadderTransit(LayerPairRole role, uint32_t curSectorIndex, uint32_t x, uint32_t y, int level, PositionVertexMap& interLayerVertexLookup, RowVertices& row, CrossDeckVertexMap& crossDeckVertices)
	{
		auto sector = mwBuilding->_getSector(curSectorIndex);
		float xOffset = (float)(x - sector->getCellX()) + 0.5f;
		float yOffset = (float)(y - sector->getCellY());

		auto cellPos = make_pair(x, y);

		if (role == LayerPairRole::Front)
		{
			auto locLadderVert = make_shared<SectorMarkerVertex>(sector, xOffset, yOffset);

			appendRowVertex(row, x, SlotTransit, locLadderVert);

			// Add to cross-layer lookup
			if (interLayerVertexLookup.find(cellPos) != interLayerVertexLookup.end())
			{
				string errMsg = format("A LadderTransit was already set on the front Layer of the pair at {},{}", x, y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}

			interLayerVertexLookup[cellPos] = locLadderVert;
			return;
		}

		auto ladderTransit = dynamic_pointer_cast<LadderTransit>(sector);
		auto ladder = ladderTransit->getLadder();
		auto ladderVert = make_shared<LadderVertex>(sector, ladder, xOffset, yOffset, level);

		appendRowVertex(row, x, SlotTransit, ladderVert);

		// Edge
		auto it = interLayerVertexLookup.find(cellPos);

		if (it == interLayerVertexLookup.end())
		{
			string errMsg = format("A LadderTransit was not set on the Layer in front at {},{}", x, y);

			mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
			throw BuildingException(mwBuilding, errMsg);
		}

		addEdge(make_shared<LadderMountEdge>(ladder), it->second, ladderVert,
			it->second->getSector()->getLayerIndex() != ladderVert->getSector()->getLayerIndex());

		// Add Ladder Vertex to lookup for joining up
		addCrossDeckVertex(ladderTransit, ladderVert, crossDeckVertices);
	}

	shared_ptr<Vertex> Graph::createLiftTransitVertex(shared_ptr<LiftTransit> liftTransit, uint32_t x, uint32_t y, CrossDeckVertexMap& crossDeckVertices)
	{
		uint32_t stopOffset = y - liftTransit->getCellY();
		float xOffset = (float)(x - liftTransit->getCellX()) + liftTransit->getCellsWide() * 0.5f;
		float yOffset = (float)stopOffset;

		auto liftVertex = make_shared<LiftVertex>(liftTransit, liftTransit->getLift(), xOffset, yOffset, stopOffset);

		// Add Ladder Vertex to lookup for joining up
		addCrossDeckVertex(liftTransit, liftVertex, crossDeckVertices);

		return liftVertex;
	}

	shared_ptr<Vertex> Graph::createShuttleTransitVertex(shared_ptr<ShuttleTransit> shuttleTransit, uint32_t x, uint32_t y, CrossDeckVertexMap& crossDeckVertices)
	{
		uint32_t stopOffset = x - shuttleTransit->getCellX();
		float xOffset = (float)stopOffset + 0.5f;
		float yOffset = (float)(y - shuttleTransit->getCellY());

		auto shuttleVertex = make_shared<ShuttleVertex>(shuttleTransit, shuttleTransit->getShuttle(), xOffset, yOffset, stopOffset);

		// Add Ladder Vertex to lookup for joining up
		addCrossDeckVertex(shuttleTransit, shuttleVertex, crossDeckVertices);

		return shuttleVertex;
	}

	void Graph::processStairwellTransit(LayerPairRole role, uint32_t curSectorIndex, uint32_t backSectorIndex, uint32_t x, uint32_t y, uint32_t deckOffset, PositionVertexMap& interLayerVertexLookup, RowVertices& row, CrossDeckVertexMap& crossDeckVertices)
	{
		// Stairwell Vertices are probably the most complex to place, as we have to design a usable path through the transit
		// area, whose steps are not too steep.  Also need to take into account the width of Agents as they pass through.
		
		// There are 16 square steps horizontally.  This makes 20 vertically, given it's 2 cells wide.
		// First flight has 5 steps, middle has 10, and top-most has 5 again.
		auto sector = mwBuilding->_getSector(curSectorIndex);
		float xOffset = (float)(x - sector->getCellX()) + 1;
		float yOffset = (float)(y - sector->getCellY());

		auto backSector = mwBuilding->_getSector(backSectorIndex);
		auto stairwellTransit = dynamic_pointer_cast<StairwellTransit>(backSector);

		auto xOffset0 = xOffset;
		auto cellPos = make_pair(x, y);

		if (role == LayerPairRole::Front)
		{
			auto locStairwellVert = make_shared<StairwellLocationVertex>(sector, xOffset0, yOffset);

			appendRowVertex(row, x, SlotTransit, locStairwellVert);

			// Add to cross-layer lookup
			if (interLayerVertexLookup.find(cellPos) != interLayerVertexLookup.end())
			{
				string errMsg = format("A StairwellTransit was already set on the front Layer of the pair at {},{}", x, y);

				mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
				throw BuildingException(mwBuilding, errMsg);
			}

			interLayerVertexLookup[cellPos] = locStairwellVert;
			return;
		}

		auto stairwell = stairwellTransit->getStairwell();
		auto path = stairwell->getDeckPath(deckOffset);
		auto stairwellVert0 = make_shared<StairwellVertex>(sector, stairwell,
			path[0].x, path[0].y, deckOffset);

		appendRowVertex(row, x, SlotTransit, stairwellVert0);

		// Edge
		auto it = interLayerVertexLookup.find(cellPos);

		if (it == interLayerVertexLookup.end())
		{
			string errMsg = format("A StairwellTransit was not set on the Layer in front at {},{}", x, y);

			mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
			throw BuildingException(mwBuilding, errMsg);
		}

		addEdge(make_shared<StairwellMountEdge>(stairwell), it->second, stairwellVert0,
			it->second->getSector()->getLayerIndex() != stairwellVert0->getSector()->getLayerIndex());

		// Add Ladder Vertex to lookup for joining up
		addCrossDeckVertex(stairwellTransit, stairwellVert0, crossDeckVertices);

		// Intermediate Vertices
		if (y < (stairwellTransit->getCellY() + stairwellTransit->getDecksHigh() - 1))
		{
			// Lower landing
			auto stairwellVert1 = make_shared<StairwellVertex>(sector, stairwell,
				path[1].x, path[1].y, deckOffset);

			appendStandaloneRowVertex(row, x, SlotTransit, stairwellVert1);
			addCrossDeckVertex(stairwellTransit, stairwellVert1, crossDeckVertices);

			// Upper landing
			auto stairwellVert2 = make_shared<StairwellVertex>(sector, stairwell,
				path[2].x, path[2].y, deckOffset);

			appendStandaloneRowVertex(row, x, SlotTransit, stairwellVert2);
			addCrossDeckVertex(stairwellTransit, stairwellVert2, crossDeckVertices);
		}
	}

	void Graph::processStaircaseTransit(LayerPairRole role, uint32_t curSectorIndex,
		uint32_t backSectorIndex, uint32_t x, uint32_t y,
		PositionVertexMap& interLayerVertexLookup, RowVertices& row,
		CrossDeckVertexMap& crossDeckVertices)
	{
		auto transit = dynamic_pointer_cast<StaircaseTransit>(mwBuilding->_getSector(backSectorIndex));
		auto staircase = transit->getStaircase();
		auto path = staircase->getPath();
		bool const lower = y == transit->getCellY();
		auto const point = path[lower ? 0 : 1];
		auto cellPos = make_pair(x, y);

		if (role == LayerPairRole::Front)
		{
			auto sector = mwBuilding->_getSector(curSectorIndex);
			// Mount at the Staircase path endpoint rather than the endpoint cell's
			// centre. Z-connected mount vertices must occupy the same global point.
			auto const endpointX = (float)transit->getCellX() + point.x;
			auto locationVertex = make_shared<StairwellLocationVertex>(sector,
				endpointX - (float)sector->getCellX(), (float)(y - sector->getCellY()));
			appendRowVertex(row, x, SlotTransit, locationVertex);
			if (!interLayerVertexLookup.emplace(cellPos, locationVertex).second)
				throw BuildingException(mwBuilding, format("A Transit was already set at {},{}", x, y));
		}
		else
		{
			auto sector = mwBuilding->_getSector(curSectorIndex);
			auto staircaseVertex = make_shared<StaircaseVertex>(sector, staircase, point.x, point.y);
			appendRowVertex(row, x, SlotTransit, staircaseVertex);
			auto found = interLayerVertexLookup.find(cellPos);
			if (found == interLayerVertexLookup.end())
				throw BuildingException(mwBuilding, format("A Staircase endpoint has no Location at {},{}", x, y));
			addEdge(make_shared<StaircaseMountEdge>(staircase), found->second, staircaseVertex, true);
			addCrossDeckVertex(transit, staircaseVertex, crossDeckVertices);
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

	bool Graph::doVerticesCrossSector(uint32_t prevIndex, uint32_t nextIndex, int /* layerIndex */, uint32_t /* nextX */, uint32_t y) const
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

	bool Graph::cellIsProcessable(uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto const& cellDef = mwBuilding->getLayer(layerIndex)->getCellDefinition(x, y);

		if (!cellDef.occupied())
		{
			return false;
		}

		if (cellDef.floorType != CellFloorType::None)
		{
			return true;
		}

		// A Staircase may terminate at an upper Room boundary where the Room itself
		// has no floor; the open adjacent Location is the landing, so that cell is
		// still processed so its mount Vertex can be created.
		if (layerIndex + 1 >= mwBuilding->getLayerCount())
		{
			return false;
		}

		auto const& backCell = mwBuilding->getLayer(layerBehind(layerIndex))->getCellDefinition(x, y);

		if (!backCell.occupied())
		{
			return false;
		}

		auto staircase = dynamic_pointer_cast<StaircaseTransit>(mwBuilding->_getSector(backCell.sectorIndex));

		if (!staircase)
		{
			return false;
		}

		uint32_t const upperX = staircase->getRiseSide() == CORE_SIDE_RIGHT
			? staircase->getCellX() + staircase->getCellsWide() - 1
			: staircase->getCellX();

		return y == staircase->getCellY() + 1 && x == upperX;
	}

	bool Graph::isLeftMostObjectCell(uint32_t layerIndex, uint32_t x, uint32_t y, CellDefinition const& cellDef) const
	{
		if (x == 0) return true;

		auto const& left = mwBuilding->getLayer(layerIndex)->getCellDefinition(x - 1, y);

		// Object indices are per-Sector, so the neighbouring cell only suppresses this
		// one when it is the same object in the same Sector.  Comparing the index alone
		// would hide a left-most object sitting next to an unrelated Sector that happens
		// to use the same index.
		return left.sectorObjectIndex != cellDef.sectorObjectIndex
			|| left.sectorIndex != cellDef.sectorIndex;
	}

	Graph::LayerRows Graph::buildLayerRows() const
	{
		auto const layerCount = mwBuilding->getLayerCount();
		auto const decksHigh = mwBuilding->getDecksHigh();

		LayerRows rows(layerCount);

		for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex)
		{
			auto const layer = mwBuilding->getLayer(layerIndex);
			auto const cellsWide = layer->getCellsWide();

			rows[layerIndex].reserve(decksHigh);

			for (uint32_t y = 0; y < decksHigh; ++y)
			{
				RowVertices row;
				row.layerIndex = layerIndex;
				row.y = y;
				row.segmentOfCell.resize(cellsWide);

				// A row's Vertices are flushed in runs that break whenever the row crosses
				// a Sector boundary that may not be walked through.  Those boundaries
				// depend only on the Sector index sequence along the row, so every pass
				// maps a cell to the same Segment and Vertices from different passes still
				// join the right run.
				uint32_t segment{ 0 };
				uint32_t prevSectorIndex{ ~0u };

				row.segments.emplace_back();

				for (uint32_t x = 0; x < cellsWide; ++x)
				{
					auto const sectorIndex = layer->getCellDefinition(x, y).sectorIndex;

					if (!doVerticesCrossSector(prevSectorIndex, sectorIndex, layerIndex, x, y))
					{
						// Close off the run built so far and open the next one.
						row.flushSector.push_back(prevSectorIndex);
						row.segments.emplace_back();
						++segment;
					}

					row.segmentOfCell[x] = segment;
					prevSectorIndex = sectorIndex;
				}

				row.flushSector.push_back(prevSectorIndex);

				rows[layerIndex].push_back(std::move(row));
			}
		}

		return rows;
	}

	void Graph::appendRowVertex(RowVertices& row, uint32_t x, uint32_t slot, std::shared_ptr<Vertex> vertex) const
	{
		row.segments[row.segmentOfCell[x]].push_back(RowVertex{ x, slot, false, std::move(vertex) });
	}

	void Graph::appendStandaloneRowVertex(RowVertices& row, uint32_t x, uint32_t slot, std::shared_ptr<Vertex> vertex) const
	{
		row.segments[row.segmentOfCell[x]].push_back(RowVertex{ x, slot, true, std::move(vertex) });
	}

	void Graph::flushRowVertices(RowVertices& row)
	{
		for (size_t segment = 0; segment < row.segments.size(); ++segment)
		{
			auto& entries = row.segments[segment];

			stable_sort(entries.begin(), entries.end(), [](RowVertex const& a, RowVertex const& b) {
				if (a.x != b.x) return a.x < b.x;
				return a.slot < b.slot;
			});

			// Standalone Vertices are part of the row's ordering but take no part in
			// connecting it, so they are published before the run they sit in.
			VertexList vertices;
			vertices.reserve(entries.size());

			for (auto const& entry : entries)
			{
				if (entry.standalone)
				{
					mVertices.push_back(entry.vertex);
				}
				else
				{
					vertices.push_back(entry.vertex);
				}
			}

			auto const flushSector = row.flushSector[segment];
			auto prevSector = flushSector != ~0u ? mwBuilding->getSector(flushSector) : nullptr;

			processSectorVertices(vertices, prevSector, row.layerIndex, row.y);
		}
	}

	void Graph::processLayerRow(uint32_t layerIndex, uint32_t y, RowVertices& row, CrossDeckVertexMap& crossDeckVertices)
	{
		auto const layer = mwBuilding->getLayer(layerIndex);
		auto const cellsWide = layer->getCellsWide();

		for (uint32_t x = 0; x < cellsWide; ++x)
		{
			auto const& cellDef = layer->getCellDefinition(x, y);

			if (!cellIsProcessable(layerIndex, x, y))
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

					appendRowVertex(row, x, SlotGapLeft, make_shared<SectorMarkerVertex>(sector, xOffset, yOffset));
				}
			}

			//
			// Process objects
			//
			for (auto markerIndex : cellDef.markers)
			{
				ObjectData obj = { markerIndex, layerIndex, x, y, mwBuilding->_getSector(cellDef.sectorIndex), {} };

				processMarker(obj, row);
			}

			for (int side = 0; side < 3; ++side)
			{
				if (cellDef.controls[side] != ~0u)
				{
					ObjectData obj = { cellDef.controls[side], layerIndex, x, y,
						mwBuilding->_getSector(cellDef.sectorIndex), {} };

					processInteractionPoint(obj, row);
				}
			}

			if (cellDef.bulkheadIndices[CORE_SIDE_RIGHT] != ~0u)
			{
				// There's a bulkhead door to the right
				auto const& rightCellDef = layer->getCellDefinition(x + 1, y);

				ObjectData obj = {
					cellDef.bulkheadIndices[CORE_SIDE_RIGHT],
					layerIndex,
					x, y,
					mwBuilding->_getSector(cellDef.sectorIndex),
					{ mwBuilding->_getSector(cellDef.sectorIndex), mwBuilding->_getSector(rightCellDef.sectorIndex) }
				};

				processBulkheadDoor(obj, row);
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
					ObjectData obj = { thisLadderIndex, layerIndex, x, y,
						mwBuilding->_getSector(cellDef.sectorIndex), {} };

					processLadderObject(obj, row, crossDeckVertices, lowest ? CORE_LEVEL_LOW : CORE_LEVEL_HIGH);
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
				if (stopIndex != ~0u && isLeftMostObjectCell(layerIndex, x, y, cellDef))
				{
					ObjectData obj = { thisLiftIndex, layerIndex, x, y, sector, {} };

					processLiftObject(obj, row, crossDeckVertices, y - liftObject->getCellY());
				}
			}

			// Process floor
			if (cellDef.floorType == CellFloorType::Walkway)
			{
				ObjectData obj = { cellDef.floorIndex, layerIndex, x, y,
					mwBuilding->_getSector(cellDef.sectorIndex), {} };

				processWalkway(obj, row);
			}
			else if (cellDef.floorType == CellFloorType::ForceBridge)
			{
				auto sector = mwBuilding->_getSector(cellDef.sectorIndex);
				auto floorObject = sector->_getObject(cellDef.floorIndex);
				// A multi-cell bridge is referenced by every cell in its span, but
				// contributes one pair of vertices and one traversal edge.
				if (floorObject->getCellX() == x)
				{
					ObjectData obj = { cellDef.floorIndex, layerIndex, x, y, sector, {} };
					processForceBridge(obj, row);
				}
			}

			// See if we need to put a Vertex on the right, if there is a gap
			if (x < (cellsWide - 1))
			{
				auto const& rightCellDef = layer->getCellDefinition(x + 1, y);

				if (neighbourCellInAir(cellDef, rightCellDef, y, CORE_SIDE_RIGHT))
				{
					auto sector = mwBuilding->_getSector(cellDef.sectorIndex);

					// SectorObjectVertex takes an offset within the Sector
					float xOffset = (float)(x - sector->getCellX()) + (1.0f - CORE_AGENT_MAX_WIDTH * 0.5f);
					float yOffset = (float)(y - sector->getCellY());

					appendRowVertex(row, x, SlotGapRight, make_shared<SectorMarkerVertex>(sector, xOffset, yOffset));
				}
			}
		}
	}

	void Graph::processLayerPair(uint32_t frontLayer, uint32_t backLayer, LayerRows& rows, CrossDeckVertexMap& crossDeckVertices)
	{
		// Each adjacent Layer pair gets its own inter-layer Vertex lookup, so a Vertex
		// belonging to one pair can never be joined with a Vertex of another.
		PositionVertexMap interLayerVertexLookup;

		for (uint32_t y = 0; y < mwBuilding->getDecksHigh(); ++y)
		{
			for (uint32_t x = 0; x < mwBuilding->getCellsWide(); ++x)
			{
				processPairCell(frontLayer, backLayer, x, y, interLayerVertexLookup, rows, crossDeckVertices);
			}
		}
	}

	// The Layer a threshold actually crosses, read from the threshold itself rather than
	// from whichever Layer happens to hold a reference to it.  A shared threshold
	// SectorObject is visible from both of its Sectors, so a neighbouring Layer pair can
	// stumble across it; only the pair it was authored on may pair it.
	bool Graph::thresholdBelongsToPair(std::shared_ptr<Sector> sector, uint32_t index,
		SectorObjectType type, uint32_t frontLayer, uint32_t backLayer)
	{
		if (!sector || index >= sector->getNumObjects()) return false;

		auto const object = sector->_getObject(index);

		if (!object) return false;

		if (type == SectorObjectType::Door)
		{
			auto const doorObject = dynamic_pointer_cast<DoorSectorObject>(object);
			if (!doorObject) return false;
			auto const door = doorObject->getDoor();
			return door && door->getFrontLayer() == frontLayer && door->getBackLayer() == backLayer;
		}

		if (type == SectorObjectType::Window)
		{
			auto const windowObject = dynamic_pointer_cast<WindowSectorObject>(object);
			if (!windowObject) return false;
			auto const window = windowObject->getWindow();
			return window && window->getFrontLayer() == frontLayer && window->getBackLayer() == backLayer;
		}

		return false;
	}

	void Graph::processPairCell(uint32_t frontLayer, uint32_t backLayer, uint32_t x, uint32_t y,
		PositionVertexMap& interLayerVertexLookup, LayerRows& rows,
		CrossDeckVertexMap& crossDeckVertices)
	{
		auto const frontLayerPtr = mwBuilding->getLayer(frontLayer);
		auto const backLayerPtr = mwBuilding->getLayer(backLayer);

		auto const& frontCell = frontLayerPtr->getCellDefinition(x, y);
		auto const& backCell = backLayerPtr->getCellDefinition(x, y);

		bool const frontProcessable = cellIsProcessable(frontLayer, x, y);
		bool const backProcessable = cellIsProcessable(backLayer, x, y);

		// Thresholds are authored on the front Layer of the pair and open into the
		// Layer directly behind it.  The front Vertex is recorded first, then joined
		// when the back Layer's copy of the same cell is reached.  A threshold only
		// ever pairs on the pair it belongs to, so a Layer that merely holds a
		// reference to a neighbouring pair's threshold contributes nothing.
		if (frontProcessable && frontCell.sectorObjectType == SectorObjectType::Door
			&& isLeftMostObjectCell(frontLayer, x, y, frontCell)
			&& thresholdBelongsToPair(mwBuilding->_getSector(frontCell.sectorIndex),
				frontCell.sectorObjectIndex, SectorObjectType::Door, frontLayer, backLayer))
		{
			ObjectData obj = { frontCell.sectorObjectIndex, frontLayer, x, y,
				mwBuilding->_getSector(frontCell.sectorIndex), {} };

			processDoor(obj, LayerPairRole::Front, interLayerVertexLookup, rows[frontLayer][y], crossDeckVertices);
		}

		if (frontProcessable && frontCell.sectorObjectType == SectorObjectType::Window
			&& isLeftMostObjectCell(frontLayer, x, y, frontCell)
			&& thresholdBelongsToPair(mwBuilding->_getSector(frontCell.sectorIndex),
				frontCell.sectorObjectIndex, SectorObjectType::Window, frontLayer, backLayer))
		{
			ObjectData obj = { frontCell.sectorObjectIndex, frontLayer, x, y,
				mwBuilding->_getSector(frontCell.sectorIndex), {} };

			processWindow(obj, interLayerVertexLookup, rows[frontLayer][y]);
		}

		if (backProcessable && backCell.sectorObjectType == SectorObjectType::Door
			&& isLeftMostObjectCell(backLayer, x, y, backCell)
			&& thresholdBelongsToPair(mwBuilding->_getSector(backCell.sectorIndex),
				backCell.sectorObjectIndex, SectorObjectType::Door, frontLayer, backLayer))
		{
			ObjectData obj = { backCell.sectorObjectIndex, backLayer, x, y,
				mwBuilding->_getSector(backCell.sectorIndex), {} };

			processDoor(obj, LayerPairRole::Back, interLayerVertexLookup, rows[backLayer][y], crossDeckVertices);
		}

		if (backProcessable && backCell.sectorObjectType == SectorObjectType::Window
			&& isLeftMostObjectCell(backLayer, x, y, backCell)
			&& thresholdBelongsToPair(mwBuilding->_getSector(backCell.sectorIndex),
				backCell.sectorObjectIndex, SectorObjectType::Window, frontLayer, backLayer))
		{
			ObjectData obj = { backCell.sectorObjectIndex, backLayer, x, y,
				mwBuilding->_getSector(backCell.sectorIndex), {} };

			processWindow(obj, interLayerVertexLookup, rows[backLayer][y]);
		}

		// Transits sit on the back Layer of the pair and land on the front Layer.
		if (!backCell.occupied())
		{
			return;
		}

		auto backSector = mwBuilding->_getSector(backCell.sectorIndex);

		if (backSector->getType() == SectorType::Ladder)
		{
			// Want to make sure we only process the lowest and highest cells of the Ladder.
			bool lowest = y == 0 || backLayerPtr->getCellDefinition(x, y - 1).sectorIndex != backCell.sectorIndex;
			bool highest = y == (mwBuilding->getDecksHigh() - 1) || backLayerPtr->getCellDefinition(x, y + 1).sectorIndex != backCell.sectorIndex;

			if (lowest || highest)
			{
				auto const level = lowest ? CORE_LEVEL_LOW : CORE_LEVEL_HIGH;

				if (frontProcessable)
				{
					processLadderTransit(LayerPairRole::Front, frontCell.sectorIndex, x, y, level,
						interLayerVertexLookup, rows[frontLayer][y], crossDeckVertices);
				}

				if (backProcessable)
				{
					processLadderTransit(LayerPairRole::Back, backCell.sectorIndex, x, y, level,
						interLayerVertexLookup, rows[backLayer][y], crossDeckVertices);
				}
			}
		}
		else if (backSector->getType() == SectorType::Stairwell)
		{
			// Stairwells are 2 cells wide, but we only want to process one cell for them.
			auto stairwellTransit = dynamic_pointer_cast<StairwellTransit>(backSector);

			if (stairwellTransit->getCellX() == x)
			{
				auto const deckOffset = y - stairwellTransit->getCellY();

				if (frontProcessable)
				{
					processStairwellTransit(LayerPairRole::Front, frontCell.sectorIndex, backCell.sectorIndex,
						x, y, deckOffset, interLayerVertexLookup, rows[frontLayer][y], crossDeckVertices);
				}

				if (backProcessable)
				{
					processStairwellTransit(LayerPairRole::Back, backCell.sectorIndex, backCell.sectorIndex,
						x, y, deckOffset, interLayerVertexLookup, rows[backLayer][y], crossDeckVertices);
				}
			}
		}
		else if (backSector->getType() == SectorType::Staircase)
		{
			auto staircase = dynamic_pointer_cast<StaircaseTransit>(backSector);
			uint32_t const lowerX = staircase->getRiseSide() == CORE_SIDE_RIGHT
				? staircase->getCellX() : staircase->getCellX() + staircase->getCellsWide() - 1;
			uint32_t const upperX = staircase->getRiseSide() == CORE_SIDE_RIGHT
				? staircase->getCellX() + staircase->getCellsWide() - 1 : staircase->getCellX();

			if ((y == staircase->getCellY() && x == lowerX)
				|| (y == staircase->getCellY() + 1 && x == upperX))
			{
				if (frontProcessable)
				{
					processStaircaseTransit(LayerPairRole::Front, frontCell.sectorIndex, backCell.sectorIndex,
						x, y, interLayerVertexLookup, rows[frontLayer][y], crossDeckVertices);
				}

				if (backProcessable)
				{
					processStaircaseTransit(LayerPairRole::Back, backCell.sectorIndex, backCell.sectorIndex,
						x, y, interLayerVertexLookup, rows[backLayer][y], crossDeckVertices);
				}
			}
		}
	}

	void Graph::build()
	{
		mBuildLog.clear();
		mVertices.clear();
		mEdges.clear();
		mSectorVertexLookup.clear();
		mSectorObjectVertexLookup.clear();

		auto const layerCount = mwBuilding->getLayerCount();

		if (layerCount < 2)
		{
			string errMsg = format("A Building needs at least 2 Layers to build a Graph, but has {}", layerCount);

			mBuildLog.push_back({ "Graph", ~0u, LogLevel::Error, errMsg });
			throw BuildingException(mwBuilding, errMsg);
		}

		// Every Vertex is held in the row Segment that owns it until all passes have
		// contributed, so each run of Vertices is connected exactly once no matter
		// how many Layers the Building has.
		auto rows = buildLayerRows();

		// To connect Vertices between Decks, we store each object with its Vertices, for instance
		// a Lift with each LiftVertex
		CrossDeckVertexMap crossDeckVertexLists;

		// Thresholds and Transits pair each adjacent Layer - 0<->1, 1<->2, and so on -
		// instead of one hard-coded Fore/Back pass.  Each pair is scanned front-first,
		// with its own inter-layer Vertex lookup.
		for (uint32_t frontLayer = 0; frontLayer + 1 < layerCount; ++frontLayer)
		{
			processLayerPair(frontLayer, layerBehind(frontLayer), rows, crossDeckVertexLists);
		}

		// A Layer's own content is scanned once, independently of the pairs it forms.
		for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex)
		{
			for (uint32_t y = 0; y < mwBuilding->getDecksHigh(); ++y)
			{
				processLayerRow(layerIndex, y, rows[layerIndex][y], crossDeckVertexLists);
			}
		}

		// Connect each row, in the Layer and deck order it was scanned in.
		for (uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex)
		{
			for (uint32_t y = 0; y < mwBuilding->getDecksHigh(); ++y)
			{
				flushRowVertices(rows[layerIndex][y]);
			}
		}

		// Connect Layers
		processCrossDeckVertices(crossDeckVertexLists);
	}
	void Graph::validate()
	{
		// Resource/edge authority and control-binding validation is performed by
		// Building::validateTraversalTopology against stable IDs.
	}

} // core