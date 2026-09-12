#pragma once

#include <memory>
#include <vector>


namespace core
{
	class Controller;

	class DependentPathObject
	{
	public:

		virtual ~DependentPathObject() = default;
		
		[[nodiscard]] virtual std::shared_ptr<Controller> getDependingController(uint32_t index) const = 0;
	};

} // core
