#pragma once

#include <functional>

#include "core/Vertex.h"
#include "core/Shape.h"


namespace core
{
	class VertexController;
	class Controller;

	typedef std::function<void(Agent*)> AgentExitCallback;

	class VertexControllerArea
	{
	public:

		enum struct AgentRole
		{
			MoveToExit,
			UseController,
			MoveToIntermediate,
			WaitInQueue,
			WaitForExit,
			ReadyToExit
		};

		enum struct TargetOffsetType
		{
			Exit,
			Controller,
			Intermediate
		};

		struct TargetOffset
		{
			TargetOffsetType type;
			float vertexOffset;
			float size;
			Agent* agent{ nullptr };
		};

		struct RegisteredAgent
		{
			Agent* agent;
			AgentRole role;
			int targetOffsetIndex;
		};

	private:

		VertexController* mwOwner;

		Shape mControlArea, mControllerArea, mExitArea;

		std::shared_ptr<Vertex> mVertex;

		std::vector<RegisteredAgent> mRegisteredAgents;

		AgentExitCallback mOnAgentExitCallback;

	protected:

		std::vector<TargetOffset> mTargetOffsets;

		std::shared_ptr<Controller> mController;

		std::shared_ptr<Vertex> mControllerVertex;

	private:

		void updateRegisteredAgents(float frameTime);

		bool updateAgentDetails(RegisteredAgent* details);

		bool chooseNewAgentTarget(RegisteredAgent* details);

		bool getAgentRoleDetails(Agent const* agent, AgentRole* role, int* targetOffsetIndex);

		uint32_t getControllerTargetOffsetIndex() const;

		uint32_t getExitTargetOffsetIndex() const;

		uint32_t getFirstIntermediateTargetOffsetIndex() const;

		uint32_t getNumTargetOffsetIndices() const;

		uint32_t getNumIntermediateTargetOffsetIndices() const;

		uint32_t getFreeIntermediateTargetOffsetIndex(Agent const* agent) const;

		void moveAgentToTarget(RegisteredAgent* details, float frameTime);

		void checkOkToExitAgent(RegisteredAgent* details);

		void exitAgent(Agent* agent);

	public:

		VertexControllerArea(VertexController* owner, Shape const& controlArea);

		VertexControllerArea(VertexController* owner, Shape const& controlArea, Shape const& exitArea);

		VertexControllerArea(VertexController* owner, Shape const& controlArea, std::shared_ptr<Controller> controller, std::shared_ptr<Vertex> controllerVertex);

		VertexControllerArea(VertexController* owner, Shape const& controlArea, Shape const& exitArea, std::shared_ptr<Controller> controller, std::shared_ptr<Vertex> controllerVertex);

		virtual ~VertexControllerArea() = default;

		void setVertex(std::shared_ptr<Vertex> vertex);

		std::shared_ptr<Vertex> getVertex() const;

		bool hasExitArea() const;

		bool hasController() const;

		bool inControlArea(Vector2 const& pos) const;

		Shape const& getControlArea() const;

		Shape const& getControllerArea() const;

		Shape const& getExitArea() const;

		bool hasAgentReadyToExit() const;

		AgentRole getAgentRole(Agent const* agent) const;

		void registerAgentForControl(Agent* agent);

		void unregisterAgentFromControl(Agent* agent, bool failIfNotRegistered = true);

		void requestAgentToExit(AgentExitCallback callback);

		void update(float frameTime);
	};

} // core

