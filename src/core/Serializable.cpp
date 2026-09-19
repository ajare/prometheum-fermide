#include "core/Serializable.h"

#include <utility>

namespace core
{
	Serializable::Serializable()
		: mModified(true)
	{
	}

	Serializable::Serializable(Serializable const& other)
	{
		copyFrom(other);
	}

	Serializable& Serializable::operator=(Serializable const& other)
	{
		if (this != &other)
		{
			copyFrom(other);
		}
		return *this;
	}

	void Serializable::copyFrom(Serializable const& other)
	{
		mDeserializationWarnings = other.mDeserializationWarnings;
		mDeserializationErrors = other.mDeserializationErrors;
		mModified = other.mModified;
	}

	void Serializable::swapState(Serializable& other) noexcept
	{
		mDeserializationWarnings.swap(other.mDeserializationWarnings);
		mDeserializationErrors.swap(other.mDeserializationErrors);
		std::swap(mModified, other.mModified);
	}

	void Serializable::addDeserializationWarning(std::string const& message)
	{
		mDeserializationWarnings.push_back(message);
	}

	void Serializable::addDeserializationError(std::string const& message)
	{
		mDeserializationErrors.push_back(message);
	}

	std::vector<std::string> const& Serializable::getDeserializationWarnings() const
	{
		return mDeserializationWarnings;
	}

	std::vector<std::string> const& Serializable::getDeserializationErrors() const
	{
		return mDeserializationErrors;
	}

	void Serializable::modify()
	{
		mModified = true;
	}

	bool Serializable::isModified() const
	{
		return childrenModified() || mModified;
	}

	void Serializable::markModified()
	{
		modify();
	}

	void Serializable::markUnmodified()
	{
		mModified = false;
	}

	void Serializable::copyErrorsAndWarnings(Serializable const* serializable, bool errors, bool warnings)
	{
		if (!serializable)
		{
			return;
		}
		if (warnings)
		{
			for (auto const& warning : serializable->getDeserializationWarnings())
			{
				addDeserializationWarning(warning);
			}
		}
		if (errors)
		{
			for (auto const& error : serializable->getDeserializationErrors())
			{
				addDeserializationError(error);
			}
		}
	}

	void Serializable::preSerialization(SerializationWorkData&) const
	{
	}

	void Serializable::preDeserialization(SerializationWorkData&)
	{
	}

	void Serializable::postSerialization(SerializationWorkData&) const
	{
	}

	void Serializable::postDeserialization(SerializationWorkData&)
	{
	}

	void Serializable::serialize(Serializer& serializer, SerializationWorkData& workData) const
	{
		preSerialization(workData);
		serializeImpl(serializer, workData);
		postSerialization(workData);
		if (workData.markSerializedUnmodified)
		{
			mModified = false;
		}
	}

	bool Serializable::deserialize(Serializer& serializer, SerializationWorkData& workData)
	{
		preDeserialization(workData);
		mDeserializationWarnings.clear();
		mDeserializationErrors.clear();
		bool const deserialized = deserializeImpl(serializer, workData);
		postDeserialization(workData);
		mModified = false;
		return deserialized;
	}
}
