#pragma once

#include <cstdint>
#include <deque>
#include <vector>
#include <string>


namespace core
{
	enum struct LogLevel
	{
		Debug,
		Info,
		Warning,
		Error
	};

	struct LogMessage
	{
		std::string source;
		uint32_t sourceId;
		LogLevel level;
		std::string msg;
	};

	typedef std::deque<LogMessage> Log;

	void addLogMessage(std::string const& source, uint32_t id, LogLevel level, std::string const& msg);

	std::vector<LogMessage> consumeLogMessages();

} // core
