#pragma once

#include <memory>
#include <vector>

#include "core/Controllable.h"
#include "core/DependentPathObject.h"


namespace core
{
	class DependentPathControllable : public Controllable, public DependentPathObject
	{
	public:

		DependentPathControllable();

		virtual ~DependentPathControllable() = default;

		[[nodiscard]] std::shared_ptr<Controller> getDependingController(uint32_t index) const override;
	};

} // core
