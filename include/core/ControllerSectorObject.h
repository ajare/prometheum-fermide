#pragma once

#include <cstdint>
#include <memory>

#include "core/SectorObject.h"
#include "core/Controller.h"


namespace core
{

	class ControllerSectorObject : public SectorObject
	{
	protected:

		std::shared_ptr<Controller> mController;

	public:

		ControllerSectorObject(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh, std::shared_ptr<const Sector> sector, std::shared_ptr<Object> object, uint32_t* vertexIdentifer = nullptr);

		~ControllerSectorObject() = default;

		std::shared_ptr<Controller> getController() const;
	};

} // core
