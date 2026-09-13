#pragma once

#include <stdexcept>
#include <string>

namespace core
{
	class SerializationException : public std::runtime_error
	{
	public:
		explicit SerializationException(std::string const& message)
			: std::runtime_error(message)
		{
		}
	};
}
