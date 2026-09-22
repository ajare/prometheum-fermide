// Agent clipboard payloads and Agent placement; see include/AgentClipboard.h.
//
// Three rules hold this file together.
//
// The clipboard carries the Agent group by name. A Building-local
// AgentGroupId is a receipt for one Building's registry and says nothing to
// the next one, so it never crosses the clipboard at all (ADR 0006).
//
// Agent tag IDs cross only with their registry UUID. The complete assignment
// set and exact revisioned samples travel together; a different or absent
// destination registry refuses them, while an untagged Agent remains portable.
//
// Nothing is written until the placement lands. Arming a placement validates
// and stores; a cancelled placement is a dropped struct, so it cannot leave
// an Agent, group, or tag assignment behind because it never made one.
//
// What the placement does, it does once. The group (when the destination
// does not already define that exact name), the Agent, and all assignments
// are one document edit, and a refusal anywhere leaves none of them behind.

#include "AgentClipboard.h"

#include <cmath>
#include <exception>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

#include "core/Agent.h"
#include "core/AgentGroup.h"
#include "core/AgentTagRegistry.h"
#include "core/Building.h"
#include "core/Exceptions.h"
#include "core/Sector.h"

#include "DocumentEdit.h"

using namespace std;

namespace
{
	// The clipboard envelope, shared with every other clipboard object type.
	// Version 1 covers an optional `group`: an older reader ignores a key it
	// does not know, and this reader treats an absent key as "no Agent group",
	// so the field needs no new version of its own. The optional `active` key
	// added with Agent activation (#118) behaves the same way: absent reads
	// back as activated, and an older reader ignores it.
	char const* const ClipboardKey{ "prometheumClipboard" };
	uint32_t const ClipboardVersion{ 1 };

	// The Agent group name a payload will use: the domain's own trim, judged
	// by the same rule that governs a group typed into the Groups panel, so a
	// pasted name is refused for exactly the reason a typed one would be.
	bool groupNameUsable(string const& written, string& trimmed, string& diagnostic)
	{
		trimmed = core::AgentGroup::trimName(written);
		return core::AgentGroup::nameIsValid(trimmed, &diagnostic);
	}

	bool clipboardTagStateIsWellFormed(AgentClipboardPayload const& payload,
		string& diagnostic)
	{
		auto reject = [&diagnostic](string reason)
		{
			diagnostic = std::move(reason);
			return false;
		};
		if (payload.agentTags.empty())
		{
			if (payload.agentTagRegistryUuid || payload.walkSpeedModifierSample
				|| payload.heightModifierSample)
			{
				return reject(
					"An untagged Agent clipboard payload cannot carry registry or sample state");
			}
			return true;
		}
		if (!payload.agentTagRegistryUuid
			|| !core::AgentTagRegistry::uuidIsValid(*payload.agentTagRegistryUuid))
		{
			return reject("A tagged Agent clipboard payload requires a valid registry UUID");
		}
		for (auto const tag : payload.agentTags)
			if (!tag) return reject("Clipboard Agent tag IDs cannot be zero");

		auto validateSample = [&](char const* name, core::SampledAgentPropertyType type,
			optional<core::AgentPropertySample> const& sample)
		{
			if (!sample) return true;
			if (sample->type != type)
				return reject(format("Clipboard {} sample has the wrong type", name));
			if (!sample->sourceTag || !payload.agentTags.contains(sample->sourceTag))
				return reject(format(
					"Clipboard {} sample source must be an assigned Agent tag", name));
			if (sample->propertyRevision == 0)
				return reject("Clipboard sampled Agent property revision cannot be zero");
			if (!isfinite(sample->value))
				return reject(format("Clipboard {} sample must be finite", name));
			return true;
		};
		return validateSample("Walk speed modifier",
			core::SampledAgentPropertyType::WalkSpeedModifier,
			payload.walkSpeedModifierSample)
			&& validateSample("Height modifier",
				core::SampledAgentPropertyType::HeightModifier,
				payload.heightModifierSample);
	}

	bool clipboardTagStateFitsBuilding(core::Building const& building,
		AgentClipboardPayload const& payload, string& diagnostic)
	{
		if (!clipboardTagStateIsWellFormed(payload, diagnostic)) return false;
		if (payload.agentTags.empty()) return true;
		if (!building.hasAttachedAgentTagRegistry())
		{
			diagnostic = "Tagged Agents can only be pasted into a Building with the same attached Agent tag registry";
			return false;
		}
		auto const& registry = building.getAgentTagRegistry();
		if (registry->getUuid() != *payload.agentTagRegistryUuid)
		{
			diagnostic = "Tagged Agents can only be pasted into a Building using the same Agent tag registry UUID";
			return false;
		}
		return building.validateAgentTagAssignments(payload.agentTags,
			payload.walkSpeedModifierSample, payload.heightModifierSample, &diagnostic);
	}
}

AgentClipboardPayload makeAgentClipboardPayload(core::Building const& building,
	core::AgentId agent, string name)
{
	AgentClipboardPayload payload;
	payload.name = std::move(name);

	auto const lookup = building.lookupAgent(agent);
	if (!lookup) return payload;

	payload.flags = lookup.entity->getFlags();
	payload.active = lookup.entity->isActive();
	payload.agentTags = lookup.entity->getAgentTagIds();
	payload.walkSpeedModifierSample = lookup.entity->getWalkSpeedModifierSample();
	payload.heightModifierSample = lookup.entity->getHeightModifierSample();
	if (!payload.agentTags.empty())
	{
		if (!building.hasAgentTagRegistryReference())
			throw runtime_error(
				"A tagged Agent's Building has no Agent tag registry identity");
		payload.agentTagRegistryUuid = building.getExpectedAgentTagRegistryUuid();
	}

	// The group's name crosses; its ID stays home. An Agent holding an ID the
	// Building cannot resolve reads back as ungrouped rather than inventing a
	// name for a group that is gone - a state the Building is not supposed to
	// reach at all, since deleting a group takes every assignment with it
	// (#112).
	auto const assigned = lookup.entity->getAgentGroupId();
	if (assigned)
	{
		auto const group = building.lookupAgentGroup(assigned);
		if (group) payload.group = group.entity->getName();
	}

	return payload;
}

string makeAgentClipboardText(AgentClipboardPayload const& payload, bool cut)
{
	string diagnostic;
	if (!clipboardTagStateIsWellFormed(payload, diagnostic))
		throw invalid_argument(diagnostic);

	YAML::Emitter output;
	output << YAML::BeginMap
		<< YAML::Key << ClipboardKey << YAML::Value << YAML::BeginMap
		<< YAML::Key << "version" << YAML::Value << ClipboardVersion
		<< YAML::Key << "operation" << YAML::Value << (cut ? "cut" : "copy")
		<< YAML::Key << "type" << YAML::Value << "Agent"
		<< YAML::Key << "object" << YAML::Value << YAML::BeginMap
		<< YAML::Key << "name" << YAML::Value << payload.name
		<< YAML::Key << "flags" << YAML::Value << payload.flags;
	// No group, no key: the payload says nothing about a classification
	// rather than saying "the empty one".
	if (payload.group) output << YAML::Key << "group" << YAML::Value << *payload.group;
	// Same shape for activation: an activated Agent writes no `active` key,
	// so a payload written before activation existed reads back activated
	// (#118).
	if (!payload.active) output << YAML::Key << "active" << YAML::Value << false;
	if (!payload.agentTags.empty())
	{
		output << YAML::Key << "agentTagRegistryUuid" << YAML::Value
			<< *payload.agentTagRegistryUuid
			<< YAML::Key << "tags" << YAML::Value << YAML::Flow << YAML::BeginSeq;
		for (auto const tag : payload.agentTags) output << tag.value;
		output << YAML::EndSeq;
		if (payload.walkSpeedModifierSample || payload.heightModifierSample)
		{
			output << YAML::Key << "propertySamples" << YAML::Value << YAML::BeginSeq;
			auto writeSample = [&output](char const* type,
				core::AgentPropertySample const& sample)
			{
				output << YAML::BeginMap
					<< YAML::Key << "type" << YAML::Value << type
					<< YAML::Key << "sourceTag" << YAML::Value << sample.sourceTag.value
					<< YAML::Key << "propertyRevision" << YAML::Value
					<< sample.propertyRevision
					<< YAML::Key << "value" << YAML::Value << sample.value
					<< YAML::EndMap;
			};
			if (payload.walkSpeedModifierSample)
				writeSample("walkSpeedModifier", *payload.walkSpeedModifierSample);
			if (payload.heightModifierSample)
				writeSample("heightModifier", *payload.heightModifierSample);
			output << YAML::EndSeq;
		}
	}
	output << YAML::EndMap << YAML::EndMap << YAML::EndMap;

	if (!output.good()) throw runtime_error(output.GetLastError());
	return string(output.c_str());
}

bool readAgentClipboardObject(YAML::Node const& object,
	AgentClipboardPayload& payload, string& diagnostic)
{
	payload = {};
	diagnostic.clear();

	if (!object || !object.IsMap())
	{
		diagnostic = "Clipboard object definition is required";
		return false;
	}

	if (!object["name"])
	{
		diagnostic = "Clipboard field 'name' is required";
		return false;
	}
	try { payload.name = object["name"].as<string>(); }
	catch (exception const&)
	{
		diagnostic = "Clipboard field 'name' has an invalid value";
		return false;
	}
	if (payload.name.empty())
	{
		diagnostic = "Agent name cannot be empty";
		return false;
	}

	if (!object["flags"])
	{
		diagnostic = "Clipboard field 'flags' is required";
		return false;
	}
	try { payload.flags = object["flags"].as<uint32_t>(); }
	catch (exception const&)
	{
		diagnostic = "Clipboard field 'flags' has an invalid value";
		return false;
	}

	// An absent `active` is an activated Agent, which is exactly how a
	// payload written before activation existed reads. A present one has to
	// be a boolean: a value of any other shape is refused rather than coerced,
	// because a silently-activated paste of a deactivated Agent would start
	// simulating someone the author had parked (#118).
	if (object["active"])
	{
		try { payload.active = object["active"].as<bool>(); }
		catch (exception const&)
		{
			diagnostic = "Clipboard field 'active' must be a boolean";
			return false;
		}
	}

	// An absent `group` is an ungrouped Agent, which is exactly how a
	// payload written before Agent grouping existed reads. A present one has
	// to be a name: a value that cannot be read as text is refused rather
	// than quietly turned into one, because a silently-empty group would drop
	// the Agent's classification without telling anyone.
	if (object["group"])
	{
		string value;
		try { value = object["group"].as<string>(); }
		catch (exception const&)
		{
			diagnostic = "Clipboard field 'group' must be an Agent group name";
			return false;
		}
		payload.group = value;
	}

	if (object["agentTagRegistryUuid"])
	{
		try { payload.agentTagRegistryUuid
			= object["agentTagRegistryUuid"].as<string>(); }
		catch (exception const&)
		{
			diagnostic = "Clipboard field 'agentTagRegistryUuid' must be a registry UUID";
			return false;
		}
	}
	if (object["tags"])
	{
		auto const tags = object["tags"];
		if (!tags.IsSequence())
		{
			diagnostic = "Clipboard field 'tags' must be a sequence of Agent tag IDs";
			return false;
		}
		for (auto const& entry : tags)
		{
			uint64_t value;
			try { value = entry.as<uint64_t>(); }
			catch (exception const&)
			{
				diagnostic = "Clipboard Agent tag IDs must be unsigned integers";
				return false;
			}
			core::AgentTagId const id{ value };
			if (!id)
			{
				diagnostic = "Clipboard Agent tag IDs cannot be zero";
				return false;
			}
			if (!payload.agentTags.insert(id).second)
			{
				diagnostic = format(
					"Clipboard Agent tag IDs must be unique ({} appears twice)", value);
				return false;
			}
		}
	}
	if (object["propertySamples"])
	{
		auto const samples = object["propertySamples"];
		if (!samples.IsSequence())
		{
			diagnostic = "Clipboard field 'propertySamples' must be a sequence";
			return false;
		}
		for (auto const& entry : samples)
		{
			if (!entry.IsMap())
			{
				diagnostic = "Clipboard Agent property samples must be maps";
				return false;
			}
			core::AgentPropertySample sample;
			string type;
			try
			{
				type = entry["type"].as<string>();
				sample.sourceTag = core::AgentTagId{ entry["sourceTag"].as<uint64_t>() };
				sample.propertyRevision = entry["propertyRevision"].as<uint64_t>();
				sample.value = entry["value"].as<float>();
			}
			catch (exception const&)
			{
				diagnostic = "Clipboard Agent property sample has an invalid or missing field";
				return false;
			}

			optional<core::AgentPropertySample>* destination{ nullptr };
			if (type == "walkSpeedModifier")
			{
				sample.type = core::SampledAgentPropertyType::WalkSpeedModifier;
				destination = &payload.walkSpeedModifierSample;
			}
			else if (type == "heightModifier")
			{
				sample.type = core::SampledAgentPropertyType::HeightModifier;
				destination = &payload.heightModifierSample;
			}
			else
			{
				diagnostic = "Clipboard Agent property sample type is not supported";
				return false;
			}
			if (*destination)
			{
				diagnostic = format(
					"Clipboard Agent contains more than one {} sample",
					type == "walkSpeedModifier" ? "Walk speed modifier" : "Height modifier");
				return false;
			}
			*destination = sample;
		}
	}

	return clipboardTagStateIsWellFormed(payload, diagnostic);
}

core::AgentGroupId findAgentGroupByName(core::Building const& building,
	string const& name)
{
	// Stored names are already trimmed, so this is the Building's own
	// comparison: exact, and case-sensitive. "Crew" never matches "crew".
	for (auto const id : building.getAgentGroupIds())
	{
		auto const group = building.lookupAgentGroup(id);
		if (group && group.entity->getName() == name) return id;
	}
	return {};
}

bool armAgentPlacement(PendingAgentPlacement& pending,
	core::Building const& building, AgentClipboardPayload const& payload,
	shared_ptr<const core::Sector> sector,
	uint32_t deckOffset, float localX, string& diagnostic)
{
	pending.cancel();
	diagnostic.clear();

	if (!sector)
	{
		diagnostic = "There is no sector to place an Agent in";
		return false;
	}
	if (payload.name.empty())
	{
		diagnostic = "Agent name cannot be empty";
		return false;
	}

	// Judged now, at the keystroke: an Agent group name that the Building
	// would refuse has no business being deferred into a fall that is only
	// going to fail with it later.
	if (payload.group)
	{
		string trimmed;
		if (!groupNameUsable(*payload.group, trimmed, diagnostic)) return false;
	}
	if (!clipboardTagStateFitsBuilding(building, payload, diagnostic)) return false;

	pending.payload = payload;
	pending.sector = sector;
	pending.deckOffset = deckOffset;
	pending.localX = localX;
	return true;
}

bool commitAgentPlacement(shared_ptr<core::Building> const& building,
	AgentClipboardPayload const& payload,
	shared_ptr<const core::Sector> sector,
	uint32_t deckOffset, float localX,
	core::AgentId& placed, string& diagnostic)
{
	placed = {};
	diagnostic.clear();

	if (!building)
	{
		diagnostic = "There is no Building to place an Agent in";
		return false;
	}
	if (!sector)
	{
		diagnostic = "There is no sector to place an Agent in";
		return false;
	}
	if (payload.name.empty())
	{
		diagnostic = "Agent name cannot be empty";
		return false;
	}

	// Re-judged here as well as at arming: this is the seam that writes, and
	// a payload that reached it by any other route still has to clear the
	// same bar before anything is created.
	optional<string> groupName;
	if (payload.group)
	{
		string trimmed;
		if (!groupNameUsable(*payload.group, trimmed, diagnostic)) return false;
		groupName = trimmed;
	}
	if (!clipboardTagStateFitsBuilding(*building, payload, diagnostic)) return false;
	if (!payload.agentTags.empty() && !building->isSimulationPaused())
	{
		diagnostic = "Pause the simulation before pasting a tagged Agent";
		return false;
	}

	// Captured before the first write, so the undo entry holds the document
	// exactly as it stood before the paste. Every refusal below drops it
	// uncommitted: no entry, and nothing to undo.
	auto const undo = captureDocumentSnapshot(building);
	if (!undo)
	{
		diagnostic = "Could not capture the editor state for the Agent placement";
		return false;
	}

	core::AgentId agentId{};
	core::AgentGroupId createdGroup{};

	// The compensation for a failure partway through, in the reverse order
	// of the writes. The normal path never reaches it with anything to undo;
	// it is here so an unexpected refusal cannot abandon an Agent, a group, or
	// an assignment that nothing points at.
	auto rollBack = [&]()
	{
		string compensation;
		if (agentId)
		{
			auto const removal = building->removeAgent(agentId);
			if (!removal.removed) compensation += "; and " + removal.diagnostic;
		}
		if (createdGroup)
		{
			string groupDiagnostic;
			if (!building->deleteAgentGroup(createdGroup, &groupDiagnostic))
				compensation += "; and " + groupDiagnostic;
		}
		return compensation;
	};

	try
	{
		// The Agent is created first: if that is refused, no group has been
		// made yet, so the common failure leaves nothing behind at all.
		agentId = building->createAgent(payload.name, sector->getIndex(), deckOffset, localX);
		auto const created = building->lookupAgent(agentId).entity;
		if (!created)
		{
			diagnostic = "The placed Agent could not be found in the Building" + rollBack();
			return false;
		}
		created->setFlags(payload.flags);
		// Activation travels with the payload's other authored state. The
		// setter is the raw Agent seam, like setFlags above: placement is
		// allowed while the simulation runs, and a pasted Agent the payload
		// deactivated must land deactivated rather than be refused (#118).
		created->setActive(payload.active);

		if (groupName)
		{
			// The destination's own group when it already defines this exact
			// name - a paste is never how a duplicate group gets made - and a
			// new one only where the name is genuinely missing.
			auto groupId = findAgentGroupByName(*building, *groupName);
			if (!groupId)
			{
				groupId = building->addAgentGroup(*groupName);
				createdGroup = groupId;
			}
			string assignDiagnostic;
			if (!building->setAgentGroup(agentId, groupId, &assignDiagnostic))
			{
				diagnostic = "The pasted Agent could not be assigned to its Agent group: "
					+ assignDiagnostic + rollBack();
				return false;
			}
		}
		if (!payload.agentTags.empty())
		{
			string assignDiagnostic;
			if (!building->restoreAgentTagAssignments(agentId, payload.agentTags,
				payload.walkSpeedModifierSample, payload.heightModifierSample,
				&assignDiagnostic))
			{
				diagnostic = "The pasted Agent's tag assignments could not be restored: "
					+ assignDiagnostic + rollBack();
				return false;
			}
		}
	}
	catch (core::Exception const& error)
	{
		diagnostic = error.getMessage() + rollBack();
		return false;
	}
	catch (exception const& error)
	{
		diagnostic = string(error.what()) + rollBack();
		return false;
	}

	// Agent, Agent group and assignment land together, as one document edit:
	// one undo entry covers all three, and undoing it takes all three away.
	commitDocumentEdit(std::move(undo));
	placed = agentId;
	return true;
}

bool commitPendingAgentPlacement(PendingAgentPlacement& pending,
	shared_ptr<core::Building> const& building,
	core::AgentId& placed, string& diagnostic)
{
	if (!pending.armed())
	{
		placed = {};
		diagnostic = "There is no pending Agent placement to land";
		return false;
	}

	// The pending state is taken out of the caller's hands before the write
	// starts, so a placement cannot be landed twice - not by a retry, and not
	// by a second frame of the fall.
	auto const payload = pending.payload;
	auto const sector = pending.sector;
	auto const deckOffset = pending.deckOffset;
	auto const localX = pending.localX;
	pending.cancel();

	return commitAgentPlacement(building, payload, sector, deckOffset, localX,
		placed, diagnostic);
}

bool cutAgent(shared_ptr<core::Building> const& building,
	core::AgentId agent, string& diagnostic)
{
	diagnostic.clear();

	if (!building)
	{
		diagnostic = "There is no Building to cut an Agent from";
		return false;
	}
	if (!agent)
	{
		diagnostic = "There is no Agent to cut";
		return false;
	}

	auto const lookup = building->lookupAgent(agent);
	if (!lookup)
	{
		diagnostic = lookup.diagnostic.empty() ? "The Agent is not one this Building owns"
			: lookup.diagnostic;
		return false;
	}

	// Only the Agent is taken. The Building's Agent group registry is not
	// touched, so the group the cut Agent belonged to stays defined - for
	// its remaining members now, and for the paste this cut put on the
	// clipboard afterwards.
	lookup.entity->clearPath();
	auto const removal = building->removeAgent(agent);
	if (!removal.removed)
	{
		diagnostic = removal.diagnostic;
		return false;
	}
	return true;
}
