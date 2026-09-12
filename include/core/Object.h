#pragma once

#include <vector>
#include <string>
#include <memory>

#include "core/DependentPathControllable.h"
#include "core/Shape.h"
#include "core/SensorType.h"


namespace core
{
	class SensorSource;

	// Intended as a base class for Interactables, Windows, etc.
	class Object : public DependentPathControllable, public Shape
	{
		std::shared_ptr<const SensorSource> mSensorSource;

	protected:

		bool canSense(SensorType type) const;

	public:

		Object(float x, float y, float width, float height);

		~Object() = default;

		virtual std::vector<std::pair<std::string, std::string>> getInternalsStrings() const;

		void setSensorSource(std::shared_ptr<const SensorSource> sensor);
	};

} // core
