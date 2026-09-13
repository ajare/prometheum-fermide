#include "core/Serializer.h"

namespace core
{
	void Serializer::writeBool(std::string const& name, bool value)
	{
		writeInt8(name, value ? 1 : 0);
	}

	void Serializer::writeString(std::string const& name, std::string const& value)
	{
		writeString(name, value.data(), value.size());
	}

	bool Serializer::readBool(std::string const& name, bool optional, bool defaultValue)
	{
		return readInt8(name, optional, defaultValue ? 1 : 0) != 0;
	}
}
