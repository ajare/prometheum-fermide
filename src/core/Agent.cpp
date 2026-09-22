#include "core/Agent.h"
#include "core/Building.h"
#include "core/Edge.h"
#include "core/Location.h"
#include "core/Path.h"
#include "core/AgentTagRegistry.h"
#include "core/Log.h"
#include "core/Exceptions.h"
#include "core/SerializationException.h"


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
		// The Agent group is written by its stable ID and never by name, so a
		// rename of the group leaves every assigned Agent's stored form
		// untouched (ADR 0006). An Agent with no group writes no field at all -
		// the same convention as an Agent with no path, which writes no `path`
		// map - so a missing field reads back as "no Agent group".
		if (mAgentGroup) serializer.writeUint64("group", mAgentGroup.value);
		if (!mAgentTags.empty())
		{
			serializer.beginArray("tags");
			// std::set iteration is ascending AgentTagId order, keeping document
			// diffs stable regardless of the order in which tags were assigned.
			for (auto const id : mAgentTags) serializer.writeUint64("", id.value);
			serializer.endArray();
		}
		// An activated Agent writes no `active` key at all - the same convention
		// as an Agent with no path writing no `path` map - so a document written
		// before activation existed reads back with every Agent activated (#118).
		if (!mActive) serializer.writeBool("active", false);
		serializer.endMap();
	}

	bool Agent::deserializeImpl(Serializer& serializer, SerializationWorkData&)
	{
		serializer.beginMap("agent");
		mName = serializer.readString("name");
		mFlags = serializer.readUint32("flags");
		// Absent means no Agent group. Whether an ID that is present actually
		// names a group this Building owns is the Building's call, made before
		// the Agent is taken in.
		mAgentGroup = AgentGroupId{ serializer.readUint64("group", true, 0) };
		set<AgentTagId> agentTags;
		if (serializer.hasField("tags"))
		{
			serializer.beginArray("tags");
			while (serializer.nextArrayItem())
			{
				auto const id = AgentTagId{ serializer.readUint64("") };
				if (!id)
					throw SerializationException("Serialized Agent tag ID cannot be zero");
				if (!agentTags.insert(id).second)
					throw SerializationException(format(
						"Serialized Agent tag IDs must be unique ({} appears twice)", id.value));
			}
			serializer.endArray();
		}
		mAgentTags = std::move(agentTags);
		// Absent means activated: the default for every newly created Agent and
		// for every Agent loaded from a document that predates activation (#118).
		mActive = serializer.readBool("active", true, true);
		serializer.endMap();

		mState = State::Idle;
		mPath = {};
		mPathStartPosition = {};
		mResetPosition = {};
		mResetPath.reset();
		mResetPathActive = false;
		mTraversalTask.reset();
		mQueuedTraversalTask.reset();
		mTraversalLocalGoal.reset();
		return true;
	}

	string const& Agent::getName() const
	{
		return mName;
	}

	EffectiveAgentColour Agent::getEffectiveColour() const
	{
		EffectiveAgentColour effective;
		if (!mBuilding || !mBuilding->hasAttachedAgentTagRegistry()) return effective;

		auto const& registry = mBuilding->getAgentTagRegistry();
		for (auto const tag : mAgentTags)
		{
			auto const* definition = registry->lookupAgentTag(tag);
			if (!definition) continue;
			auto const* colour = definition->getColour();
			if (!colour) continue;
			effective.value = colour->value;
			effective.sourceTag = tag;
			break;
		}
		return effective;
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

	void Agent::setActive(bool active)
	{
		if (active != mActive)
		{
			mActive = active;
			modify();
		}
	}

	void Agent::setPosition(SectorPosition pos, bool authored)
	{
		mPosition = pos;
		if (authored)
		{
			mResetPosition = pos;
			modify();
		}
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
		mResetPosition = mPosition;
		mResetPath = path;
		mResetPathActive = startPathing;
		assignPath(std::move(path), startPathing, true);
	}

	void Agent::assignPath(shared_ptr<Path> path, bool startPathing, bool markModified)
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
			if (markModified) modify();
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
						if (markModified) modify();
						return;
					}
				}
			}
		}

		mPathStartPosition = mPosition.sector() ? getGlobalPosition() : Vector2::ZERO;
		clearRuntimePath();
		mPath.path = std::move(path);
		mPath.targetNode = 0;
		if (markModified) modify();

		if (startPathing)
		{
			this->startPathing();
		}
	}

	void Agent::clearRuntimePath()
	{
		cancelTraversal();
		mEarlyDoorPressResource = {};
		mEarlyDoorPressAttempted = false;
		mEarlyQueueApproachDirectionX = 0;
		mPath.path = nullptr;
		mPath.targetNode = 0;
		mState = State::Idle;
	}

	void Agent::clearPath()
	{
		clearRuntimePath();
		mResetPosition = mPosition;
		mResetPath.reset();
		mResetPathActive = false;
		modify();
	}

	void Agent::startPathing()
	{
		if (!mPath.path || mPath.path->nodes.empty())
		{
			startIdling();
			return;
		}

		cancelTraversal();
		mEarlyQueueApproachDirectionX = 0;
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
		mEarlyQueueApproachDirectionX = 0;
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

		setPosition({ mPosition.sector(), mPosition.local() + moveAmt }, false);

		return reachedPos;
	}

	int Agent::chooseVertexOffset(int /* dim */, pair<float, uint32_t> const* offsets, uint32_t /* numOffsets */)
	{
		int chosen = 0;

		if (chosen < 0)
		{
			// Replan route?
			throw NotImplementedException("Agent::chooseVertexOffset() no offset chosen");
		}

		return (int)offsets[chosen].second;
	}

	uint32_t Agent::getSkippablePathTarget(uint32_t vertexA) const
	{
		if (!mPath.path || !getSector() || vertexA + 1 >= mPath.path->nodes.size()) return vertexA;
		auto const& nodeA = mPath.path->nodes[vertexA];
		if (!nodeA.targetVertex
			|| nodeA.targetVertex->getSubType() == VertexSubType::Interactable) return vertexA;
		auto const positionA = nodeA.targetVertex->getPosition();

		auto requiresActionAtSource = [&](shared_ptr<const Edge> const& edge)
		{
			if (!edge) return true;
			if (edge->getType() == EdgeType::Location) return false;
			if (edge->getType() == EdgeType::LiftMount && mBuilding)
			{
				auto resource = mBuilding->lookupTraversalResource(edge->getTraversalResourceId());
				// An open platform's mount edge is a topology-only exit handoff. Calls and
				// destination selection occur on its Lift edges instead.
				return !resource || !resource.entity->isOpenPlatformLift();
			}
			return true;
		};

		// Coincident nodes are topology-only. Look through them to obtain the next
		// physical vertex, but stop at an edge that requires an action at the current
		// waypoint (for example, operating a lift call control).
		auto vertexB = vertexA + 1;
		while (vertexB < mPath.path->nodes.size())
		{
			auto const& node = mPath.path->nodes[vertexB];
			if (!node.targetVertex
				|| node.targetVertex->getSubType() == VertexSubType::Interactable
				|| requiresActionAtSource(node.edge)) return vertexA;
			if (node.targetVertex->getPosition().distanceTo(positionA) > 0.001f) break;
			++vertexB;
		}
		if (vertexB >= mPath.path->nodes.size()) return vertexA;

		auto const& nodeB = mPath.path->nodes[vertexB];
		auto const positionB = nodeB.targetVertex->getPosition();
		auto const agentPosition = getGlobalPosition();
		auto const layer = getSector()->getLayerIndex();
		if (nodeA.targetVertex->getSector()->getLayerIndex() != layer
			|| nodeB.targetVertex->getSector()->getLayerIndex() != layer
			|| abs(positionA.y - positionB.y) > 0.001f
			|| abs(positionA.x - positionB.x) <= 0.001f) return vertexA;
		auto const sideA = positionA.x - agentPosition.x;
		auto const sideB = positionB.x - agentPosition.x;
		return sideA * sideB < -0.000001f ? vertexB : vertexA;
	}

	void Agent::moveToVertex(float frameTime)
	{
		if (!mPath.path || mPath.targetNode >= mPath.path->nodes.size())
		{
			startIdling();
			return;
		}

		mPath.targetNode = getSkippablePathTarget(mPath.targetNode);
		auto const& targetPos = mPath.path->nodes[mPath.targetNode].targetVertex->getPosition();
		if (mBuilding && mPath.targetNode + 1 < mPath.path->nodes.size()
			&& mBuilding->stopForAvailableQueuePosition(*this,
				mPath.path->nodes[mPath.targetNode + 1].edge, targetPos,
				getWalkSpeed() * frameTime))
		{
			mState = State::WaitingForTraversal;
			return;
		}
		// Ticket #98: a plain Door's request-creation gate is the crossing-width
		// band. An Agent whose next edge crosses a Door enters the traversal
		// flow as soon as it stands within the crossing width at the threshold
		// row, instead of converging on the exact vertex. The early stop above
		// keeps precedence for contended doors - it claims the queue spot on
		// the way, so the two never double-start the flow; request creation
		// itself still happens once, in the next intent-collection phase.
		if (mBuilding && mPath.targetNode + 1 < mPath.path->nodes.size()
			&& mBuilding->isAtDoorCrossingArrival(*this,
				mPath.path->nodes[mPath.targetNode + 1].edge, targetPos))
		{
			mState = State::WaitingForTraversal;
			return;
		}
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
		mEarlyQueueApproachDirectionX = 0;
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
			auto vertexA = mPath.targetNode + 1;
			auto resource = mBuilding->lookupTraversalResource(requestLookup.entity->getResource());
			if (resource && resource.entity->isOpenPlatformLift()
				&& requestLookup.entity->getEdgeType() == EdgeType::Lift)
			{
				// Adjacent Platform Lift edges form one transport journey. LOOK may pass
				// intermediate floors, so the traversal endpoint is the final contiguous
				// Lift vertex rather than the first graph segment.
				while (vertexA + 1 < mPath.path->nodes.size()
					&& mPath.path->nodes[vertexA + 1].edge
					&& mPath.path->nodes[vertexA + 1].edge->getType() == EdgeType::Lift)
					++vertexA;
				mTraversalTask->destinationVertex = mPath.path->nodes[vertexA].targetVertex;
				mTraversalTask->pathNodesConsumed = vertexA - mPath.targetNode;
			}
			auto const directTarget = getSkippablePathTarget(vertexA);
			if (directTarget != vertexA)
			{
				mTraversalTask->destinationVertex = mPath.path->nodes[directTarget].targetVertex;
				mTraversalTask->pathNodesConsumed = directTarget - mPath.targetNode;
			}
			// Inter-layer thresholds have coincident 2D endpoints. Keep the
			// locomotion task visible for a short deterministic crossing instead
			// of committing in the permit-allocation tick. The crossing runs in
			// place at the Agent's current position; the far-side Door vertex is
			// never a movement target.
			mTraversalTask->traversalTicksRemaining =
				requestLookup.entity->getEdgeType() == EdgeType::Door ? 6 : 0;
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

		auto const consumed = mTraversalTask->pathNodesConsumed;
		for (uint32_t i = 0; i < consumed && mPath.path; ++i) nextPathNode();
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
			assignPath(std::move(alternative), true, false);
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
			if (mQueuedTraversalTask)
			{
				mTraversalTask = std::move(mQueuedTraversalTask);
				mQueuedTraversalTask.reset();
				mState = State::WaitingForTraversal;
			}
			return;
		}
		considerTraversalReplan();
	}

	void Agent::cancelTraversal()
	{
		if (!mTraversalTask && !mQueuedTraversalTask) return;

		if (mBuilding)
		{
			if (mTraversalTask)
			{
				mBuilding->cancelTraversal(mTraversalTask->request, mTraversalTask->permit);
				mBuilding->releaseTraversal(mTraversalTask->request, mTraversalTask->permit);
			}
			if (mQueuedTraversalTask)
			{
				mBuilding->cancelTraversal(mQueuedTraversalTask->request, mQueuedTraversalTask->permit);
				mBuilding->releaseTraversal(mQueuedTraversalTask->request, mQueuedTraversalTask->permit);
			}
		}
		mTraversalTask.reset();
		mQueuedTraversalTask.reset();
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
			// A same-sector Location edge may lead directly to a queued threshold.
			// Stop and commit that unconstrained edge at the queue boundary so the
			// following Door/Ladder request is created before reaching its centre.
			if (mTraversalTask && mBuilding && mPath.path
				&& mTraversalTask->edge->getType() == EdgeType::Location
				&& mTraversalTask->destinationVertex->getSector().get() == getSector()
				&& mPath.targetNode + 2 < mPath.path->nodes.size()
				&& mBuilding->stopForAvailableQueuePosition(*this,
					mPath.path->nodes[mPath.targetNode + 2].edge,
					mTraversalTask->destinationVertex->getPosition(),
					getWalkSpeed() * frameTime))
			{
				auto const& sourceNode = mPath.path->nodes[mPath.targetNode + 1];
				auto const& destinationNode = mPath.path->nodes[mPath.targetNode + 2];
				TraversalTask queued;
				queued.edge = destinationNode.edge;
				queued.sourceVertex = sourceNode.targetVertex;
				queued.destinationVertex = destinationNode.targetVertex;
				queued.request = mBuilding->createTraversalRequest(*this, queued.edge,
					queued.sourceVertex, queued.destinationVertex);
				mEarlyQueueApproachDirectionX = 0;
				mQueuedTraversalTask = std::move(queued);
				mState = State::AwaitingTraversalCommit;
				break;
			}
			if (mTraversalTask && mTraversalTask->traversalTicksRemaining > 0)
			{
				--mTraversalTask->traversalTicksRemaining;
				break;
			}
			if (mTraversalTask)
			{
				// A Door crossing is a scripted layer change executed in place at
				// the Agent's current position. It never walks toward the far-side
				// Door vertex; the commit places the Agent where it already stands.
				if (mTraversalTask->edge->getType() == EdgeType::Door)
				{
					mState = State::AwaitingTraversalCommit;
					break;
				}
				float traversalSpeed = mTraversalTask->edge->getTraversalSpeed(this);
				if (traversalSpeed <= 0.0f)
					traversalSpeed = mTraversalTask->edge->getType() == EdgeType::Ladder
						? getClimbSpeed() : getWalkSpeed();
				if (moveToPosition(mTraversalTask->destinationVertex->getPosition(), frameTime,
					traversalSpeed))
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