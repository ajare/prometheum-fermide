#pragma once

#include <exception>
#include <string>
#include <format>

#if _MSC_VER >= 1930
#  include <source_location>
#endif


class ExitApplicationException : public std::exception
{
	int mExitCode;

public:

	ExitApplicationException(int exitCode, std::string message)
		: std::exception(message.c_str())
		, mExitCode(exitCode)
	{
	}

	int getExitCode() const
	{
		return mExitCode;
	}
};

class NotImplementedException : public std::exception
{
public:

#if _MSC_VER < 1930
	NotImplementedException()
		: std::exception("Not implemented yet.")
	{
	}
#else
	NotImplementedException(std::string const& message, std::source_location loc = std::source_location::current())
		: std::exception(std::format("Function {} at {}:{} is not implemented yet: {}", loc.function_name(), loc.file_name(), loc.line(), message).c_str())
	{
	}
#endif

	NotImplementedException(std::string const& function, std::string const& message)
		: std::exception(std::format("{} : {} is not implemented yet.", function, message).c_str())
	{
	}
};