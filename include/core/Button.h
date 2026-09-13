#pragma once

#include <cstdint>
#include <string>

#include "core/Object.h"
#include "core/EntityId.h"

namespace core
{
	// Visual representation of a physical control. Its actionable behaviour is
	// represented by a building-owned InteractionPoint and typed bindings.
	class Button : public Object
	{
		std::string mName;
		float mEnableTimer{ -1.0f };
		bool mAutoReEnable{ false };
		bool mEnabled{ true };
		InteractionPointId mInteractionPoint;

	public:
		Button(std::string const& name, uint32_t cellX, uint32_t cellY,
			float xOffset, float yOffset, uint32_t flags = 0);

		std::string getDescription() const override;
		bool isEnabled() const { return mEnabled; }
		InteractionPointId getInteractionPointId() const { return mInteractionPoint; }
		void _setInteractionPointId(InteractionPointId id) { mInteractionPoint = id; }
		void enable();
		void disable();
		void update(float frameTime) override;
		void _setPlacement(float centerX, float baseY, float yAdjustment = 0.0f);
	};
}
