#pragma once

#include <format>
#include <source_location>
#include <stdexcept>
#include <string>

class ExitApplicationException : public std::runtime_error
{
	int mExitCode;

public:
	ExitApplicationException(int exitCode, std::string const& message)
		: std::runtime_error(message)
		, mExitCode(exitCode)
	{
	}

	int getExitCode() const
	{
		return mExitCode;
	}
};

class NotImplementedException : public std::runtime_error
{
public:
	NotImplementedException(std::string const& message = "Not implemented yet.",
		std::source_location loc = std::source_location::current())
		: std::runtime_error(std::format("Function {} at {}:{} is not implemented yet: {}",
			loc.function_name(), loc.file_name(), loc.line(), message))
	{
	}

	NotImplementedException(std::string const& function, std::string const& message)
		: std::runtime_error(std::format("{}: {} is not implemented yet.", function, message))
	{
	}
};
