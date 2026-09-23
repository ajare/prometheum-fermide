#pragma once

// The Agents section's Agent-group table (ticket #109). This lives in its own
// translation unit - rather than inside UI.cpp - so the headless smoke checks
// can compile and render the real panel inside a CPU-side ImGui context;
// UI.cpp's spdlog/nfd dependencies never link headlessly. It is the same seam
// ticket #99 opened for the Door panel.
//
// The commit functions below are the panel's own boundary: they are exactly
// what the widgets call, so a headless check exercises the real add/rename
// path - snapshot, validation, history - rather than a mirrored copy.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/EntityId.h"

namespace core
{
	class World;
}

// Creates a group named `rawName` (trimmed by the World) and commits one
// undoable document edit. Returns the new AgentGroupId, or a falsy ID with
// `diagnostic` naming the refused name's problem and no history entry.
core::AgentGroupId commitAgentGroupAdd(
	std::shared_ptr<core::World> const& world,
	std::string const& rawName, std::string& diagnostic);

// Renames an existing group in place, keeping its ID and its position in the
// creation order. Commits one undoable document edit on success; a refused
// rename changes nothing and leaves the history untouched.
bool commitAgentGroupRename(
	std::shared_ptr<core::World> const& world, core::AgentGroupId id,
	std::string const& rawName, std::string& diagnostic);

// Deletes an existing group and, in the same World operation, returns
// every Agent assigned to it to no Agent group. The group removal and all of
// the assignment clearing are one undoable document edit: there is no way to
// undo one half without the other. Returns false with `diagnostic` and no
// history entry when the group is unknown or there is no World.
bool commitAgentGroupDelete(
	std::shared_ptr<core::World> const& world, core::AgentGroupId id,
	std::string& diagnostic);

// Whether deleting `id` has to be put to the user first: a group with at
// least one assigned Agent does, an empty group does not. Throws if the
// group is not one this World issued, the same way the count does.
bool agentGroupDeleteRequiresConfirmation(core::World const& world,
	core::AgentGroupId id);

// The confirmation's text: the group's name and the authoritative number of
// Agents that will revert to no Agent group, read off the World rather
// than from anything the panel remembers. Throws if the group is not one
// this World issued.
std::string agentGroupDeleteConfirmationText(core::World const& world,
	core::AgentGroupId id);

// The Delete control's entry point. An empty group is deleted immediately,
// in one document edit, with no confirmation asked. An occupied group is not
// touched here: the confirmation is armed instead, and nothing - no group,
// assignment, count, dirty flag, or history entry - changes until the user
// answers it. A group this World does not know is refused and logged.
void requestAgentGroupDelete(
	std::shared_ptr<core::World> const& world, core::AgentGroupId id);

// Whether a deletion is awaiting the user's answer, optionally reporting
// which group it was armed for and how many of its Agents the World said
// would revert to no Agent group when the request was made.
bool agentGroupDeletePending(core::AgentGroupId* id = nullptr,
	uint32_t* memberCount = nullptr);

// Answers the armed deletion with the delete itself: one document edit
// covering the group and every assignment cleared. Returns false, having
// changed nothing, when no deletion is armed or the World refuses the
// group.
bool confirmPendingAgentGroupDelete(
	std::shared_ptr<core::World> const& world, std::string& diagnostic);

// Answers the armed deletion with a cancel. Nothing is committed and nothing
// stays armed: the World, its Agents, the document's dirty state and the
// undo history are all left exactly as the request found them.
void cancelPendingAgentGroupDelete();

// Drops every in-progress inline edit. Called when the document is replaced -
// new, opened, or restored by undo - so a stale editor never resumes against
// a different World's group.
void resetAgentGroupsPanelState();

// The Groups table's column headers, in the order the panel declares them:
// the group's name, its aggregate Active toggle, its live membership count,
// then the row's Delete control. The panel sets up its columns from this exact
// list, so the list a check reads is the list the table is built with rather
// than a second copy kept for testing.
std::vector<std::string> const& agentGroupsPanelColumns();

// Renders one group's eye toggle. The eye is open while any current member is
// active: pressing it deactivates all members; when none are active, pressing
// the slashed eye activates all members. This is a bulk edit of the Agents'
// own flags, so a member may still be toggled individually afterwards. Empty
// groups and all groups while the simulation runs have a disabled control.
void renderAgentGroupActivationCell(
	std::shared_ptr<core::World> const& world, core::AgentGroupId id);

// The text the Agents column shows for one group: how many of the World's
// Agents are assigned to it. The count is derived through the World on
// every call - the group stores no total of its own - so it covers every
// Layer and Sector and every movement state, and is current the moment an
// assignment is made. Throws if the group is not one this World issued.
std::string agentGroupMemberCountLabel(core::World const& world,
	core::AgentGroupId id);

// Renders one membership count cell: the group's current count, read off the
// World. Read-only, so there is nothing here to commit and no edit to undo.
void renderAgentGroupMemberCountCell(core::World const& world,
	core::AgentGroupId id);

// Renders one row's Delete cell: the trash control, offered for every Agent
// group whether or not the simulation is running. Pressing it goes through
// requestAgentGroupDelete, so the empty/occupied split is the same one every
// other caller gets.
void renderAgentGroupDeleteCell(
	std::shared_ptr<core::World> const& world, core::AgentGroupId id);

// Renders the occupied-group confirmation, and nothing at all when no
// deletion is armed. Drawn every frame the Groups panel is drawn, so a
// popup that was opened can always be answered, dismissed, or closed from
// under itself. Kept public so a headless check can put the real modal on
// screen and see that it is really there.
void renderAgentGroupDeleteConfirmation(
	std::shared_ptr<core::World> const& world);

// Renders the Groups table for the Agents collapsible section: one inline
// rename editor per group, in creation order, each with its Active eye, live
// Agents count and Delete control, plus the add row and the deletion
// confirmation. Group activation is editable only while the simulation is
// paused; the rest of the panel remains available while it runs.
void renderAgentGroupsPanel(std::shared_ptr<core::World> const& world);
