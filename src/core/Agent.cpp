#include "core/Agent.h"
#include "core/Location.h"
#include "core/Path.h"
#include "core/Pathing.h"
#include "core/VertexController.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	Agent::Agent(string const& name)
		: Controller()
		, mName(name)
		, mFlags(0)
		, mState(State::Idle)
	{
	}

	string const& Agent::getName() const
	{
		return mName;
	}

	Agent::State Agent::getState() const
	{
		return mState;
	}

	bool Agent::underVertexControl() const
	{
		return mState == State::UnderVertexControl;
	}

	string Agent::getDescription() const
	{
		return format("Agent: {}", getName());
	}

	Sector const* Agent::getSector() const
	{
		return mPosition.sector();
	}

	Vector2 const& Agent::getLocalPosition() const
	{
		return mPosition.local();
	}

	Vector2 Agent::getGlobalPosition() const
	{
		return mPosition.global();
	}

	float Agent::getWidth() const
	{
		return CORE_AGENT_MAX_WIDTH;
	}

	float Agent::getHeight() const
	{
		return CORE_AGENT_MAX_HEIGHT;
	}

	Shape Agent::getBounds() const
	{
		auto pos = getGlobalPosition();

		auto agentWidth2 = getWidth() * 0.5f;
		auto agentHeight = getHeight();

		auto pos0 = pos;
		pos0.x -= agentWidth2;

		auto pos1 = pos;
		pos1.x += agentWidth2;
		pos1.y += agentHeight;

		return { pos0, pos1 - pos0 };
	}

	float Agent::getWalkSpeed() const
	{
		return (float)CORE_AGENT_BASE_WALK_SPEED;
	}

	float Agent::getClimbSpeed() const
	{
		return (float)CORE_AGENT_BASE_CLIMB_SPEED;
	}

	uint32_t Agent::getFlags() const
	{
		return mFlags;
	}

	bool Agent::flagsSet(uint32_t flags) const
	{
		return (mFlags & flags) != 0;
	}

	void Agent::setFlags(uint32_t flags)
	{
		mFlags |= flags;
	}

	void Agent::unsetFlags(uint32_t flags)
	{
		mFlags &= ~flags;
	}

	void Agent::setPosition(SectorPosition pos)
	{
		mPosition = pos;
	}

	shared_ptr<Path> const& Agent::getPath() const
	{
		return mPath.path;
	}

	uint32_t Agent::getPathTargetNodeIndex() const
	{
		return mPath.targetNode;
	}

	Agent::EdgeTraversalData Agent::getEdgeTraversalData() const
	{
		return {
			mPath.path->nodes[mPath.targetNode].targetVertex->getSector(),
			mPath.path->nodes[mPath.targetNode + 1].targetVertex->getSector(),
			mPath.path->nodes[mPath.targetNode + 1].targetVertex
		};
	}

	PathNode& Agent::getTargetPathNode() const
	{
		return mPath.path->nodes[mPath.targetNode];
	}

	bool Agent::atEndOfPath() const
	{
		return mPath.atEnd();
	}

	void Agent::checkMovedUnderVertexControl()
	{
		auto thisVertex = getTargetPathNode().targetVertex;
		auto vertexController = thisVertex->getController();

		uint32_t controlAreaIndex;
		if (!vertexController || !vertexController->inControlArea(mPosition, &controlAreaIndex))
		{
			return;
		}

		// If we are moving past the Vertex, then we don't need/want to be under its control
		auto nextVertex = pathing::findNextVertexForVertexInPath(mPath.path, thisVertex.get(), mPath.targetNode);

		if (vertexController->transitionRequiresControl(thisVertex, nextVertex))
		{
			vertexController->registerAgentForControl(this, controlAreaIndex);
		}
	}

	void Agent::onRegisteredAgentForVertexControl()
	{
		mState = State::UnderVertexControl;
	}

	void Agent::onUnregisteredAgentForVertexControl()
	{
		mState = State::MovingToVertex;
	}

	void Agent::onVertexControllerNotification(VertexControllerNotificationType type)
	{
		// Do nothing
	}

	void Agent::setPath(shared_ptr<Path> path, bool startPathing)
	{
		mPath.path = path;
		mPath.targetNode = 0;

		if (startPathing)
		{
			this->startPathing();
		}
	}

	void Agent::clearPath()
	{
		mPath.path = nullptr;
		mPath.targetNode = 0;
	}

	void Agent::startPathing()
	{
		mState = State::MovingToVertex;
		mPath.targetNode = 0;

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Started pathing"));
	}

	void Agent::pausePathing()
	{
		mState = State::Idle;

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Paused pathing"));
	}

	bool Agent::nextPathNode()
	{
		mPath.targetNode++;

		if (atEndOfPath())
		{
			startIdling();
			return true;
		}
		else
		{
			return false;
		}
	}

	bool Agent::traversePathEdge(bool skipVertex)
	{
		auto nextVertex = mPath.path->nodes[mPath.targetNode + 1].targetVertex;

		auto curSector = mPath.path->nodes[mPath.targetNode].targetVertex->getSector();
		auto nextSector = nextVertex->getSector();

		curSector->exitAgent(this);

		Vector2 vertexOffset = getGlobalPosition() - nextVertex->getPosition();

		nextSector->enterAgent(this, nextVertex, vertexOffset);

		bool ended = nextPathNode();

		if (skipVertex && !ended)
		{
			return nextPathNode();
		}
		else
		{
			return ended;
		}
	}

	void Agent::startIdling()
	{
		mState = State::Idle;
		mPath.path = nullptr;
		mPath.targetNode = 0;

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Started idling"));
	}

	bool Agent::moveToPosition(Vector2 const& pos, float frameTime)
	{
		auto agentPos = getGlobalPosition();
		auto posDist = agentPos.distanceTo(pos);
		auto moveDist = getWalkSpeed() * frameTime;
		auto moveDelta = pos - agentPos;
		auto reachedPos = moveDist >= posDist;
		auto moveAmt = reachedPos ? moveDelta : moveDelta.normalisedCopy() * moveDist;

		setPosition({ mPosition.sector(), mPosition.local() + moveAmt });

		return reachedPos;
	}

	int Agent::chooseVertexOffset(int dim, pair<float, uint32_t> const* offsets, uint32_t numOffsets)
	{
		int chosen = 0;

		if (chosen < 0)
		{
			// Replan route?
			throw NotImplementedException("Agent::chooseVertexOffset() no offset chosen");
		}

		return (int)offsets[chosen].second;
	}

	void Agent::moveToVertex(float frameTime)
	{
		auto const& targetPos = mPath.path->nodes[mPath.targetNode].targetVertex->getPosition();

		if (moveToPosition(targetPos, frameTime))
		{
			if (nextPathNode())
			{
				return;
			}
		}

		checkMovedUnderVertexControl();
	}

	bool Agent::moveToVertexOffset(int dim, float offset, float frameTime)
	{
		ASSERT_DIM_OK(dim);

		auto targetPos = mPath.path->nodes[mPath.targetNode].targetVertex->getPosition();

		if (dim == CORE_DIM_X)
		{
			targetPos.x += offset;
		}
		else
		{
			targetPos.y += offset;
		}

		return moveToPosition(targetPos, frameTime);
	}

	ControllableActionStatus Agent::useImpl(Controller* controller, ControllableActionCallback callback)
	{
		return ControllableActionStatus::Rejected;
	}

	void Agent::wake()
	{
		if (mState != State::Idle)
		{
			return;
		}

		if (mPath.path)
		{
			startPathing();
		}
	}

	void Agent::update(float frameTime)
	{
		switch (mState)
		{
		case State::Idle:
			break;

		case State::MovingToVertex:
			moveToVertex(frameTime);
			break;

		case State::UnderVertexControl:
			break;

		default:
			throw UnhandledException(mState, "Agent::State");
		}
	}

} // core