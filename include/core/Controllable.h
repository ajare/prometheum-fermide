#pragma once

#include <memory>
#include <vector>
#include <string>
#include <array>

#include "core/Useable.h"
#include "core/ControllableAction.h"
#include "core/ControllableActionType.h"
#include "core/ControllableActionStatus.h"
#include "core/ControllableActionCallback.h"
#include "core/ControllableActionData.h"


namespace core
{
	class Controller;
	class Controllable;

	// Virtual because objects such as Button are both Controllable and Controller;
	// they must share one Useable state rather than contain two competing copies.
	class Controllable : public virtual Useable
	{
		static uint32_t IdGenerator;

		static uint32_t ActionIdGenerator;

	private:

		uint32_t mControllableId;

		std::vector<std::shared_ptr<Controller>> mControllers;

	protected:

		std::vector<ControllableAction> mActions;

		std::array<ControllableActionCallback, (int)ControllableActionType::COUNT> mTypeCallbacks;

	private:

		virtual void sortActionsByPriority(std::vector<ControllableAction>& actions);

		virtual void insertNewAction(ControllableAction const& action);

		void popCurrentAction();

		virtual void updateImpl(float frameTime, ControllableActionType action, ControllableActionStatus status);

		void finaliseCurrentAction();

	protected:

		ControllableAction& getCurrentAction();

		ControllableAction const& getCurrentAction() const;

		ControllableAction const& getAction(uint32_t id) const;

		ControllableAction& getAction(uint32_t id);

		void processCurrentAction(float frameTime);

		virtual bool validateAction(ControllableActionType type) const;
		
		virtual ControllableActionStatus startAction(ControllableAction const& action);

		virtual void finishAction(ControllableAction const& action);

		virtual ControllableActionStatus updateAction(ControllableAction const& action, float frameTime);

		virtual bool interruptAction(ControllableActionType newType, ControllableActionType curType);

		virtual bool modifyAndReject(ControllableActionType type, ControllableActionData const& data);

	public:

		Controllable();

		virtual ~Controllable() = default;

		uint32_t getId() const;

		uint32_t getNumControllers() const;

		std::shared_ptr<Controller> getController(uint32_t index) const;
		
		ControllableActionStatus getActionStatus(uint32_t id) const;

		void _addController(std::shared_ptr<Controller> controller);

		void _setTypeCallback(ControllableActionType type, ControllableActionCallback callback);

		virtual uint32_t handleAction(ControllableActionType type, bool autoTrigger = true, ControllableActionData data = {}, ControllableActionCallback callback = {});

		// Overridden from Useable
		bool canBeUsed(Controller const* controller) const override;

		virtual void update(float frameTime);
	};

} // core

