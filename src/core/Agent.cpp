#include "core/Agent.h"
#include "core/Building.h"
#include "core/Edge.h"
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

	void Agent::attachToBuilding(Building* building)
	{
		if (mBuilding && mBuilding != building)
		{
			throw Exception("Agent is already attached to another Building");
		}
		mBuilding = building;
	}

	shared_ptr<Path> const& Agent::getPath() const
	{
		return mPath.path;
	}

	uint32_t Agent::getPathTargetNodeIndex() const
	{
		return mPath.targetNode;
	}

	bool Agent::hasActiveLocomotionTask() const
	{
		return mTraversalTask.has_value();
	}

	TraversalRequestId Agent::getTraversalRequestId() const
	{
		return mTraversalTask ? mTraversalTask->request : TraversalRequestId{};
	}

	TraversalPermitId Agent::getTraversalPermitId() const
	{
		return mTraversalTask ? mTraversalTask->permit : TraversalPermitId{};
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
		clearPath();
		mPath.path = std::move(path);
		mPath.targetNode = 0;

		if (startPathing)
		{
			this->startPathing();
		}
	}

	void Agent::clearPath()
	{
		cancelTraversal();
		mPath.path = nullptr;
		mPath.targetNode = 0;
		mState = State::Idle;
	}

	void Agent::startPathing()
	{
		if (!mPath.path || mPath.path->nodes.empty())
		{
			startIdling();
			return;
		}

		cancelTraversal();
		mState = State::MovingToVertex;

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Started pathing"));
	}

	void Agent::pausePathing()
	{
		cancelTraversal();
		mState = State::Idle;

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Paused pathing"));
	}

	bool Agent::nextPathNode()
	{
		++mPath.targetNode;

		if (!mPath.path || mPath.targetNode >= mPath.path->nodes.size() - 1)
		{
			mState = State::Idle;
			mPath.path = nullptr;
			mPath.targetNode = 0;
			addLogMessage(getDescription(), 0, LogLevel::Debug, format("Started idling"));
			return true;
		}

		mState = State::WaitingForTraversal;
		return false;
	}

	bool Agent::traversePathEdge(bool skipVertex)
	{
		// Legacy vertex controllers are not traversal authorities for an Agent
		// executing the replacement protocol. A sector transfer is legal only in
		// Building's commit phase while this Agent owns a live permit.
		CORE_VAR_UNUSED(skipVertex);
		return false;
	}

	void Agent::startIdling()
	{
		cancelTraversal();
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
		if (!mPath.path || mPath.targetNode >= mPath.path->nodes.size())
		{
			startIdling();
			return;
		}

		auto const& targetPos = mPath.path->nodes[mPath.targetNode].targetVertex->getPosition();
		if (!moveToPosition(targetPos, frameTime))
		{
			return;
		}

		if (mPath.targetNode + 1 >= mPath.path->nodes.size())
		{
			startIdling();
			return;
		}

		// The Agent has reached the source endpoint. Request creation is deferred
		// to the next intent-collection phase rather than changing membership here.
		mState = State::WaitingForTraversal;
	}

	void Agent::collectTraversalIntent()
	{
		if (mState != State::WaitingForTraversal || mTraversalTask || !mBuilding
			|| !mPath.path || mPath.targetNode + 1 >= mPath.path->nodes.size())
		{
			return;
		}

		auto const& sourceNode = mPath.path->nodes[mPath.targetNode];
		auto const& destinationNode = mPath.path->nodes[mPath.targetNode + 1];
		if (!destinationNode.edge || !sourceNode.targetVertex || !destinationNode.targetVertex)
		{
			return;
		}

		TraversalTask task;
		task.edge = destinationNode.edge;
		task.sourceVertex = sourceNode.targetVertex;
		task.destinationVertex = destinationNode.targetVertex;
		task.request = mBuilding->createTraversalRequest(*this, task.edge,
			task.sourceVertex, task.destinationVertex);
		mTraversalTask = std::move(task);
	}

	void Agent::allocateTraversal()
	{
		if (mState != State::WaitingForTraversal || !mTraversalTask || !mBuilding)
		{
			return;
		}

		auto requestLookup = mBuilding->lookupTraversalRequest(mTraversalTask->request);
		if (!requestLookup || requestLookup.entity->getState() != TraversalRequestState::Pending)
		{
			return;
		}

		mBuilding->allocateTraversalRequest(mTraversalTask->request,
			mTraversalTask->edge, mTraversalTask->destinationVertex);
		requestLookup = mBuilding->lookupTraversalRequest(mTraversalTask->request);
		if (requestLookup && requestLookup.entity->getState() == TraversalRequestState::Granted)
		{
			mTraversalTask->permit = requestLookup.entity->getPermit();
			// Inter-layer thresholds have coincident 2D endpoints. Keep the
			// locomotion task visible for a short deterministic crossing instead
			// of committing in the permit-allocation tick.
			mTraversalTask->traversalTicksRemaining =
				requestLookup.entity->getEdgeType() == EdgeType::Door
				&& getGlobalPosition().distanceTo(mTraversalTask->destinationVertex->getPosition()) < 0.001f ? 6 : 0;
			mState = State::TraversingEdge;
		}
	}

	void Agent::commitTraversal()
	{
		if (mState != State::AwaitingTraversalCommit || !mTraversalTask || !mBuilding)
		{
			return;
		}

		if (!mBuilding->commitTraversal(*this, mTraversalTask->request,
			mTraversalTask->permit, mTraversalTask->destinationVertex))
		{
			return;
		}

		nextPathNode();
	}

	void Agent::cleanupTraversal()
	{
		if (!mTraversalTask || !mBuilding)
		{
			return;
		}

		auto request = mBuilding->lookupTraversalRequest(mTraversalTask->request);
		if (request && (request.entity->getState() == TraversalRequestState::Committed
			|| request.entity->getState() == TraversalRequestState::Cancelled))
		{
			mBuilding->releaseTraversal(mTraversalTask->request, mTraversalTask->permit);
			mTraversalTask.reset();
			mTraversalLocalGoal.reset();
		}
	}

	void Agent::cancelTraversal()
	{
		if (!mTraversalTask)
		{
			return;
		}

		if (mBuilding)
		{
			mBuilding->cancelTraversal(mTraversalTask->request, mTraversalTask->permit);
			mBuilding->releaseTraversal(mTraversalTask->request, mTraversalTask->permit);
		}
		mTraversalTask.reset();
		mTraversalLocalGoal.reset();
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

		case State::TraversingEdge:
			if (mTraversalTask && mTraversalTask->traversalTicksRemaining > 0)
			{
				--mTraversalTask->traversalTicksRemaining;
				break;
			}
			if (mTraversalTask
				&& moveToPosition(mTraversalTask->destinationVertex->getPosition(), frameTime))
			{
				mState = State::AwaitingTraversalCommit;
			}
			break;

		case State::WaitingForTraversal:
			if (mTraversalLocalGoal)
			{
				moveToPosition(*mTraversalLocalGoal, frameTime);
			}
			break;

		case State::AwaitingTraversalCommit:
		case State::UnderVertexControl:
			break;

		default:
			throw UnhandledException(mState, "Agent::State");
		}
	}

} // core