#pragma once

#include <string>
#include <cstdint>
#include <memory>

#include "core/Controller.h"
#include "core/SectorPosition.h"
#include "core/Shape.h"
#include "core/Path.h"
#include "core/VertexControllerNotificationType.h"


namespace core
{
	class Sector;
	class VertexController;

	struct PathIterator
	{
		std::shared_ptr<Path> path;
		uint32_t targetNode{ 0 };

		bool atEnd() const
		{
			return targetNode == (uint32_t)path->nodes.size();
		}
	};

	class Agent : public Controller
	{
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
			UnderVertexControl
		};

	private:

		std::string mName;

		SectorPosition mPosition;

		uint32_t mFlags;

		State mState;

		PathIterator mPath;

	private:

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback);

		void setPosition(SectorPosition pos);

		bool moveToPosition(Vector2 const& pos, float frameTime);

		void startIdling();

		bool nextPathNode();

		bool traversePathEdge(bool skipVertex);

		void moveToVertex(float frameTime);

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
