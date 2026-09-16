#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/SectorPosition.h"
#include "core/Shape.h"
#include "core/Path.h"
#include "core/EntityId.h"
#include "core/Serializable.h"


namespace core
{
	class Building;
	class Sector;

	struct PathIterator
	{
		std::shared_ptr<Path> path;
		uint32_t targetNode{ 0 };

		bool atEnd() const
		{
			return !path || targetNode >= (uint32_t)path->nodes.size();
		}
	};

	class Agent : public Serializable
	{
		friend class Building;
		friend class Sector;

	public:

		struct EdgeTraversalData
		{
			std::shared_ptr<Sector> curSector, nextSector;
			std::shared_ptr<const Vertex> nextVertex;
		};

		enum struct State
		{
			Idle,
			MovingToVertex,
			WaitingForTraversal,
			TraversingEdge,
			AwaitingTraversalCommit
		};

		struct TraversalTask
		{
			TraversalRequestId request;
			TraversalPermitId permit;
			std::shared_ptr<const Edge> edge;
			std::shared_ptr<const Vertex> sourceVertex;
			std::shared_ptr<const Vertex> destinationVertex;
			uint32_t pathNodesConsumed{ 1 };
			uint64_t traversalTicksRemaining{ 0 };
		};

	private:

		std::string mName;

		Building* mBuilding{ nullptr };

		SectorPosition mPosition;

		// Authored position and route are kept separate from transient locomotion.
		// Simulation advances mPosition/mPath, while serialization and Reset use
		// this immutable baseline.
		SectorPosition mResetPosition;
		std::shared_ptr<Path> mResetPath;
		bool mResetPathActive{ false };

		uint32_t mFlags;

		State mState;

		PathIterator mPath;
		// Position before the current route began. Traversal requests use this (or
		// the preceding path node) to retain the side from which the Agent approached.
		Vector2 mPathStartPosition;

		std::optional<TraversalTask> mTraversalTask;
		// A queue request may be made while the preceding same-sector Location
		// edge is still active, allowing the Agent to stop before the queue tail.
		std::optional<TraversalTask> mQueuedTraversalTask;

		// Traversal resources assign local goals; the Agent remains the sole owner
		// of walking and advances itself during the movement phase.
		std::optional<Vector2> mTraversalLocalGoal;

		// Prevent repeated opportunistic presses while following the same immediate
		// Door edge. This is transient locomotion state, not authored simulation data.
		TraversalResourceId mEarlyDoorPressResource;
		bool mEarlyDoorPressAttempted{ false };

		// Set when locomotion stops at the outer edge of an available queue lane.
		// +1 approaches from the left, -1 from the right, and 0 requests at the endpoint.
		int mEarlyQueueApproachDirectionX{ 0 };

	private:

		bool childrenModified() const override;

		void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const override;

		bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) override;

		void setPosition(SectorPosition pos, bool authored = true);

		void attachToBuilding(Building* building);

		void assignPath(std::shared_ptr<Path> path, bool startPathing, bool markModified);

		void clearRuntimePath();

		bool moveToPosition(Vector2 const& pos, float frameTime, float speed);

		uint32_t getSkippablePathTarget(uint32_t vertexA) const;

		bool moveToPosition(Vector2 const& pos, float frameTime)
		{
			return moveToPosition(pos, frameTime, getWalkSpeed());
		}

		void startIdling();

		bool nextPathNode();

		void moveToVertex(float frameTime);

		void collectTraversalIntent();

		void allocateTraversal();

		void commitTraversal();

		void cleanupTraversal();

		void considerTraversalReplan();

		float estimateRemainingPathSeconds(std::shared_ptr<Path> const& path, uint32_t fromNode) const;

		void cancelTraversal();

		bool moveToVertexOffset(int dim, float offset, float frameTime);

		// Pathing helpers
		PathNode& getTargetPathNode() const;

		bool atEndOfPath() const;


	public:

		Agent(std::string const& name);

		virtual ~Agent() = default;

		std::string const& getName() const;

		State getState() const;

		std::string getDescription() const;

		Sector const* getSector() const;

		Vector2 const& getLocalPosition() const;

		Vector2 getGlobalPosition() const;

		float getWidth() const;

		float getHeight() const;

		Shape getBounds() const;

		float getWalkSpeed() const;

		float getClimbSpeed() const;

		// Used by edge route-cost implementations; this is an observation only.
		float estimateTraversalDelay(TraversalResourceId resource, SectorId sourceSector) const;

		uint32_t getFlags() const;

		bool flagsSet(uint32_t flags) const;

		void setFlags(uint32_t flags);

		void unsetFlags(uint32_t flags);

		std::shared_ptr<Path> const& getPath() const;

		uint32_t getPathTargetNodeIndex() const;

		bool hasActiveLocomotionTask() const;

		TraversalRequestId getTraversalRequestId() const;

		TraversalPermitId getTraversalPermitId() const;

		EdgeTraversalData getEdgeTraversalData() const;

		void setPath(std::shared_ptr<Path> path, bool startPathing);

		void clearPath();

		void startPathing();

		void pausePathing();

		int chooseVertexOffset(int dim, std::pair<float, uint32_t> const* offsets, uint32_t numOffsets);


		void wake();

		void update(float frameTime);
	};

} // core
