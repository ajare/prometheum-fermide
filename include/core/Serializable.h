#pragma once

#include <string>
#include <vector>

#include "core/SerializationWorkData.h"
#include "core/Serializer.h"

namespace core
{
	class Serializable
	{
		std::vector<std::string> mDeserializationWarnings;
		std::vector<std::string> mDeserializationErrors;
		mutable bool mModified;

		virtual bool childrenModified() const = 0;

	protected:
		virtual void copyFrom(Serializable const& other);
		void swapState(Serializable& other) noexcept;

		virtual void serializeImpl(Serializer& serializer, SerializationWorkData& workData) const = 0;
		virtual bool deserializeImpl(Serializer& serializer, SerializationWorkData& workData) = 0;

		virtual void preSerialization(SerializationWorkData& workData) const;
		virtual void preDeserialization(SerializationWorkData& workData);
		virtual void postSerialization(SerializationWorkData& workData) const;
		virtual void postDeserialization(SerializationWorkData& workData);

		void copyErrorsAndWarnings(Serializable const* serializable, bool errors, bool warnings);
		void addDeserializationWarning(std::string const& message);
		void addDeserializationError(std::string const& message);
		void modify();

	public:
		Serializable();
		Serializable(Serializable const& other);
		virtual ~Serializable() = default;

		Serializable& operator=(Serializable const& other);

		std::vector<std::string> const& getDeserializationWarnings() const;
		std::vector<std::string> const& getDeserializationErrors() const;
		bool isModified() const;

		void serialize(Serializer& serializer, SerializationWorkData& workData) const;
		bool deserialize(Serializer& serializer, SerializationWorkData& workData);
	};
}
