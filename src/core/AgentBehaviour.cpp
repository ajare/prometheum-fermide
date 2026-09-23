// Agent behaviour definition naming, schema, and source-module validation.

#include "core/AgentBehaviour.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace core
{
	using namespace std;

	namespace
	{
		bool valueMatchesType(AgentBehaviourConfigurationValue const& value,
			AgentBehaviourSchemaType type)
		{
			switch (type)
			{
			case AgentBehaviourSchemaType::Boolean:
				return agentBehaviourConfigurationGetIf<bool>(&value) != nullptr;
			case AgentBehaviourSchemaType::Integer:
				return agentBehaviourConfigurationGetIf<int64_t>(&value) != nullptr;
			case AgentBehaviourSchemaType::Number:
				return agentBehaviourConfigurationGetIf<double>(&value) != nullptr;
			case AgentBehaviourSchemaType::String:
				return agentBehaviourConfigurationGetIf<string>(&value) != nullptr;
			case AgentBehaviourSchemaType::Duration:
				return agentBehaviourConfigurationGetIf<AgentBehaviourDuration>(&value) != nullptr;
			case AgentBehaviourSchemaType::Marker:
				return agentBehaviourConfigurationGetIf<MarkerId>(&value) != nullptr;
			case AgentBehaviourSchemaType::List:
				return agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationList>(&value) != nullptr;
			case AgentBehaviourSchemaType::Record:
				return agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationRecord>(&value) != nullptr;
			}
			return false;
		}

		bool defaultMatchesSchema(AgentBehaviourConfigurationValue const& value,
			AgentBehaviourSchemaField const& field, string const& path, size_t depth,
			string* diagnostic)
		{
			auto reject = [diagnostic, &path](string reason)
			{
				if (diagnostic) *diagnostic = "Default for Agent behaviour schema field #"
					+ path + " " + std::move(reason);
				return false;
			};
			if (depth > MaxAgentBehaviourConfigurationDepth)
				return reject("exceeds the maximum configuration depth of 16");
			if (!valueMatchesType(value, field.type))
				return reject("has type " + string(agentBehaviourConfigurationValueTypeName(value))
					+ ", expected " + agentBehaviourSchemaTypeName(field.type));
			if (auto const* number = agentBehaviourConfigurationGetIf<double>(&value);
				number && !isfinite(*number))
				return reject("must be a finite Number");
			if (auto const* duration = agentBehaviourConfigurationGetIf<AgentBehaviourDuration>(&value);
				duration && duration->ticks == 0)
				return reject("Duration must be at least one tick");
			if (auto const* list = agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationList>(&value))
			{
				if (field.children.size() != 1)
					return reject("belongs to a List without exactly one element schema");
				if (list->size() > MaxAgentBehaviourListElements)
					return reject("contains more than 4096 List elements");
				for (size_t index = 0; index < list->size(); ++index)
					if (!defaultMatchesSchema((*list)[index], field.children.front(),
						path + "[" + to_string(index) + "]", depth + 1, diagnostic))
						return false;
			}
			if (auto const* record = agentBehaviourConfigurationGetIf<AgentBehaviourConfigurationRecord>(&value))
			{
				for (auto const& [name, nested] : *record)
				{
					auto child = find_if(field.children.begin(), field.children.end(),
						[&](auto const& candidate) { return candidate.name == name; });
					if (child == field.children.end())
						return reject("contains undeclared field '" + name + "'");
					if (!defaultMatchesSchema(nested, *child, path + "." + name,
						depth + 1, diagnostic)) return false;
				}
				for (auto const& child : field.children)
					if (!record->contains(child.name) && child.required)
						return reject("is missing required field '" + child.name + "'");
			}
			return true;
		}

		// Field names and behaviour names share the Marker naming rule set:
		// trimmed, non-empty, valid UTF-8, at most 63 bytes.
		bool utf8TextIsValid(std::string const& text, char const* what,
			std::string* diagnostic)
		{
			auto reject = [diagnostic, what](std::string reason)
			{
				if (diagnostic) *diagnostic = std::string(what) + " " + std::move(reason);
				return false;
			};
			auto const* bytes = reinterpret_cast<unsigned char const*>(text.data());
			size_t index = 0;
			while (index < text.size())
			{
				auto const lead = bytes[index];
				if (lead == 0) return reject("cannot contain a NUL byte");
				size_t continuation = 0;
				unsigned int minimum = 0;
				if (lead < 0x80) { ++index; continue; }
				if ((lead & 0xE0) == 0xC0) { continuation = 1; minimum = 0x80; }
				else if ((lead & 0xF0) == 0xE0) { continuation = 2; minimum = 0x800; }
				else if ((lead & 0xF8) == 0xF0) { continuation = 3; minimum = 0x10000; }
				else return reject("must be valid UTF-8");
				if (index + continuation >= text.size()) return reject("must be valid UTF-8");
				unsigned int codepoint = lead & (0xFF >> (continuation + 1));
				for (size_t i = 1; i <= continuation; ++i)
				{
					auto const next = bytes[index + i];
					if ((next & 0xC0) != 0x80) return reject("must be valid UTF-8");
					codepoint = (codepoint << 6) | (next & 0x3F);
				}
				if (codepoint < minimum || codepoint > 0x10FFFF
					|| (codepoint >= 0xD800 && codepoint <= 0xDFFF))
					return reject("must be valid UTF-8");
				index += continuation + 1;
			}
			return true;
		}

		bool schemaFieldsAreValidAtDepth(
			std::vector<AgentBehaviourSchemaField> const& fields, size_t depth,
			std::string* diagnostic)
		{
			auto reject = [diagnostic](std::string reason)
			{
				if (diagnostic) *diagnostic = std::move(reason);
				return false;
			};
			if (depth > MaxAgentBehaviourConfigurationDepth)
				return reject("Agent behaviour schemas cannot nest deeper than "
					+ std::to_string(MaxAgentBehaviourConfigurationDepth) + " levels");
			std::vector<std::string> names;
			for (auto const& field : fields)
			{
				if (std::string(agentBehaviourSchemaTypeName(field.type)) == "unknown")
					return reject("Unknown Agent behaviour schema type");
				if (!AgentBehaviour::nameIsValid(field.name, diagnostic))
					return false;
				if (std::find(names.begin(), names.end(), field.name) != names.end())
					return reject("Agent behaviour schema field #" + field.name
						+ " appears twice in one Record");
				names.push_back(field.name);
				if (field.required && field.defaultValue)
					return reject("Required Agent behaviour schema field #" + field.name
						+ " cannot declare a default");
				if (!field.required && !field.defaultValue)
					return reject("Optional Agent behaviour schema field #" + field.name
						+ " must declare a default");
				if (field.defaultValue && !defaultMatchesSchema(*field.defaultValue,
					field, field.name, depth, diagnostic)) return false;
				switch (field.type)
				{
				case AgentBehaviourSchemaType::List:
					if (field.children.size() != 1)
						return reject("Agent behaviour schema List #" + field.name
							+ " must declare exactly one element type");
					if (!schemaFieldsAreValidAtDepth(field.children, depth + 1, diagnostic))
						return false;
					break;
				case AgentBehaviourSchemaType::Record:
					if (field.children.empty())
						return reject("Agent behaviour schema Record #" + field.name
							+ " must declare at least one field");
					if (!schemaFieldsAreValidAtDepth(field.children, depth + 1, diagnostic))
						return false;
					break;
				default:
					if (!field.children.empty())
						return reject("Agent behaviour schema field #" + field.name
							+ " of a scalar type cannot declare nested fields");
					break;
				}
			}
			return true;
		}
	}

	char const* agentBehaviourConfigurationValueTypeName(
		AgentBehaviourConfigurationValue const& value)
	{
		return visit([](auto const& typed) -> char const*
		{
			using T = decay_t<decltype(typed)>;
			if constexpr (is_same_v<T, bool>) return "boolean";
			else if constexpr (is_same_v<T, int64_t>) return "integer";
			else if constexpr (is_same_v<T, double>) return "number";
			else if constexpr (is_same_v<T, string>) return "string";
			else if constexpr (is_same_v<T, AgentBehaviourDuration>) return "duration";
			else if constexpr (is_same_v<T, MarkerId>) return "marker";
			else if constexpr (is_same_v<T, AgentBehaviourConfigurationList>) return "list";
			else return "record";
		}, value.value);
	}

	char const* agentBehaviourModuleStatusName(AgentBehaviourModuleStatus status)
	{
		switch (status)
		{
		case AgentBehaviourModuleStatus::NotLoaded: return "Not loaded";
		case AgentBehaviourModuleStatus::Loaded: return "Loaded";
		case AgentBehaviourModuleStatus::Error: return "Error";
		}
		return "Unknown";
	}

	char const* agentBehaviourSchemaTypeName(AgentBehaviourSchemaType type)
	{
		switch (type)
		{
		case AgentBehaviourSchemaType::Boolean: return "boolean";
		case AgentBehaviourSchemaType::Integer: return "integer";
		case AgentBehaviourSchemaType::Number: return "number";
		case AgentBehaviourSchemaType::String: return "string";
		case AgentBehaviourSchemaType::Duration: return "duration";
		case AgentBehaviourSchemaType::Marker: return "marker";
		case AgentBehaviourSchemaType::List: return "list";
		case AgentBehaviourSchemaType::Record: return "record";
		}
		return "unknown";
	}

	bool agentBehaviourSchemaTypeFromName(std::string const& name,
		AgentBehaviourSchemaType& type)
	{
		if (name == "boolean") type = AgentBehaviourSchemaType::Boolean;
		else if (name == "integer") type = AgentBehaviourSchemaType::Integer;
		else if (name == "number") type = AgentBehaviourSchemaType::Number;
		else if (name == "string") type = AgentBehaviourSchemaType::String;
		else if (name == "duration") type = AgentBehaviourSchemaType::Duration;
		else if (name == "marker") type = AgentBehaviourSchemaType::Marker;
		else if (name == "list") type = AgentBehaviourSchemaType::List;
		else if (name == "record") type = AgentBehaviourSchemaType::Record;
		else return false;
		return true;
	}

	bool agentBehaviourSchemaFieldsAreValid(
		std::vector<AgentBehaviourSchemaField> const& fields, std::string* diagnostic)
	{
		if (!schemaFieldsAreValidAtDepth(fields, 1, diagnostic)) return false;
		if (diagnostic) diagnostic->clear();
		return true;
	}

	std::unique_ptr<AgentBehaviourHelperModule> AgentBehaviourHelperModule::create(
		std::string name, std::string sourceModulePath)
	{
		return std::unique_ptr<AgentBehaviourHelperModule>(
			new AgentBehaviourHelperModule(std::move(name), std::move(sourceModulePath)));
	}

	bool AgentBehaviourHelperModule::nameIsValid(std::string const& name,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (name.empty()) return reject("An Agent behaviour helper module name cannot be blank");
		if (name == "prometheum.v1")
			return reject("The built-in module name 'prometheum.v1' is reserved");
		if (name.size() > 255)
			return reject("An Agent behaviour helper module name cannot exceed 255 bytes");

		bool atSegmentStart = true;
		for (unsigned char character : name)
		{
			if (character == '.')
			{
				if (atSegmentStart)
					return reject("An Agent behaviour helper module name must use dotted identifiers");
				atSegmentStart = true;
				continue;
			}
			auto const letter = (character >= 'a' && character <= 'z')
				|| (character >= 'A' && character <= 'Z');
			auto const valid = atSegmentStart
				? (letter || character == '_')
				: (letter || (character >= '0' && character <= '9')
					|| character == '_');
			if (!valid)
				return reject("An Agent behaviour helper module name must use dotted identifiers");
			atSegmentStart = false;
		}
		if (atSegmentStart)
			return reject("An Agent behaviour helper module name must use dotted identifiers");
		auto const lastSeparator = name.find_last_of('.');
		auto const finalSegment = lastSeparator == std::string::npos
			? name : name.substr(lastSeparator + 1);
		if (finalSegment == "lua" || finalSegment == "so"
			|| finalSegment == "dll" || finalSegment == "dylib")
			return reject("An Agent behaviour helper module name cannot be a file or native-module name");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	std::unique_ptr<AgentBehaviour> AgentBehaviour::create(std::string name,
		std::string sourceModulePath, std::vector<AgentBehaviourSchemaField> schema,
		uint64_t revision)
	{
		// Not std::make_unique: that free function is not a friend, and the
		// constructor deliberately is not public.
		return std::unique_ptr<AgentBehaviour>(new AgentBehaviour(std::move(name),
			std::move(sourceModulePath), std::move(schema), revision));
	}

	std::string AgentBehaviour::trimName(std::string const& value)
	{
		auto const first = value.find_first_not_of(" \t");
		if (first == std::string::npos) return {};
		auto const last = value.find_last_not_of(" \t");
		return value.substr(first, last - first + 1);
	}

	bool AgentBehaviour::nameIsValid(std::string const& trimmed,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (trimmed.empty()) return reject("An Agent behaviour name cannot be blank");
		if (trimName(trimmed) != trimmed)
			return reject("An Agent behaviour name must be trimmed");
		if (trimmed.size() > MaxNameBytes)
			return reject("An Agent behaviour name cannot exceed "
				+ std::to_string(MaxNameBytes) + " bytes (got "
				+ std::to_string(trimmed.size()) + ")");
		if (!utf8TextIsValid(trimmed, "An Agent behaviour name", diagnostic))
			return false;
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool AgentBehaviour::sourceModulePathIsValid(std::string const& path,
		std::string* diagnostic)
	{
		auto reject = [diagnostic](std::string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (path.empty())
			return reject("An Agent behaviour source module path cannot be blank");
		if (path.find('\\') != std::string::npos)
			return reject("An Agent behaviour source module path must use '/' separators");
		if (path.find('\0') != std::string::npos || path.find(':') != std::string::npos)
			return reject("An Agent behaviour source module path contains an invalid character");
		if (path.front() == '/')
			return reject("An Agent behaviour source module path cannot be absolute");
		if (path.size() < 4 || !path.ends_with(".lua"))
			return reject("An Agent behaviour source module path must name a .lua file");
		std::filesystem::path const lexical(path);
		if (lexical.is_absolute())
			return reject("An Agent behaviour source module path cannot be absolute");
		for (auto const& part : lexical)
		{
			auto const component = part.string();
			if (component.empty() || component == ".")
				return reject("An Agent behaviour source module path is not normalized");
			if (component == "..")
				return reject("An Agent behaviour source module path cannot leave the registry package");
		}
		if (lexical.lexically_normal().generic_string() != path)
			return reject("An Agent behaviour source module path is not normalized");
		if (diagnostic) diagnostic->clear();
		return true;
	}
}
