#include <iterator>

#include "core/Log.h"


namespace core
{
	using namespace std;

	static Log Logger;


	void addLogMessage(string const& source, uint32_t sourceId, LogLevel level, string const& msg)
	{
		Logger.push_back({
			source,
			sourceId,
			level,
			msg
		});

		if (Logger.size() > 2048)
		{
			Logger.pop_front();
		}
	}

	vector<LogMessage> consumeLogMessages()
	{
		vector<LogMessage> msgs;
		
		copy(Logger.begin(), Logger.end(), back_inserter(msgs));

		Logger.clear();

		return msgs;
	}

} // core