#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace core
{
	// The typed configuration vocabulary an Agent behaviour declares. Schemas
	// are structured registry metadata, never executable Lua: configuration is
	// validated against them before any source executes.
	enum class AgentBehaviourSchemaType
	{
		Boolean,
		Integer,
		Number,
		String,
		// A whole-number simulation-tick duration.
		Duration,
		// A reference to a named Building-owned Marker.
		Marker,
		// An ordered collection of one element type.
		List,
		// An ordered set of named fields.
		Record
	};

	// One typed configuration field declared by an Agent behaviour schema.
	// List uses children as its single element type; Record uses them as its
	// ordered fields. Scalar types leave children empty.
	struct AgentBehaviourSchemaField
	{
		std::string name;
		AgentBehaviourSchemaType type{ AgentBehaviourSchemaType::Boolean };
		std::vector<AgentBehaviourSchemaField> children;

		bool operator==(AgentBehaviourSchemaField const& other) const = default;
	};

	char const* agentBehaviourSchemaTypeName(AgentBehaviourSchemaType type);
	bool agentBehaviourSchemaTypeFromName(std::string const& name,
		AgentBehaviourSchemaType& type);

	// Validates a schema field list the way a Record body is validated: field
	// names are trimmed, non-empty, valid UTF-8, at most 63 bytes, and unique
	// within their containing Record; Lists declare exactly one element type;
	// Records declare at least one field; nesting is bounded.
	bool agentBehaviourSchemaFieldsAreValid(
		std::vector<AgentBehaviourSchemaField> const& fields,
		std::string* diagnostic = nullptr);

	// A durable, reusable Agent behaviour definition owned by an Agent
	// behaviour registry. Its ID is registry-local, stable across rename, and
	// never reused. The revision increases monotonically whenever the authored
	// definition a Building's configuration was validated against changes.
	// The source module path is registry-relative: it identifies one managed
	// Lua file inside the registry package and never names an absolute path or
	// a location outside the package directory.
	class AgentBehaviour
	{
		friend class AgentBehaviourRegistry;

		std::string mName;
		uint64_t mRevision{ 1 };
		std::string mSourceModulePath;
		std::vector<AgentBehaviourSchemaField> mSchema;

		AgentBehaviour(std::string name, std::string sourceModulePath,
			std::vector<AgentBehaviourSchemaField> schema, uint64_t revision)
			: mName(std::move(name))
			, mRevision(revision)
			, mSourceModulePath(std::move(sourceModulePath))
			, mSchema(std::move(schema))
		{
		}

		void setName(std::string name) { mName = std::move(name); }
		void setRevision(uint64_t revision) { mRevision = revision; }
		void setSourceModulePath(std::string path)
		{
			mSourceModulePath = std::move(path);
		}
		void setSchema(std::vector<AgentBehaviourSchemaField> schema)
		{
			mSchema = std::move(schema);
		}

	public:
		static constexpr size_t MaxNameBytes{ 63 };

		static std::unique_ptr<AgentBehaviour> create(std::string name,
			std::string sourceModulePath,
			std::vector<AgentBehaviourSchemaField> schema, uint64_t revision = 1);
		static std::string trimName(std::string const& value);
		static bool nameIsValid(std::string const& trimmed,
			std::string* diagnostic = nullptr);
		static bool sourceModulePathIsValid(std::string const& path,
			std::string* diagnostic = nullptr);

		std::string const& getName() const { return mName; }
		uint64_t getRevision() const { return mRevision; }
		std::string const& getSourceModulePath() const { return mSourceModulePath; }
		std::vector<AgentBehaviourSchemaField> const& getSchema() const
		{
			return mSchema;
		}
	};
}
