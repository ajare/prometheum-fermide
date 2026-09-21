#pragma once

// Agent clipboard payloads and Agent placement (ticket #113).
//
// An Agent's clipboard payload carries its Agent group by *name*, never by
// its Building-local AgentGroupId: an AgentGroupId means nothing outside the
// Building that issued it, while the name is what a destination Building can
// either match against a group it already defines or create for itself
// (ADR 0006).
//
// The name is resolved at placement rather than at parse, because a paste is
// deferred - the Agent falls from the cursor and may be cancelled on the way
// down - and a cancelled paste must leave no group behind. So the payload
// keeps the name as written, arming judges it against the same naming rule
// the Agent group panels use, and only the landing writes: the group when the
// destination does not already define that name, the Agent, and the
// assignment, all as exactly one document edit.
//
// This lives in its own translation unit - as the Agent group panels did in
// #109 and #110 - so the headless smoke checks drive the same encode, parse
// and place code the GUI calls rather than a mirrored copy, and UI.cpp's
// spdlog/nfd dependencies never link headlessly.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <yaml-cpp/yaml.h>

#include "core/EntityId.h"

namespace core
{
	class Building;
	class Sector;
}

// What an Agent clipboard payload carries.
struct AgentClipboardPayload
{
	std::string name;
	std::uint32_t flags{ 0 };

	// The Agent's Agent group, by name. nullopt means no Agent group, and is
	// written as no `group` key at all rather than an empty one: an
	// ungrouped Agent's payload is then what a build from before Agent
	// grouping existed wrote, and a payload that simply omits the key reads
	// back the same way.
	std::optional<std::string> group;
};

// The payload a copy of `agent` carries. `name` is the name the copy will
// use - the caller owns name uniqueness, the payload owns the
// classification. An Agent with no Agent group yields no group.
AgentClipboardPayload makeAgentClipboardPayload(core::Building const& building,
	core::AgentId agent, std::string name);

// The complete clipboard text for an Agent payload: the same envelope every
// other clipboard object uses, with the Agent's `object` map written from
// `payload`.
std::string makeAgentClipboardText(AgentClipboardPayload const& payload, bool cut);

// Read an Agent clipboard `object` map back into `payload`. An absent
// `group` is not an error, it is an ungrouped Agent; a present one has to
// read as text, so a value that is not a name - a number, a sequence, an
// empty scalar - is refused with a diagnostic rather than turned into some
// other value.
bool readAgentClipboardObject(YAML::Node const& object,
	AgentClipboardPayload& payload, std::string& diagnostic);

// The Building's Agent group whose name is `name`, compared the way the
// Building compares group names - trimmed, and case-sensitive - or an empty
// AgentGroupId when the Building defines no such group.
core::AgentGroupId findAgentGroupByName(core::Building const& building,
	std::string const& name);

// A paste the editor has accepted but not yet placed: the Agent is still
// falling. Nothing has been written to the document, and nothing will be
// until the placement lands.
struct PendingAgentPlacement
{
	AgentClipboardPayload payload;
	std::shared_ptr<const core::Sector> sector;
	std::uint32_t deckOffset{ 0 };
	float localX{ 0.0f };

	bool armed() const { return sector != nullptr; }

	// Dropping a pending placement is how a cancelled paste is expressed:
	// the payload goes, and there is nothing left to land.
	void cancel() { *this = PendingAgentPlacement{}; }
};

// Accept `payload` for deferred placement at `sector`/`deckOffset`/`localX`.
// The Agent group name is judged here - before anything is deferred - so an
// unusable payload is reported at the keystroke rather than after a fall
// that was always going to fail. Arming writes nothing to the Building and
// commits no undo entry.
bool armAgentPlacement(PendingAgentPlacement& pending,
	AgentClipboardPayload const& payload,
	std::shared_ptr<const core::Sector> sector,
	std::uint32_t deckOffset, float localX, std::string& diagnostic);

// Create `payload` in `building` as exactly one document edit: the Agent's
// Agent group is reused when the Building already defines that exact name,
// created when it does not, and the Agent is created and assigned in the
// same edit. Any failure - an unusable group name, a refused Agent creation,
// a refused assignment - leaves neither a new Agent nor a new Agent group
// behind and commits no undo entry, reporting the reason through
// `diagnostic`.
bool commitAgentPlacement(std::shared_ptr<core::Building> const& building,
	AgentClipboardPayload const& payload,
	std::shared_ptr<const core::Sector> sector,
	std::uint32_t deckOffset, float localX,
	core::AgentId& placed, std::string& diagnostic);

// Land an armed placement through commitAgentPlacement(). The pending state
// is dropped either way: a placement that landed has been made, and one that
// was refused has nowhere left to go.
bool commitPendingAgentPlacement(PendingAgentPlacement& pending,
	std::shared_ptr<core::Building> const& building,
	core::AgentId& placed, std::string& diagnostic);

// The removal a cut performs: the Agent leaves the Building, and nothing
// else leaves with it. Its Agent group in particular stays defined -
// cutting one member of a group is not a way to delete the group, and the
// group's other members keep their assignment - and a refusal changes
// nothing and reports why. Ticket #57: an Agent which still owns a capacity
// resource is refused rather than deleted with the ownership left behind.
bool cutAgent(std::shared_ptr<core::Building> const& building,
	core::AgentId agent, std::string& diagnostic);
