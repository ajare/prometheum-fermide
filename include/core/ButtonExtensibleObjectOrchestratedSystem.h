#pragma once

#include <memory>
#include <vector>

#include "core/OrchestratedSystem.h"
#include "core/Button.h"
#include "core/ExtensibleObject.h"


namespace core
{
	class Orchestrator;

	class ButtonExtensibleObjectOrchestratedSystem : public OrchestratedSystem
	{
		std::shared_ptr<ExtensibleObject> mExtensibleObject;

		std::vector<Button*> mButtons;

	private:

		void onExtensibleObjectToggled(Controllable const* ExtensibleObject, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data);

	public:

		explicit ButtonExtensibleObjectOrchestratedSystem(std::shared_ptr<Orchestrator> orchestrator);

		~ButtonExtensibleObjectOrchestratedSystem() = default;

		void setExtensibleObject(std::shared_ptr<ExtensibleObject> ExtensibleObject);

		void addButton(std::shared_ptr<Button> button);

		ControllableActionStatus handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data) override;
	};

} // core
