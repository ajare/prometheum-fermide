#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/Shape.h"
#include "core/SensorType.h"

namespace core
{
	class SensorSource;

	// Physical simulation/rendering object. Interaction and device-operation state
	// is owned by World registries, not by this geometry base class.
	class Object : public Shape
	{
		std::shared_ptr<const SensorSource> mSensorSource;

	protected:
		bool canSense(SensorType type) const;

	public:
		Object(float x, float y, float width, float height);
		virtual ~Object() = default;

		virtual std::string getDescription() const = 0;
		virtual std::vector<std::pair<std::string, std::string>> getInternalsStrings() const;
		virtual void update(float frameTime);

		void setSensorSource(std::shared_ptr<const SensorSource> sensor);
	};
}
