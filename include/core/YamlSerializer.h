#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <stack>
#include <string>

#include <yaml-cpp/yaml.h>

#include "core/SerializationException.h"
#include "core/Serializer.h"
#include "core/SerializerFactory.h"

namespace core
{
	class YamlSerializer : public Serializer
	{
		enum class ObjectType
		{
			Sequence,
			Map
		};

		bool mSerializing;
		std::string mSource;
		bool mSourceIsFile;
		YAML::Emitter mEmitter;
		YAML::Node mLoadedData;
		std::stack<ObjectType> mTypeStack;
		std::stack<YAML::Node> mNodeStack;
		std::stack<size_t> mSequenceIteratorStack;
		std::stack<std::string> mPath;

		YamlSerializer(bool serializing, std::string source, bool sourceIsFile);

		void requireSerializing(char const* operation) const;
		void requireDeserializing(char const* operation) const;
		std::string getPath(std::string const& leaf) const;
		YAML::Node readNode(std::string const& name) const;

		template<typename T>
		void write(std::string const& name, T const& value)
		{
			requireSerializing("write");
			if (mTypeStack.empty())
			{
				throw SerializationException("Cannot write a YAML value outside a map or array");
			}

			if (mTypeStack.top() == ObjectType::Sequence)
			{
				mEmitter << value;
			}
			else
			{
				mEmitter << YAML::Key << name << YAML::Value << value;
			}
			if (!mEmitter.good())
			{
				throw SerializationException(std::string("Could not write YAML: ") + mEmitter.GetLastError());
			}
		}

		template<typename T>
		T readScalar(std::string const& name, bool optional, T const& defaultValue, char const* typeName) const
		{
			try
			{
				return readNode(name).as<T>();
			}
			catch (std::exception const& exception)
			{
				if (optional)
				{
					return defaultValue;
				}
				throw SerializationException(std::format("Could not read {} at {}: {}",
					typeName, getPath(name), exception.what()));
			}
		}

	public:
		using Serializer::writeString;

		static std::unique_ptr<YamlSerializer> toFile(std::string const& filepath);
		static std::unique_ptr<YamlSerializer> toString();
		static std::unique_ptr<YamlSerializer> fromFile(std::string const& filepath);
		static std::unique_ptr<YamlSerializer> fromString(std::string const& text);

		std::string getSerializedString() const;
		bool hasField(std::string const& name) const override;
		bool fieldIsMap(std::string const& name) const override;

		void writeBool(std::string const& name, bool value) override;
		void writeUint8(std::string const& name, uint8_t value) override;
		void writeUint16(std::string const& name, uint16_t value) override;
		void writeUint32(std::string const& name, uint32_t value) override;
		void writeUint64(std::string const& name, uint64_t value) override;
		void writeInt8(std::string const& name, int8_t value) override;
		void writeInt16(std::string const& name, int16_t value) override;
		void writeInt32(std::string const& name, int32_t value) override;
		void writeInt64(std::string const& name, int64_t value) override;
		void writeFloat(std::string const& name, float value) override;
		void writeDouble(std::string const& name, double value) override;
		void writeString(std::string const& name, char const* text, size_t length) override;

		void beginMap(std::string const& name) override;
		void endMap() override;
		void beginArray(std::string const& name = "", bool blockNotFlow = true) override;
		void endArray() override;
		bool nextArrayItem() override;

		void serialize() override;
		void deserialize() override;

		bool readBool(std::string const& name = "", bool optional = false, bool defaultValue = false) override;
		uint8_t readUint8(std::string const& name = "", bool optional = false, uint8_t defaultValue = 0) override;
		uint16_t readUint16(std::string const& name = "", bool optional = false, uint16_t defaultValue = 0) override;
		uint32_t readUint32(std::string const& name = "", bool optional = false, uint32_t defaultValue = 0) override;
		uint64_t readUint64(std::string const& name = "", bool optional = false, uint64_t defaultValue = 0) override;
		int8_t readInt8(std::string const& name = "", bool optional = false, int8_t defaultValue = 0) override;
		int16_t readInt16(std::string const& name = "", bool optional = false, int16_t defaultValue = 0) override;
		int32_t readInt32(std::string const& name = "", bool optional = false, int32_t defaultValue = 0) override;
		int64_t readInt64(std::string const& name = "", bool optional = false, int64_t defaultValue = 0) override;
		float readFloat(std::string const& name = "", bool optional = false, float defaultValue = 0.0f) override;
		double readDouble(std::string const& name = "", bool optional = false, double defaultValue = 0.0) override;
		std::string readString(std::string const& name = "", bool optional = false,
			std::string const& defaultValue = "") override;
	};

	class YamlSerializerFactory : public SerializerFactory
	{
		bool mSerializing;
		bool mIsFile;

	public:
		YamlSerializerFactory(bool serializing, bool isFile)
			: mSerializing(serializing), mIsFile(isFile)
		{
		}

		std::unique_ptr<Serializer> create(std::string const& target) const override;
		std::string description() const override;
		std::string extension() const override;
	};
}
