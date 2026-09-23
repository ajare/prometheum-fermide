#pragma once

#include <format>
#include <source_location>
#include <stdexcept>
#include <string>

#include "core/World.h"


namespace core
{

	class Exception : public std::runtime_error
	{
		std::string mMessage;

	public:

		explicit Exception(std::string const& message)
			: std::runtime_error(message)
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

		NotImplementedException(std::string const& message = "Not implemented yet.",
			std::source_location loc = std::source_location::current())
			: Exception(format("Function {} at {}:{} is not implemented yet: {}", loc.function_name(), loc.file_name(), loc.line(), message))
		{
		}

		NotImplementedException(std::string const& function, std::string const& message)
			: Exception(function + ": " + message + " is not implemented yet.")
		{
		}
	};


	template<typename T>
	class UnhandledException : public Exception
	{
	public:

		UnhandledException(T value, std::string const& desc,
			std::source_location loc = std::source_location::current())
			: Exception(format("Value {} for {} at {}:{} was either invalid or otherwise unexpected.", (int)value, desc, loc.file_name(), loc.line()))
		{
		}
	};

	class WorldException : public Exception
	{
		World const* mwWorld;

	public:

		WorldException(World const* world, std::string const& message)
			: Exception(std::format("{}: {}", world->getName(), message))
			, mwWorld(world)
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