#include "core/Object.h"
#include "core/SensorSource.h"


namespace core
{

	using namespace std;

	Object::Object(float x, float y, float width, float height)
		: DependentPathControllable()
		, Shape(x, y, width, height)
	{
	}

	vector<pair<string, string>> Object::getInternalsStrings() const
	{
		vector<pair<string, string>> res;

		if (!mActions.empty())
		{
			auto const& action = getCurrentAction();

			res.push_back({ "Action Type", getControllableActionTypeString(action.type) });
			res.push_back({ "Action Status", getControllableActionStatusString(action.status) });
		}

		return res;
	}

	bool Object::canSense(SensorType type) const
	{
		assert(mSensorSource.get() != nullptr && "No SensorSource set!");

		return mSensorSource->canSense(type);
	}

	void Object::setSensorSource(shared_ptr<const SensorSource> sensor)
	{
		mSensorSource = sensor;
	}

} // core