#pragma once

#include <exception>
#include <string>
#include <format>

#if _MSC_VER >= 1930
#  include <source_location>
#endif

#include "core/Building.h"


namespace core
{

	class Exception : public std::exception
	{
		std::string mMessage;

	public:

		explicit Exception(std::string const& message)
			: std::exception(message.c_str())
			, mMessage(message)
		{
		}

		std::string const& getMessage() const
		{
			return mMessage;
		}
	};


	class NotImplementedException : public Exception
	{
	public:

#if _MSC_VER < 1930
		NotImplementedException()
			: Exception("Not implemented yet.")
		{
		}
#else
		NotImplementedException(std::string const& message, std::source_location loc = std::source_location::current())
			: Exception(format("Function {} at {}:{} is not implemented yet: {}", loc.function_name(), loc.file_name(), loc.line(), message))
		{
		}
#endif

		NotImplementedException(std::string const& function, std::string const& message)
			: Exception(function + ": " + message + " is not implemented yet.")
		{
		}
	};


	template<typename T>
	class UnhandledException : public Exception
	{
	public:

#if _MSC_VER < 1930
		UnhandledException()
			: Exception("Unhandled case.")
		{
		}
#else
		UnhandledException(T value, std::string const& desc, std::source_location loc = std::source_location::current())
			: Exception(format("Value {} for {} at {}:{} was either invalid or otherwise unexpected.", (int)value, desc, loc.file_name(), loc.line()))
		{
		}
#endif
	};

	class BuildingException : public Exception
	{
		Building const* mwBuilding;

	public:

		BuildingException(Building const* building, std::string const& message)
			: Exception(std::format("{}: {}", building->getName(), message))
			, mwBuilding(building)
		{
		}
	};

	class SectorException : public Exception
	{
		Sector const* mwSector;

	public:

		SectorException(Sector const* sector, std::string const& message)
			: Exception(std::format("{}: {}", sector->getName(), message))
			, mwSector(sector)
		{
		}
	};

	class GraphException : public Exception
	{
	public:

		GraphException(std::string const& message)
			: Exception(message)
		{
		}
	};

} // core