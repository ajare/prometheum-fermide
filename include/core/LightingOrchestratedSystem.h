#pragma once

#include <memory>
#include <vector>

#include "core/OrchestratedSystem.h"
#include "core/Button.h"
#include "core/Sector.h"


namespace core
{
	class Orchestrator;

	class LightingOrchestratedSystem : public OrchestratedSystem
	{
		std::shared_ptr<Sector> mSector;

		std::vector<Button*> mButtons;

	private:

		void onLightsToggled(Controllable const* Sector, ControllableActionType action, ControllableActionStatus status, ControllableActionData data);

	public:

		explicit LightingOrchestratedSystem(std::shared_ptr<Orchestrator> orchestrator);

		~LightingOrchestratedSystem() = default;

		void setSector(std::shared_ptr<Sector> Sector);

		void addButton(std::shared_ptr<Button> button);

		ControllableActionStatus handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data) override;
	};

} // core
