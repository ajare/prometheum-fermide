#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/Controller.h"
#include "core/SectorPosition.h"
#include "core/Shape.h"
#include "core/Path.h"
#include "core/VertexControllerNotificationType.h"
#include "core/EntityId.h"


namespace core
{
	class Building;
	class Sector;
	class VertexController;

	struct PathIterator
	{
		std::shared_ptr<Path> path;
		uint32_t targetNode{ 0 };

		bool atEnd() const
		{
			return !path || targetNode >= (uint32_t)path->nodes.size();
		}
	};

	class Agent : public Controller
	{
		friend class Building;
		friend class Sector;
		friend class VertexControllerArea;

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
			AwaitingTraversalCommit,
			UnderVertexControl
		};

		struct TraversalTask
		{
			TraversalRequestId request;
			TraversalPermitId permit;
			std::shared_ptr<const Edge> edge;
			std::shared_ptr<const Vertex> sourceVertex;
			std::shared_ptr<const Vertex> destinationVertex;
			uint64_t traversalTicksRemaining{ 0 };
		};

	private:

		std::string mName;

		Building* mBuilding{ nullptr };

		SectorPosition mPosition;

		uint32_t mFlags;

		State mState;

		PathIterator mPath;

		std::optional<TraversalTask> mTraversalTask;

	private:

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback);

		void setPosition(SectorPosition pos);

		void attachToBuilding(Building* building);

		bool moveToPosition(Vector2 const& pos, float frameTime);

		void startIdling();

		bool nextPathNode();

		bool traversePathEdge(bool skipVertex);

		void moveToVertex(float frameTime);

		void collectTraversalIntent();

		void allocateTraversal();

		void commitTraversal();

		void cleanupTraversal();

		void cancelTraversal();

		bool moveToVertexOffset(int dim, float offset, float frameTime);

		// Pathing helpers
		PathNode& getTargetPathNode() const;

		bool atEndOfPath() const;

		void checkMovedUnderVertexControl();

	public:

		Agent(std::string const& name);

		virtual ~Agent() = default;

		std::string const& getName() const;

		State getState() const;

		bool underVertexControl() const;

		// Overridden from Useable
		std::string getDescription() const override;

		Sector const* getSector() const;

		Vector2 const& getLocalPosition() const;

		Vector2 getGlobalPosition() const;

		float getWidth() const;

		float getHeight() const;

		Shape getBounds() const;

		float getWalkSpeed() const;

		float getClimbSpeed() const;

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

		virtual void onRegisteredAgentForVertexControl();

		virtual void onUnregisteredAgentForVertexControl();

		virtual void onVertexControllerNotification(VertexControllerNotificationType type);

		void wake();

		void update(float frameTime);
	};

} // core
