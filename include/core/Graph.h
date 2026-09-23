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
	class World;

	// Which side of an adjacent Layer pair a threshold or Transit is being processed
	// as.  A pair is always processed front-first, so the front Vertex of a pairing
	// is recorded before the back Vertex tries to join it.
	enum struct LayerPairRole
	{
		Front,
		Back
	};

	class Graph
	{
		struct ObjectData
		{
			uint32_t index;
			uint32_t layerIndex;
			uint32_t x, y;

			// Sector occupying the cell on the Layer being processed.
			std::shared_ptr<Sector> sector;

			// Bulkhead Doors span two Locations side by side, so they need both.
			std::shared_ptr<Sector> adjacent[CORE_NUM_SIDES];
		};

		typedef std::map<std::pair<uint32_t, uint32_t>, std::shared_ptr<Vertex>> PositionVertexMap;
		typedef std::vector<std::shared_ptr<Vertex>> VertexList;
		typedef std::map<std::shared_ptr<VerticalEdgeCreator>, VertexList> CrossDeckVertexMap;

		// A Vertex waiting in a Layer row, tagged with the cell and the scan slot that
		// produced it.  A row is connected in (cell, slot) order, which keeps the scan
		// order deterministic now that several passes feed the same row.
		struct RowVertex
		{
			uint32_t x;
			uint32_t slot;

			// A standalone Vertex belongs to the row's ordering but takes no part in
			// connecting it.
			bool standalone{ false };

			std::shared_ptr<Vertex> vertex;
		};

		// One row of one Layer, split into the Segments that are flushed
		// independently as the row crosses a Sector boundary.
		struct RowVertices
		{
			uint32_t layerIndex{ 0 };
			uint32_t y{ 0 };

			// Segment each cell of the row belongs to.
			std::vector<uint32_t> segmentOfCell;

			// Sector reported when the matching Segment is flushed.
			std::vector<uint32_t> flushSector;

			// Vertices and the ordered Sectors collected per Segment. A Sector with
			// no authored topology still needs a synthetic Vertex when an open wall
			// merges it into a run, so inferred routes can start there.
			std::vector<std::vector<RowVertex>> segments;
			std::vector<std::vector<uint32_t>> sectors;
		};

		// Every row of every Layer, indexed [layerIndex][deck].
		typedef std::vector<std::vector<RowVertices>> LayerRows;

	private:

		World* mwWorld;

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

		void processMarker(ObjectData const& obj, RowVertices& row);

		void processDoor(ObjectData const& obj, LayerPairRole role, PositionVertexMap& interLayerVertexLookup, RowVertices& row, CrossDeckVertexMap& crossDeckVertices);

		void processWindow(ObjectData const& obj, PositionVertexMap& interLayerVertexLookup, RowVertices& row);

		void processInteractionPoint(ObjectData const& obj, RowVertices& row);

		void processBulkheadDoor(ObjectData const& obj, RowVertices& row);

		void processWalkway(ObjectData const& obj, RowVertices& row);

		void processForceBridge(ObjectData const& obj, RowVertices& row);

		void processLadderObject(ObjectData const& obj, RowVertices& row, CrossDeckVertexMap& crossDeckVertices, int level);

		void processLiftObject(ObjectData const& obj, RowVertices& row, CrossDeckVertexMap& crossDeckVertices, uint32_t stopOffset);

		void processLadderTransit(LayerPairRole role, uint32_t curSectorIndex, uint32_t x, uint32_t y, int level, PositionVertexMap& interLayerVertexLookup, RowVertices& row, CrossDeckVertexMap& crossDeckVertices);

		void processStairwellTransit(LayerPairRole role, uint32_t curSectorIndex, uint32_t backSectorIndex, uint32_t x, uint32_t y, uint32_t deckOffset, PositionVertexMap& interLayerVertexLookup, RowVertices& row, CrossDeckVertexMap& crossDeckVertices);

		void processStaircaseTransit(LayerPairRole role, uint32_t curSectorIndex, uint32_t backSectorIndex,
			uint32_t x, uint32_t y, PositionVertexMap& interLayerVertexLookup, RowVertices& row,
			CrossDeckVertexMap& crossDeckVertices);

		std::shared_ptr<Vertex> createLiftTransitVertex(std::shared_ptr<LiftTransit> liftTransit, uint32_t x, uint32_t y, CrossDeckVertexMap& crossDeckVertices);

		std::shared_ptr<Vertex> createShuttleTransitVertex(std::shared_ptr<ShuttleTransit> shuttleTransit, uint32_t x, uint32_t y, CrossDeckVertexMap& crossDeckVertices);

		void addEdge(std::shared_ptr<Edge> edge, std::shared_ptr<Vertex> vertex0, std::shared_ptr<Vertex> vertex1, bool connectZ);

		void addCrossDeckVertex(std::shared_ptr<VerticalEdgeCreator> edgeCreator, std::shared_ptr<Vertex> vertex, CrossDeckVertexMap& crossDeckVertices);

		void processCrossDeckVertices(CrossDeckVertexMap const& crossDeckVertices);

		// A cell is scanned when it is occupied and has a floor to walk on.  The one
		// exception is the open Location cell beside the top of a Staircase transit
		// on the Layer directly behind, which is that flight's landing.
		bool cellIsProcessable(uint32_t layerIndex, uint32_t x, uint32_t y) const;

		// A wide SectorObject is scanned once, from its left-most cell.
		bool isLeftMostObjectCell(uint32_t layerIndex, uint32_t x, uint32_t y, CellDefinition const& cellDef) const;

		// Splits every Layer row into the Segments that are flushed independently.
		LayerRows buildLayerRows() const;

		void appendRowVertex(RowVertices& row, uint32_t x, uint32_t slot, std::shared_ptr<Vertex> vertex) const;

		void appendStandaloneRowVertex(RowVertices& row, uint32_t x, uint32_t slot, std::shared_ptr<Vertex> vertex) const;

		void flushRowVertices(RowVertices& row);

		// Scans one Layer row for the content that belongs to that Layer alone.
		void processLayerRow(uint32_t layerIndex, uint32_t y, RowVertices& row, CrossDeckVertexMap& crossDeckVertices);

		// A threshold records its SectorObject against the Sector that owns the cell.
		// Authoring a threshold on a Layer whose pair the World cannot yet express
		// leaves an index that belongs to a different Sector, so resolve it defensively
		// rather than trusting it.
		std::shared_ptr<SectorObject> resolveSectorObject(std::shared_ptr<Sector> sector, uint32_t index,
			char const* what, uint32_t x, uint32_t y);

		// The Layers a threshold actually crosses, read from the threshold itself rather
		// than from whichever Layer happens to hold a reference to it.  A shared
		// threshold SectorObject is visible from both of its Sectors, so a neighbouring
		// Layer pair can stumble across it; only the pair it was authored on may pair it.
		static bool thresholdBelongsToPair(std::shared_ptr<Sector> sector, uint32_t index,
			SectorObjectType type, uint32_t frontLayer, uint32_t backLayer);

		// Scans one adjacent Layer pair for the thresholds and Transits that join the
		// front Layer of the pair to the Layer directly behind it.
		void processLayerPair(uint32_t frontLayer, uint32_t backLayer, LayerRows& rows, CrossDeckVertexMap& crossDeckVertices);

		void processPairCell(uint32_t frontLayer, uint32_t backLayer, uint32_t x, uint32_t y,
			PositionVertexMap& interLayerVertexLookup, LayerRows& rows,
			CrossDeckVertexMap& crossDeckVertices);

	public:

		explicit Graph(World* world);

		virtual ~Graph() = default;

		std::vector<std::shared_ptr<const Vertex>> const& getVertices() const;

		std::vector<std::shared_ptr<const Edge>> const& getEdges() const;

		std::shared_ptr<const Vertex> const getVertexAtPosition(uint32_t layerIndex, float x, float y, float vertexRadius) const;

		std::shared_ptr<const Vertex> getClosestVertexInSector(Sector const* sector, Vector2 const& pos) const;

		std::shared_ptr<const Vertex> getVertexByIdentifier(uint32_t identifier) const;
		std::shared_ptr<const Vertex> getVertexForObject(std::shared_ptr<SectorObject> const& object) const;

		Log const& getBuildLog() const;

		std::shared_ptr<Path> calculatePath(Agent const* agent, std::shared_ptr<const Vertex> source, std::shared_ptr<const Vertex> target) const;

		std::shared_ptr<Path> calculatePath(Agent const* agent, std::shared_ptr<const Vertex> target) const;

		void build();

		void validate();
	};

} // core
