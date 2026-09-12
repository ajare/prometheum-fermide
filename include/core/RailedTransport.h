#pragma once

#include <cstdint>
#include <vector>

#include "core/Object.h"
#include "core/CellPosition.h"

namespace core
{
	class Door;

	class RailedTransport : public Object
	{
	protected:

		enum struct State
		{
			Idle,
			Moving,
			Arrived,
			WaitingOpenAndDisembark,
			WaitingEmbarkAndClose,
			WaitingDisembark,
			WaitingEmbark,
			Leaving
		};

	protected:

		State mState;

		std::vector<CellPosition> mStops;

		bool mLooping;

		float mSpeed;

		float mCurStop, mPrevStop;

		int mMoveDir;

		float mStopTimer;

	private:

		virtual ControllableActionStatus updateCallToStop(ControllableAction const& action, float frameTime);

		// Overridden from Useable
		ControllableActionStatus useImpl(Controller* controller, ControllableActionCallback callback) override;

		// Overridden from Controllable
		bool validateAction(ControllableActionType type) const override;

		// Overridden from Controllable
		void sortActionsByPriority(std::vector<ControllableAction>& actions) override;

		// Overridden from Controllable
		bool interruptAction(ControllableActionType newAction, ControllableActionType curAction) override;

		// Overridden from Controllable
		bool modifyAndReject(ControllableActionType type, ControllableActionData const& data) override;

		virtual uint32_t getStopRefIndex(uint32_t index) const = 0;

	protected:

		Vector2 getPosition(uint32_t* lowStopIndex = nullptr) const;

		void updatePosition();

		CellPosition const& getStop(uint32_t index) const;

		float getStopDistance(uint32_t index) const;

		virtual void arriveAtStop(uint32_t index);

		// Overriden from Controllable
		ControllableActionStatus startAction(ControllableAction const& action) override;

		// Overriden from Controllable
		ControllableActionStatus updateAction(ControllableAction const& action, float frameTime) override;

	public:

		RailedTransport(float xOffset, float yOffset, float transportWidth, float transportHeight, float speed, std::vector<CellPosition> const &stops, bool looping);

		~RailedTransport() = default;

		bool hasStop(uint32_t x, uint32_t y) const;

		uint32_t getNumStops() const;

		uint32_t getStopDeckIndex(uint32_t index) const;

		uint32_t getStopIndex(uint32_t x, uint32_t y) const;

		std::vector<uint32_t> getRequestedStops() const;

		// Overridden from Object
		std::vector<std::pair<std::string, std::string>> getInternalsStrings() const override;

		void onDoorOpened(Door const* door);

		void onDoorClosed(Door const* door);
	};

} // core
