#include "core/VertexControllerArea.h"
#include "core/VertexController.h"
#include "core/Log.h"
#include "core/Exceptions.h"


namespace core
{
	using namespace std;

	VertexControllerArea::VertexControllerArea(VertexController* owner, Shape const& controlArea)
		: VertexControllerArea(owner, controlArea, { 0, 0, -1, -1 })
	{
	}

	VertexControllerArea::VertexControllerArea(VertexController* owner, Shape const& controlArea, Shape const& exitArea)
		: VertexControllerArea(owner, controlArea, exitArea, nullptr, nullptr)
	{
	}

	VertexControllerArea::VertexControllerArea(VertexController* owner, Shape const& controlArea, shared_ptr<Controller> controller, shared_ptr<Vertex> controllerVertex)
		: VertexControllerArea(owner, controlArea, { 0, 0, -1, -1 }, controller, controllerVertex)
	{
	}

	VertexControllerArea::VertexControllerArea(VertexController* owner, Shape const& controlArea, Shape const& exitArea, shared_ptr<Controller> controller, shared_ptr<Vertex> controllerVertex)
		: mwOwner(owner)
		, mControlArea(controlArea)
		, mControllerArea(controllerVertex ? Shape(controllerVertex->getPosition().x - CORE_AGENT_REACH_DIST, controllerVertex->getPosition().y, CORE_AGENT_REACH_DIST * 2, CORE_AGENT_MAX_HEIGHT) : Shape(0, 0, -1, -1)), mExitArea(exitArea)
		, mController(controller)
		, mControllerVertex(controllerVertex)
	{
		if (controller)
		{
			mTargetOffsets.push_back({
				TargetOffsetType::Controller,
				controllerVertex->getPosition().x,
				CORE_AGENT_REACH_DIST * 2.0f,
				nullptr
			});
		}

		if (exitArea.getSize().x > 0.0f)
		{
			mTargetOffsets.push_back({
				TargetOffsetType::Exit,
				exitArea.getPosition().x,
				exitArea.getSize().x,
				nullptr
			});
		}
	}

	void VertexControllerArea::setVertex(shared_ptr<Vertex> vertex)
	{
		mVertex = vertex;

		// Fix special offsets
		auto cIndex = getControllerTargetOffsetIndex();

		if (cIndex != ~0u)
		{
			mTargetOffsets[cIndex].vertexOffset -= vertex->getPosition().x;
		}

		auto eIndex = getExitTargetOffsetIndex();

		if (eIndex != ~0u)
		{
			mTargetOffsets[eIndex].vertexOffset -= vertex->getPosition().x;
		}
	}

	shared_ptr<Vertex> VertexControllerArea::getVertex() const
	{
		return mVertex;
	}

	bool VertexControllerArea::hasExitArea() const
	{
		return mExitArea.getSize().x >= 0.0f;
	}

	bool VertexControllerArea::hasController() const
	{
		return mController != nullptr;
	}

	bool VertexControllerArea::inControlArea(Vector2 const& pos) const
	{
		return mControlArea.pointInShape(pos);
	}

	Shape const& VertexControllerArea::getControlArea() const
	{
		return mControlArea;
	}

	Shape const& VertexControllerArea::getControllerArea() const
	{
		return mControllerArea;
	}

	Shape const& VertexControllerArea::getExitArea() const
	{
		return mExitArea;
	}

	uint32_t VertexControllerArea::getControllerTargetOffsetIndex() const
	{
		if (!hasController())
		{
			return ~0u;
		}
		
		return 0;
	}

	uint32_t VertexControllerArea::getExitTargetOffsetIndex() const
	{
		if (!hasExitArea())
		{
			return ~0u;
		}

		return hasController() ? 1 : 0;
	}

	uint32_t VertexControllerArea::getFirstIntermediateTargetOffsetIndex() const
	{
		auto exitIndex = getExitTargetOffsetIndex();

		if (exitIndex == ~0u)
		{
			auto controllerIndex = getControllerTargetOffsetIndex();

			return controllerIndex == ~0u ? 0 : controllerIndex + 1;
		}
		else
		{
			return exitIndex + 1;
		}
	}

	uint32_t VertexControllerArea::getNumTargetOffsetIndices() const
	{
		return (uint32_t)mTargetOffsets.size();
	}

	uint32_t VertexControllerArea::getNumIntermediateTargetOffsetIndices() const
	{
		return  getNumTargetOffsetIndices() - getFirstIntermediateTargetOffsetIndex();
	}

	void VertexControllerArea::registerAgentForControl(Agent* agent)
	{
		AgentRole role;
		int targetOffsetIndex;

		if (getAgentRoleDetails(agent, &role, &targetOffsetIndex))
		{
			mRegisteredAgents.push_back({
				agent, 
				role,
				targetOffsetIndex
			});

			mTargetOffsets[targetOffsetIndex].agent = agent;

			agent->onRegisteredAgentForVertexControl();
		}
		else
		{
			throw NotImplementedException("Agent could not be assigned a role.  Need to replan");
		}
	}

	void VertexControllerArea::unregisterAgentFromControl(Agent* agent, bool failIfNotRegistered)
	{
		auto numAgents = (uint32_t)mRegisteredAgents.size();

		for (uint32_t i = 0; i < numAgents; ++i)
		{
			if (mRegisteredAgents[i].agent == agent)
			{
				agent->onUnregisteredAgentForVertexControl();

				for (uint32_t j = i; j < numAgents - 1; ++j)
				{
					mRegisteredAgents[j] = mRegisteredAgents[j + 1];
				}

				mRegisteredAgents.pop_back();
				return;
			}
		}

		if (failIfNotRegistered)
		{
			throw Exception(format("Could not find supposedly-registered agent {} in VertexControllerArea", agent->getDescription()));
		}
	}

	uint32_t VertexControllerArea::getFreeIntermediateTargetOffsetIndex(Agent const* agent) const
	{
		auto firstIntermediateIndex = getFirstIntermediateTargetOffsetIndex();
		auto numIntermediateSpots = getNumIntermediateTargetOffsetIndices();

		// Get the closest stop on each side, if there is one.  If stop 0 is free, just return that.
		if (mTargetOffsets[firstIntermediateIndex].agent == nullptr)
		{
			return firstIntermediateIndex;
		}

		// Formula for best position: minimum (distance to walk + distance to door)
		uint32_t bestIndex = ~0u;
		float bestTotalDistance = numeric_limits<float>::max();
		float agentPos = agent->getGlobalPosition().x;
		float vertexPos = mVertex->getPosition().x;

		for (uint32_t i = 0; i < numIntermediateSpots; ++i)
		{
			auto const& offset = mTargetOffsets[firstIntermediateIndex + i];

			if (offset.agent)
			{
				continue;
			}

			float totalDistance = fabs(agentPos - (vertexPos + offset.vertexOffset)) + fabs(offset.vertexOffset - vertexPos);

			if (totalDistance < bestTotalDistance)
			{
				bestTotalDistance = totalDistance;
				bestIndex = i;
			}
		}

		return bestIndex;
	}

	bool VertexControllerArea::hasAgentReadyToExit() const
	{
		// Ready Agents are at the first intermediate offset and in Idle state
		auto firstIndex = getFirstIntermediateTargetOffsetIndex();
		
		for (auto const& agentDetails : mRegisteredAgents)
		{
			auto const& [agent, role, targetOffsetIndex] = agentDetails;

			if ((role == AgentRole::WaitInQueue && targetOffsetIndex == firstIndex) || 
				role == AgentRole::MoveToExit || 
				role == AgentRole::WaitForExit ||
				role == AgentRole::ReadyToExit)
			{
				return true;
			}
		}

		return false;
	}

	bool VertexControllerArea::getAgentRoleDetails(Agent const* agent, AgentRole* role, int* targetOffsetIndex)
	{
		if (mController && mwOwner->controllerUseRequired())
		{
			// Find current user
			for (auto& agentDetails : mRegisteredAgents)
			{
				if (agentDetails.role == AgentRole::UseController)
				{
					// Is the new Agent closer to the Controller than the current one?
					auto controllerPos = mControllerVertex->getPosition();
					auto thisAgentDist = agent->getGlobalPosition().distanceToSq(controllerPos);
					auto curAgentDist = agentDetails.agent->getGlobalPosition().distanceToSq(controllerPos);

					if (thisAgentDist < curAgentDist)
					{
						*role = AgentRole::UseController;
						*targetOffsetIndex = (int)getControllerTargetOffsetIndex();
					}

					// This Agent should now look for a spot
					agentDetails.targetOffsetIndex = (int)getFreeIntermediateTargetOffsetIndex(agent);
					agentDetails.role = AgentRole::MoveToIntermediate;

					return true;
				}
			}

			// No Agent is heading to use the Controller, so let's do it ourself
			*role = AgentRole::UseController;
			*targetOffsetIndex = (int)getControllerTargetOffsetIndex();
			return true;
		}
		else
		{
			// Agent should look for a spot
			*targetOffsetIndex = (int)getFreeIntermediateTargetOffsetIndex(agent);
			*role = AgentRole::MoveToIntermediate;

			return *targetOffsetIndex >= 0;
		}
	}

	VertexControllerArea::AgentRole VertexControllerArea::getAgentRole(Agent const* agent) const
	{
		for (auto const& agentDetails : mRegisteredAgents)
		{
			if (agentDetails.agent == agent)
			{
				return agentDetails.role;
			}
		}

		throw Exception("Agent not registered in VertexControllerArea");
	}

	bool VertexControllerArea::updateAgentDetails(RegisteredAgent* details)
	{
		auto oldIndex = details->targetOffsetIndex;
		
		if (getAgentRoleDetails(details->agent, &details->role, &details->targetOffsetIndex))
		{
			mTargetOffsets[oldIndex].agent = nullptr;
			mTargetOffsets[details->targetOffsetIndex].agent = details->agent;
			return true;
		}
		else
		{
			return false;
		}
	}

	bool VertexControllerArea::chooseNewAgentTarget(RegisteredAgent* details)
	{
		float curVertexOffset = fabs(mTargetOffsets[details->targetOffsetIndex].vertexOffset);
		float curBestOffset = 0.0f;
		uint32_t curBestIndex = details->targetOffsetIndex;

		// Find the next free offset along.  Ie the offset with the same sign, which
		// is the largest free one less than current offset.
		for (uint32_t i = getFirstIntermediateTargetOffsetIndex(); i < getNumTargetOffsetIndices(); ++i)
		{
			if (i == details->targetOffsetIndex)
			{
				continue;
			}

			auto const& targetOffset = mTargetOffsets[i];
			auto tOffset = fabs(targetOffset.vertexOffset);

			if (curVertexOffset * targetOffset.vertexOffset >= 0 &&  // both same sign
				tOffset < curVertexOffset &&                         // offset is closer to Vertex than current
				tOffset > curBestOffset &&                           // offset is larger than current closest
				!targetOffset.agent)                                 // there's no Agent at this offset                       
			{
				curBestOffset = tOffset;
				curBestIndex = i;
			}
		}

		if (curBestIndex == details->targetOffsetIndex)
		{
			details->role = AgentRole::WaitInQueue;
			return false;
		}
		else
		{
			mTargetOffsets[details->targetOffsetIndex].agent = nullptr;

			details->role = AgentRole::MoveToIntermediate;
			details->targetOffsetIndex = curBestIndex;

			mTargetOffsets[details->targetOffsetIndex].agent = details->agent;
			return true;
		}
	}

	void VertexControllerArea::moveAgentToTarget(RegisteredAgent* details, float frameTime)
	{
		auto& [agent, role, targetOffsetIndex] = *details;
		auto const& targetOffset = mTargetOffsets[targetOffsetIndex];
		auto const& [type, vertexOffset, size, _] = targetOffset;

		// For now, ingore size and just move to the centre.
		Vector2 agentTargetPos = mVertex->getPosition();
		agentTargetPos.x += vertexOffset;

		if (agent->moveToPosition(agentTargetPos, frameTime))
		{
			// If we've reached the offset, we need to do something.
			switch (type)
			{
			case TargetOffsetType::Exit:
				role = AgentRole::WaitForExit;
				if (!mwOwner->canExit(details->agent))
				{
					mwOwner->handleVertexAction(agent, mVertex, VertexActionType::UnblockForExit);
				}
				break;

			case TargetOffsetType::Controller:
				mController->use(agent, {});
				updateAgentDetails(details);
				break;

			case TargetOffsetType::Intermediate:
				chooseNewAgentTarget(details);
				break;
			}
		}
	}

	void VertexControllerArea::checkOkToExitAgent(RegisteredAgent* details)
	{
		if (mwOwner->canExit(details->agent))
		{
			details->role = AgentRole::ReadyToExit;
		}
	}

	void VertexControllerArea::exitAgent(Agent* agent)
	{
		if (mOnAgentExitCallback)
		{
			mOnAgentExitCallback(agent);
			mOnAgentExitCallback = {};
			agent->traversePathEdge(true);

			auto exitIndex = getExitTargetOffsetIndex();
			mTargetOffsets[exitIndex].agent = nullptr;

			// Update queue spots for other Agents
			for (auto& agentDetails : mRegisteredAgents)
			{
				if (agentDetails.agent)
				{
					chooseNewAgentTarget(&agentDetails);
				}
			}
		}
	}

	void VertexControllerArea::updateRegisteredAgents(float frameTime)
	{
		set<Agent*> agentsToUnregister;

		for (auto& agentDetails : mRegisteredAgents)
		{
			auto& [agent, role, _] = agentDetails;

			switch (role)
			{
			case AgentRole::MoveToExit:
			case AgentRole::UseController:
			case AgentRole::MoveToIntermediate:
				moveAgentToTarget(&agentDetails, frameTime);
				break;

			case AgentRole::ReadyToExit:
				exitAgent(agent);
				agentsToUnregister.insert(agent);
				break;

			case AgentRole::WaitInQueue:
				break;

			case AgentRole::WaitForExit:
				checkOkToExitAgent(&agentDetails);
				break;
			}
		}

		for (auto agent : agentsToUnregister)
		{
			unregisterAgentFromControl(agent);
		}
	}

	void VertexControllerArea::requestAgentToExit(AgentExitCallback callback)
	{
		auto firstIndex = getFirstIntermediateTargetOffsetIndex();

		for (auto& agentDetails : mRegisteredAgents)
		{
			auto& [agent, role, targetOffsetIndex] = agentDetails;

			if (role == AgentRole::WaitInQueue && targetOffsetIndex == firstIndex)
			{
				mTargetOffsets[targetOffsetIndex].agent = nullptr;
				
				role = AgentRole::MoveToExit;
				targetOffsetIndex = getExitTargetOffsetIndex();
				mOnAgentExitCallback = callback;
				return;
			}
		}

		throw Exception("No Agent ready to exit from VertexControllerArea");
	}

	void VertexControllerArea::update(float frameTime)
	{
		updateRegisteredAgents(frameTime);
	}

} // core
