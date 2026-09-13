#include "core/Agent.h"
#include "core/Building.h"
#include "core/Edge.h"
#include "core/Location.h"
#include "core/Path.h"
#include "core/Pathing.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	Agent::Agent(string const& name)
		: mName(name)
		, mFlags(0)
		, mState(State::Idle)
	{
	}

	bool Agent::childrenModified() const
	{
		return false;
	}

	void Agent::serializeImpl(Serializer& serializer, SerializationWorkData&) const
	{
		serializer.beginMap("agent");
		serializer.writeString("name", mName);
		serializer.writeUint32("flags", mFlags);
		serializer.endMap();
	}

	bool Agent::deserializeImpl(Serializer& serializer, SerializationWorkData&)
	{
		serializer.beginMap("agent");
		mName = serializer.readString("name");
		mFlags = serializer.readUint32("flags");
		serializer.endMap();

		mState = State::Idle;
		mPath = {};
		mTraversalTask.reset();
		mTraversalLocalGoal.reset();
		return true;
	}

	string const& Agent::getName() const
	{
		return mName;
	}

	Agent::State Agent::getState() const
	{
		return mState;
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

	float Agent::estimateTraversalDelay(TraversalResourceId resource, SectorId sourceSector) const
	{
		return mBuilding ? mBuilding->estimateTraversalDelay(resource, sourceSector) : 0.0f;
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
		auto const updated = mFlags | flags;
		if (updated != mFlags)
		{
			mFlags = updated;
			modify();
		}
	}

	void Agent::unsetFlags(uint32_t flags)
	{
		auto const updated = mFlags & ~flags;
		if (updated != mFlags)
		{
			mFlags = updated;
			modify();
		}
	}

	void Agent::setPosition(SectorPosition pos)
	{
		mPosition = pos;
		modify();
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

	void Agent::setPath(shared_ptr<Path> path, bool startPathing)
	{
		// An onboard replacement remains the same transport journey. Retarget the
		// live ride request and stop-request ownership instead of cancelling into a
		// needless exit/reboard cycle.
		uint32_t replacementSource = 0;
		if (startPathing && path && mTraversalTask && !mTraversalTask->permit && mBuilding
			&& mBuilding->replaceOnboardLiftDestination(*this, path, replacementSource))
		{
			mPath.path = std::move(path);
			mPath.targetNode = replacementSource;
			mTraversalTask->edge = mPath.path->nodes[replacementSource + 1].edge;
			mTraversalTask->sourceVertex = mPath.path->nodes[replacementSource].targetVertex;
			mTraversalTask->destinationVertex = mPath.path->nodes[replacementSource + 1].targetVertex;
			mTraversalTask->permit = {};
			mState = State::WaitingForTraversal;
			return;
		}

		// A granted permit freezes route intent until it is committed or expires.
		// Silently retaining the current path is safer than invalidating a crossing.
		if (mTraversalTask && mTraversalTask->permit)
		{
			return;
		}

		// Replacing only the suffix after the same immediate resource is a compatible
		// replan. Update the locomotion endpoints while retaining request/ticket age.
		if (startPathing && path && mTraversalTask && mBuilding)
		{
			auto request = mBuilding->lookupTraversalRequest(mTraversalTask->request);
			if (request && request.entity->getState() == TraversalRequestState::Pending)
			{
				for (uint32_t i = 0; i + 1 < path->nodes.size(); ++i)
				{
					auto const& source = path->nodes[i].targetVertex;
					auto const& destination = path->nodes[i + 1].targetVertex;
					auto const& edge = path->nodes[i + 1].edge;
					if (!source || !destination || !edge) continue;
					auto sourceSector = SectorId{ (uint64_t)source->getSector()->getIndex() + 1 };
					auto destinationSector = SectorId{ (uint64_t)destination->getSector()->getIndex() + 1 };
					if (sourceSector == request.entity->getSourceSector()
						&& destinationSector == request.entity->getDestinationSector()
						&& edge->getTraversalResourceId() == request.entity->getResource()
						&& edge->getType() == request.entity->getEdgeType())
					{
						mPath.path = std::move(path);
						mPath.targetNode = i;
						mTraversalTask->edge = edge;
						mTraversalTask->sourceVertex = source;
						mTraversalTask->destinationVertex = destination;
						mState = State::WaitingForTraversal;
						return;
					}
				}
			}
		}

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
		mEarlyDoorPressResource = {};
		mEarlyDoorPressAttempted = false;
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

	void Agent::startIdling()
	{
		cancelTraversal();
		mEarlyDoorPressResource = {};
		mEarlyDoorPressAttempted = false;
		mState = State::Idle;
		mPath.path = nullptr;
		mPath.targetNode = 0;

		addLogMessage(getDescription(), 0, LogLevel::Debug, format("Started idling"));
	}

	bool Agent::moveToPosition(Vector2 const& pos, float frameTime, float speed)
	{
		auto agentPos = getGlobalPosition();
		auto posDist = agentPos.distanceTo(pos);
		auto moveDist = speed * frameTime;
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
		if (!moveToPosition(targetPos, frameTime, getWalkSpeed()))
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
		if (!requestLookup) return;
		if (requestLookup.entity->getState() == TraversalRequestState::Pending)
		{
			mBuilding->allocateTraversalRequest(mTraversalTask->request,
				mTraversalTask->edge, mTraversalTask->destinationVertex);
			requestLookup = mBuilding->lookupTraversalRequest(mTraversalTask->request);
		}
		// A resource allocation initiated while processing another agent may have
		// granted this request earlier in the phase. The owner adopts that permit
		// on its next allocation turn rather than remaining a ghost lane owner.
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

	float Agent::estimateRemainingPathSeconds(shared_ptr<Path> const& path, uint32_t fromNode) const
	{
		if (!path) return 0.0f;
		float result = 0.0f;
		for (uint32_t i = fromNode + 1; i < path->nodes.size(); ++i)
		{
			auto const& node = path->nodes[i];
			if (node.edge) result += node.edge->getWeight(node.targetVertex, this, true);
		}
		return result;
	}

	void Agent::considerTraversalReplan()
	{
		if (!mTraversalTask || mTraversalTask->permit || !mBuilding || !mPath.path
			|| mPath.path->nodes.empty()) return;
		auto request = mBuilding->lookupTraversalRequest(mTraversalTask->request);
		if (!request || !request.entity->getQueueTicket()) return;
		auto const& policy = mBuilding->getTraversalWaitingPolicy();
		auto waited = mBuilding->getSimulationTick() - request.entity->getQueuedAtTick();
		if (waited < policy.minimumReplanWaitTicks
			|| (waited - policy.minimumReplanWaitTicks) % policy.replanIntervalTicks != 0) return;

		auto target = mPath.path->nodes.back().targetVertex;
		auto alternative = mBuilding->getGraph()->calculatePath(this, mTraversalTask->sourceVertex, target);
		if (!alternative || alternative->nodes.size() < 2) return;
		auto currentEta = estimateRemainingPathSeconds(mPath.path, mPath.targetNode);
		auto alternativeEta = estimateRemainingPathSeconds(alternative, 0);
		if (alternativeEta + policy.replanEtaMarginSeconds < currentEta)
		{
			setPath(std::move(alternative), true);
		}
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
			return;
		}
		considerTraversalReplan();
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

		return moveToPosition(targetPos, frameTime, getWalkSpeed());
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
				&& moveToPosition(mTraversalTask->destinationVertex->getPosition(), frameTime,
					mTraversalTask->edge->getType() == EdgeType::Ladder
						? getClimbSpeed() : getWalkSpeed()))
			{
				mState = State::AwaitingTraversalCommit;
			}
			break;

		case State::WaitingForTraversal:
			if (mTraversalLocalGoal)
			{
				moveToPosition(*mTraversalLocalGoal, frameTime, getWalkSpeed());
			}
			break;

		case State::AwaitingTraversalCommit:
			break;

		default:
			throw UnhandledException(mState, "Agent::State");
		}
	}

} // core