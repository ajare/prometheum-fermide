#include "core/Object.h"
#include "core/SensorSource.h"

namespace core
{
	Object::Object(float x, float y, float width, float height)
		: Shape(x, y, width, height)
	{
	}

	std::vector<std::pair<std::string, std::string>> Object::getInternalsStrings() const
	{
		return {};
	}

	void Object::update(float frameTime)
	{
		(void)frameTime;
	}

	bool Object::canSense(SensorType type) const
	{
		return mSensorSource && mSensorSource->canSense(type);
	}

	void Object::setSensorSource(std::shared_ptr<const SensorSource> sensor)
	{
		mSensorSource = std::move(sensor);
	}
}
