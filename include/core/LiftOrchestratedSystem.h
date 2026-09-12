#pragma once

#include <memory>
#include <map>

#include "core/OrchestratedSystem.h"
#include "core/LiftTransit.h"
#include "core/Door.h"
#include "core/Button.h"


namespace core
{
	class Orchestrator;

	class LiftOrchestratedSystem : public OrchestratedSystem
	{
		struct Stop
		{
			Door* door;
			uint32_t index;
		};

		std::shared_ptr<LiftTransit> mLift;

		std::map<Button*, Stop> mStops;

	private:

		void onDoorOpened(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data);

		void onDoorClosed(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data);

		void onLiftArrivedAtDeck(Controllable const* lift, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data);

	public:

		explicit LiftOrchestratedSystem(std::shared_ptr<Orchestrator> orchestrator);

		~LiftOrchestratedSystem() = default;

		void setLift(std::shared_ptr<LiftTransit> lift);

		void addStop(std::shared_ptr<Door> door, std::shared_ptr<Button> button);

		ControllableActionStatus handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data) override;
	};

} // core
