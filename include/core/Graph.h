#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <map>

#include "core/Vertex.h"
#include "core/Edge.h"
#include "core/VerticalEdgeCreator.h"
#include "core/CellDefinition.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/StaircaseTransit.h"
#include "core/Path.h"
#include "core/Log.h"


namespace core
{
	class Building;

	class Graph
	{
		struct ObjectData
		{
			uint32_t index;
			uint32_t layerIndex;
			uint32_t x, y;
			std::shared_ptr<Sector> sectors[2];
		};

		typedef std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<Vertex>> PositionVertexMap;
		typedef std::vector<std::shared_ptr<Vertex>> VertexList;

	private:

		Building* mwBuilding;

		std::vector<std::shared_ptr<const Vertex>> mVertices;

		std::vector<std::shared_ptr<const Edge>> mEdges;

		std::map<Sector const*, VertexList> mSectorVertexLookup;

		std::map<uint32_t, std::shared_ptr<const Vertex>> mIdentifierVertexLookup;

		std::map<std::shared_ptr<SectorObject>, std::vector<std::shared_ptr<Vertex>>> mSectorObjectVertexLookup;


		Log mBuildLog;

	private:

		void addSectorObjectVertexLookup(std::shared_ptr<SectorObject> sectorObject, std::shared_ptr<Vertex> vertex);

		bool neighbourCellInAir(CellDefinition const& cellDef, CellDefinition const& neighbourDef, uint32_t y, int toSide) const;

		bool doVerticesCrossSector(uint32_t prevIndex, uint32_t nextIndex, int layerIndex, uint32_t nextX, uint32_t y) const;

		void processSectorVertices(VertexList& vertices, std::shared_ptr<const Sector> prevSector, uint32_t layerIndex, uint32_t y);

		void processMarker(ObjectData const& obj, VertexList& workVertices);

		void processDoor(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices);

		void processWindow(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices);

		void processInteractionPoint(ObjectData const& obj, VertexList& workVertices);

		void processBulkheadDoor(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices);

		void processWalkway(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices);

		void processForceBridge(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices);

		void processLadderObject(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices, int level);

		void processLiftObject(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices, uint32_t stopOffset);

		void processLadderTransit(int layerIndex, uint32_t curSectorIndex, uint32_t x, uint32_t y, int level, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices);

		void processStairwellTransit(int layerIndex, uint32_t curSectorIndex, uint32_t backSectorIndex, uint32_t x, uint32_t y, uint32_t deckOffset, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices, std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices);

		void processStaircaseTransit(int layerIndex, uint32_t curSectorIndex, uint32_t backSectorIndex,
			uint32_t x, uint32_t y, PositionVertexMap& interLayerVertexLookup, VertexList& workVertices,
			std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices);

		std::shared_ptr<Vertex> createLiftTransitVertex(std::shared_ptr<LiftTransit> liftTransit, uint32_t x, uint32_t y, std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices);

		std::shared_ptr<Vertex> createShuttleTransitVertex(std::shared_ptr<ShuttleTransit> shuttleTransit, uint32_t x, uint32_t y, std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices);

		void addEdge(std::shared_ptr<Edge> edge, std::shared_ptr<Vertex> vertex0, std::shared_ptr<Vertex> vertex1, bool connectZ);

		void addCrossDeckVertex(std::shared_ptr<VerticalEdgeCreator> edgeCreator, std::shared_ptr<Vertex> vertex, std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList>& crossDeckVertices);

		void processCrossDeckVertices(std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList> const& crossDeckVertices);

	public:

		explicit Graph(Building* building);

		virtual ~Graph() = default;

		std::vector<std::shared_ptr<const Vertex>> const& getVertices() const;

		std::vector<std::shared_ptr<const Edge>> const& getEdges() const;

		std::shared_ptr<const Vertex> const getVertexAtPosition(uint32_t layerIndex, float x, float y, float vertexRadius) const;

		std::shared_ptr<const Vertex> getClosestVertexInSector(Sector const* sector, Vector2 const& pos) const;

		std::shared_ptr<const Vertex> getVertexByIdentifier(uint32_t identifier) const;

		Log const& getBuildLog() const;

		std::shared_ptr<Path> calculatePath(Agent const* agent, std::shared_ptr<const Vertex> source, std::shared_ptr<const Vertex> target) const;

		std::shared_ptr<Path> calculatePath(Agent const* agent, std::shared_ptr<const Vertex> target) const;

		void build();

		void validate();
	};

} // core
