#pragma once

namespace core
{
	struct SerializationWorkData
	{
		// When true, successful serialization clears the object's modified state.
		bool markSerializedUnmodified{ true };
	};
}
