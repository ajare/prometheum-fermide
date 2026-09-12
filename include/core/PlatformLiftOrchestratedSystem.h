#pragma once

#include <memory>
#include <map>

#include "core/OrchestratedSystem.h"
#include "core/Lift.h"
#include "core/Button.h"


namespace core
{
	class Orchestrator;

	class PlatformLiftOrchestratedSystem : public OrchestratedSystem
	{
		struct Stop
		{
			uint32_t index;
		};

		std::shared_ptr<Lift> mLift;

		std::map<Button*, Stop> mStops;

	private:

		void onLiftArrivedAtDeck(Controllable const* lift, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data);

	public:

		explicit PlatformLiftOrchestratedSystem(std::shared_ptr<Orchestrator> orchestrator);

		~PlatformLiftOrchestratedSystem() = default;

		void setLift(std::shared_ptr<Lift> lift);

		void addStop(std::shared_ptr<Button> button);

		ControllableActionStatus handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data) override;
	};

} // core
