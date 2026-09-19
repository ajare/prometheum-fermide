#include "core/YamlSerializer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace core
{
	namespace
	{
		constexpr size_t NoSequenceItem = std::numeric_limits<size_t>::max();

		// Regression-test seam (#62): when non-zero, the file write performed by
		// serialize() fails once this many bytes have been written, simulating a
		// late write failure such as a full disk or an exhausted quota.
		size_t gWriteFailureAfterBytes = 0;

		void removeTempFile(std::filesystem::path const& tempPath)
		{
			std::error_code ignored;
			std::filesystem::remove(tempPath, ignored);
		}
	}

	YamlSerializer::YamlSerializer(bool serializing, std::string source, bool sourceIsFile)
		: mSerializing(serializing), mSource(std::move(source)), mSourceIsFile(sourceIsFile)
	{
	}

	std::unique_ptr<YamlSerializer> YamlSerializer::toFile(std::string const& filepath)
	{
		return std::unique_ptr<YamlSerializer>(new YamlSerializer(true, filepath, true));
	}

	std::unique_ptr<YamlSerializer> YamlSerializer::toString()
	{
		return std::unique_ptr<YamlSerializer>(new YamlSerializer(true, "", false));
	}

	std::unique_ptr<YamlSerializer> YamlSerializer::fromFile(std::string const& filepath)
	{
		return std::unique_ptr<YamlSerializer>(new YamlSerializer(false, filepath, true));
	}

	std::unique_ptr<YamlSerializer> YamlSerializer::fromString(std::string const& text)
	{
		return std::unique_ptr<YamlSerializer>(new YamlSerializer(false, text, false));
	}

	void YamlSerializer::requireSerializing(char const* operation) const
	{
		if (!mSerializing)
		{
			throw SerializationException(std::format("Cannot {} with a YAML deserializer", operation));
		}
	}

	void YamlSerializer::requireDeserializing(char const* operation) const
	{
		if (mSerializing)
		{
			throw SerializationException(std::format("Cannot {} with a YAML serializer", operation));
		}
	}

	std::string YamlSerializer::getSerializedString() const
	{
		requireSerializing("get serialized output");
		return mEmitter.c_str();
	}

	std::string YamlSerializer::getPath(std::string const& leaf) const
	{
		std::string path = "/";
		auto parts = mPath;
		std::vector<std::string> ordered;
		while (!parts.empty())
		{
			if (!parts.top().empty())
			{
				ordered.push_back(parts.top());
			}
			parts.pop();
		}
		for (auto part = ordered.rbegin(); part != ordered.rend(); ++part)
		{
			path += *part + "/";
		}
		path += leaf;
		return path;
	}

	YAML::Node YamlSerializer::readNode(std::string const& name) const
	{
		requireDeserializing("read");
		YAML::Node node = mNodeStack.empty() ? mLoadedData : mNodeStack.top();
		if (!name.empty())
		{
			return node[name];
		}
		if (mSequenceIteratorStack.empty() || mSequenceIteratorStack.top() == NoSequenceItem
			|| mSequenceIteratorStack.top() >= node.size())
		{
			throw SerializationException(std::format("No current YAML array item at {}", getPath("")));
		}
		return node[mSequenceIteratorStack.top()];
	}

	bool YamlSerializer::hasField(std::string const& name) const
	{
		requireDeserializing("inspect fields");
		auto const node = mNodeStack.empty() ? mLoadedData : mNodeStack.top();
		return node.IsMap() && node[name].IsDefined();
	}

	bool YamlSerializer::fieldIsMap(std::string const& name) const
	{
		requireDeserializing("inspect fields");
		auto const node = mNodeStack.empty() ? mLoadedData : mNodeStack.top();
		return node.IsMap() && node[name].IsMap();
	}

	void YamlSerializer::writeBool(std::string const& name, bool value) { write(name, value); }
	void YamlSerializer::writeUint8(std::string const& name, uint8_t value) { write(name, value); }
	void YamlSerializer::writeUint16(std::string const& name, uint16_t value) { write(name, value); }
	void YamlSerializer::writeUint32(std::string const& name, uint32_t value) { write(name, value); }
	void YamlSerializer::writeUint64(std::string const& name, uint64_t value) { write(name, value); }
	void YamlSerializer::writeInt8(std::string const& name, int8_t value) { write(name, value); }
	void YamlSerializer::writeInt16(std::string const& name, int16_t value) { write(name, value); }
	void YamlSerializer::writeInt32(std::string const& name, int32_t value) { write(name, value); }
	void YamlSerializer::writeInt64(std::string const& name, int64_t value) { write(name, value); }
	void YamlSerializer::writeFloat(std::string const& name, float value) { write(name, value); }
	void YamlSerializer::writeDouble(std::string const& name, double value) { write(name, value); }

	void YamlSerializer::writeString(std::string const& name, char const* text, size_t length)
	{
		write(name, std::string(text, length));
	}

	void YamlSerializer::beginMap(std::string const& name)
	{
		if (mSerializing)
		{
			if (!mTypeStack.empty() && mTypeStack.top() == ObjectType::Map)
			{
				mEmitter << YAML::Key << name << YAML::Value;
			}
			mEmitter << YAML::BeginMap;
			if (!mEmitter.good())
			{
				throw SerializationException(std::string("Could not begin YAML map: ") + mEmitter.GetLastError());
			}
			mTypeStack.push(ObjectType::Map);
			mPath.push(name);
			return;
		}

		YAML::Node node;
		if (mNodeStack.empty())
		{
			// A root map's name is descriptive only, matching the emitter.
			node = mLoadedData;
		}
		else
		{
			auto const parent = mNodeStack.top();
			if (parent.IsSequence())
			{
				if (mSequenceIteratorStack.empty() || mSequenceIteratorStack.top() == NoSequenceItem
					|| mSequenceIteratorStack.top() >= parent.size())
				{
					throw SerializationException(std::format("No current YAML array item at {}", getPath(name)));
				}
				node = parent[mSequenceIteratorStack.top()];
			}
			else
			{
				node = parent[name];
			}
		}
		if (!node.IsMap())
		{
			throw SerializationException(std::format("Expected a map at {}, found YAML node type {}",
				getPath(name), static_cast<uint32_t>(node.Type())));
		}
		mNodeStack.push(node);
		mPath.push(name);
	}

	void YamlSerializer::endMap()
	{
		if (mSerializing)
		{
			if (mTypeStack.empty() || mTypeStack.top() != ObjectType::Map)
			{
				throw SerializationException("Cannot end YAML map: the current container is not a map");
			}
			mEmitter << YAML::EndMap;
			mTypeStack.pop();
		}
		else
		{
			if (mNodeStack.empty() || !mNodeStack.top().IsMap())
			{
				throw SerializationException("Cannot end YAML map: the current container is not a map");
			}
			mNodeStack.pop();
		}
		if (mPath.empty())
		{
			throw SerializationException("Cannot end YAML map: path stack is empty");
		}
		mPath.pop();
	}

	void YamlSerializer::beginArray(std::string const& name, bool blockNotFlow)
	{
		if (mSerializing)
		{
			if (!mTypeStack.empty() && mTypeStack.top() == ObjectType::Map)
			{
				mEmitter << YAML::Key << name << YAML::Value;
			}
			mEmitter << (blockNotFlow ? YAML::Block : YAML::Flow) << YAML::BeginSeq;
			if (!mEmitter.good())
			{
				throw SerializationException(std::string("Could not begin YAML array: ") + mEmitter.GetLastError());
			}
			mTypeStack.push(ObjectType::Sequence);
			mPath.push(name);
			return;
		}

		YAML::Node node;
		if (mNodeStack.empty())
		{
			node = name.empty() ? mLoadedData : mLoadedData[name];
		}
		else
		{
			auto const parent = mNodeStack.top();
			node = name.empty() ? parent : parent[name];
		}
		mNodeStack.push(node);
		mSequenceIteratorStack.push(NoSequenceItem);
		mPath.push(name);
	}

	void YamlSerializer::endArray()
	{
		if (mSerializing)
		{
			if (mTypeStack.empty() || mTypeStack.top() != ObjectType::Sequence)
			{
				throw SerializationException("Cannot end YAML array: the current container is not an array");
			}
			mEmitter << YAML::EndSeq;
			mTypeStack.pop();
		}
		else
		{
			if (mNodeStack.empty() || mSequenceIteratorStack.empty())
			{
				throw SerializationException("Cannot end YAML array: no array is open");
			}
			mSequenceIteratorStack.pop();
			mNodeStack.pop();
		}
		if (mPath.empty())
		{
			throw SerializationException("Cannot end YAML array: path stack is empty");
		}
		mPath.pop();
	}

	bool YamlSerializer::nextArrayItem()
	{
		requireDeserializing("advance an array");
		if (mNodeStack.empty() || mSequenceIteratorStack.empty())
		{
			throw SerializationException("Cannot advance YAML array: no array is open");
		}
		auto const& node = mNodeStack.top();
		if (!node.IsSequence())
		{
			throw SerializationException(std::format("Expected a sequence at {}, found YAML node type {}",
				getPath(""), static_cast<uint32_t>(node.Type())));
		}
		auto& iterator = mSequenceIteratorStack.top();
		iterator = iterator == NoSequenceItem ? 0 : iterator + 1;
		return iterator < node.size();
	}

	void YamlSerializer::setWriteFailureAfterBytesForTesting(size_t bytes)
	{
		gWriteFailureAfterBytes = bytes;
	}

	void YamlSerializer::serialize()
	{
		requireSerializing("serialize");
		if (!mTypeStack.empty())
		{
			throw SerializationException("Cannot finish YAML serialization while a container is open");
		}
		if (!mEmitter.good())
		{
			throw SerializationException(std::string("Could not serialize YAML: ") + mEmitter.GetLastError());
		}
		if (!mSourceIsFile)
		{
			return;
		}

		// Install the save transactionally (#62): write a temporary file in the
		// destination directory, flush and close it with explicit error checks,
		// and only then atomically replace the destination. A late write failure
		// keeps the previous file intact and reports failure to the caller.
		std::string const content = mEmitter.c_str();

		// #92: renaming over a symbolic link would replace the link itself with
		// a regular file while leaving the link's target untouched. Resolve the
		// destination first so the replacement installs over the target,
		// matching how the pre-#62 std::ofstream save followed the link.
		std::error_code error;
		std::filesystem::path destination(mSource);
		std::filesystem::path const resolvedDestination
			= std::filesystem::weakly_canonical(destination, error);
		if (!error)
		{
			destination = resolvedDestination;
		}

		// The rename installs the temporary file's inode, so an existing
		// destination's permission mode would be replaced by the temporary
		// file's umask-derived mode. Capture the mode now and transfer it onto
		// the replacement before installing it (#92).
		auto const existingStatus = std::filesystem::status(destination, error);
		bool const preservePermissions = !error && std::filesystem::is_regular_file(existingStatus);

		std::filesystem::path directory = destination.parent_path();
		if (directory.empty())
		{
			directory = std::filesystem::path(".");
		}
		std::filesystem::path const tempPath
			= directory / (destination.filename().string() + ".saving.tmp");

		{
			std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
			if (!output)
			{
				throw SerializationException(std::format("Could not open temporary YAML file for writing: {}",
					tempPath.string()));
			}

			constexpr size_t chunkSize = 64 * 1024;
			size_t written = 0;
			while (written < content.size())
			{
				auto const chunk = std::min(chunkSize, content.size() - written);
				output.write(content.data() + written, static_cast<std::streamsize>(chunk));
				written += chunk;
				if (gWriteFailureAfterBytes != 0 && written >= gWriteFailureAfterBytes)
				{
					output.close();
					removeTempFile(tempPath);
					throw SerializationException(std::format(
						"Could not write YAML file: {} (write failed after {} bytes)", mSource, written));
				}
				if (!output.good())
				{
					output.close();
					removeTempFile(tempPath);
					throw SerializationException(std::format("Could not write YAML file: {}", mSource));
				}
			}

			// A buffered write can still fail here; observing flush and close is
			// the whole point of writing through an explicit stream.
			output.flush();
			if (!output.good())
			{
				output.close();
				removeTempFile(tempPath);
				throw SerializationException(std::format("Could not flush YAML file: {}", mSource));
			}
			output.close();
			if (!output.good())
			{
				removeTempFile(tempPath);
				throw SerializationException(std::format("Could not close YAML file: {}", mSource));
			}
		}

		if (preservePermissions)
		{
			std::filesystem::permissions(tempPath, existingStatus.permissions(),
				std::filesystem::perm_options::replace, error);
			if (error)
			{
				removeTempFile(tempPath);
				throw SerializationException(std::format(
					"Could not preserve the permissions of YAML file: {} ({})",
					mSource, error.message()));
			}
		}

		// std::filesystem::rename is atomic within a filesystem on Linux and
		// replaces an existing destination on Windows (MoveFileEx with
		// REPLACE_EXISTING), and the temp file shares the destination's
		// directory so no cross-filesystem copy is attempted.
		std::filesystem::rename(tempPath, destination, error);
		if (error)
		{
			removeTempFile(tempPath);
			throw SerializationException(std::format("Could not replace YAML file: {} ({})",
				mSource, error.message()));
		}
	}

	void YamlSerializer::deserialize()
	{
		requireDeserializing("deserialize");
		if (!mNodeStack.empty() || !mSequenceIteratorStack.empty())
		{
			throw SerializationException("Cannot reload YAML while a container is open");
		}
		try
		{
			mLoadedData = mSourceIsFile ? YAML::LoadFile(mSource) : YAML::Load(mSource);
		}
		catch (std::exception const& exception)
		{
			throw SerializationException(std::format("Could not load YAML{}: {}",
				mSourceIsFile ? std::format(" file {}", mSource) : std::string(), exception.what()));
		}
	}

	bool YamlSerializer::readBool(std::string const& name, bool optional, bool defaultValue)
	{
		try
		{
			auto const node = readNode(name);
			try
			{
				return node.as<bool>();
			}
			catch (std::exception const&)
			{
				// Version 1 files represented booleans as the integers 0 and 1.
				return node.as<int32_t>() != 0;
			}
		}
		catch (std::exception const& exception)
		{
			if (optional)
			{
				return defaultValue;
			}
			throw SerializationException(std::format("Could not read bool at {}: {}",
				getPath(name), exception.what()));
		}
	}

	uint8_t YamlSerializer::readUint8(std::string const& name, bool optional, uint8_t defaultValue)
	{
		return readScalar(name, optional, defaultValue, "uint8");
	}
	uint16_t YamlSerializer::readUint16(std::string const& name, bool optional, uint16_t defaultValue)
	{
		return readScalar(name, optional, defaultValue, "uint16");
	}
	uint32_t YamlSerializer::readUint32(std::string const& name, bool optional, uint32_t defaultValue)
	{
		return readScalar(name, optional, defaultValue, "uint32");
	}
	uint64_t YamlSerializer::readUint64(std::string const& name, bool optional, uint64_t defaultValue)
	{
		return readScalar(name, optional, defaultValue, "uint64");
	}
	int8_t YamlSerializer::readInt8(std::string const& name, bool optional, int8_t defaultValue)
	{
		return readScalar(name, optional, defaultValue, "int8");
	}
	int16_t YamlSerializer::readInt16(std::string const& name, bool optional, int16_t defaultValue)
	{
		return readScalar(name, optional, defaultValue, "int16");
	}
	int32_t YamlSerializer::readInt32(std::string const& name, bool optional, int32_t defaultValue)
	{
		return readScalar(name, optional, defaultValue, "int32");
	}
	int64_t YamlSerializer::readInt64(std::string const& name, bool optional, int64_t defaultValue)
	{
		return readScalar(name, optional, defaultValue, "int64");
	}
	float YamlSerializer::readFloat(std::string const& name, bool optional, float defaultValue)
	{
		return readScalar(name, optional, defaultValue, "float");
	}
	double YamlSerializer::readDouble(std::string const& name, bool optional, double defaultValue)
	{
		return readScalar(name, optional, defaultValue, "double");
	}
	std::string YamlSerializer::readString(std::string const& name, bool optional,
		std::string const& defaultValue)
	{
		return readScalar(name, optional, defaultValue, "string");
	}

	std::unique_ptr<Serializer> YamlSerializerFactory::create(std::string const& target) const
	{
		if (mSerializing)
		{
			return mIsFile ? YamlSerializer::toFile(target) : YamlSerializer::toString();
		}
		return mIsFile ? YamlSerializer::fromFile(target) : YamlSerializer::fromString(target);
	}

	std::string YamlSerializerFactory::description() const
	{
		return "YAML";
	}

	std::string YamlSerializerFactory::extension() const
	{
		return "yaml";
	}
}
