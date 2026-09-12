#include "core/SensorType.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	string getSensorTypeString(SensorType type)
	{
		switch (type)
		{
		case SensorType::AgentBlocking:
			return "Agent blocking";
		
		default:
			throw UnhandledException(type, "SensorType");
		}
	}

} // core