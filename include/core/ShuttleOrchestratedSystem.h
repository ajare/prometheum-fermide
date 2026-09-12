#pragma once

#include <memory>
#include <map>

#include "core/OrchestratedSystem.h"
#include "core/ShuttleTransit.h"
#include "core/Door.h"
#include "core/Button.h"


namespace core
{
	class Orchestrator;

	class ShuttleOrchestratedSystem : public OrchestratedSystem
	{
		struct Stop
		{
			Door* door;
			uint32_t index;
		};

		std::shared_ptr<ShuttleTransit> mShuttle;

		std::map<Button*, Stop> mStops;

	private:

		void onDoorOpened(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data);

		void onDoorClosed(Controllable const* door, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data);

		void onShuttleArrivedAtStop(Controllable const* shuttle, ControllableActionType action, ControllableActionStatus status, ControllableActionData const& data);

	public:

		explicit ShuttleOrchestratedSystem(std::shared_ptr<Orchestrator> orchestrator);

		~ShuttleOrchestratedSystem() = default;

		void setShuttle(std::shared_ptr<ShuttleTransit> shuttle);

		void addStop(uint32_t stopIndex, std::vector<std::shared_ptr<Door>> const& doors, std::vector<std::shared_ptr<Button>> const& buttons);

		ControllableActionStatus handleAction(Controller* object, Controller* subject, ControllerActionType action, ControllableActionData data) override;
	};

} // core
