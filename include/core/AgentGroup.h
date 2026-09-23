#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace core
{
	// A named, World-scoped classification an Agent may be assigned to
	// (ADR 0006). The World owns every AgentGroup, allocates its stable
	// AgentGroupId, and is the only thing allowed to rename it: a name is only
	// meaningful against the World's own uniqueness rule, so no public
	// setter exists for callers to slip past that check.
	//
	// An AgentGroup is authored editor metadata. It never reaches the runtime
	// snapshot, the simulation events, or any traversal decision.
	class AgentGroup
	{
		friend class World;

		std::string mName;

		explicit AgentGroup(std::string name)
			: mName(std::move(name))
		{
		}

		// Renaming keeps the group's identity: only the name moves, and the
		// World has already validated the new value before calling this.
		void setName(std::string name) { mName = std::move(name); }

	public:
		// A group name is a trimmed, non-empty UTF-8 string that fits in this
		// many bytes. The limit is deliberately in bytes rather than code
		// points so the value that is validated is the value that is stored
		// and written, with no second measurement to disagree with it.
		static constexpr size_t MaxNameBytes{ 63 };

		// Factory rather than a public constructor, so the only way to make a
		// group is through the World that has already validated its name.
		static std::unique_ptr<AgentGroup> create(std::string name);

		std::string const& getName() const { return mName; }

		// Surrounding whitespace is stripped before a name is judged. Only
		// ASCII space and tab are trimmed: they are what a stray keystroke
		// produces, and stripping Unicode blanks would silently rewrite the
		// user's own characters.
		static std::string trimName(std::string const& value);

		// Shape check shared by every World entry point, so creation and
		// rename refuse the same values for the same reason. `trimmed` is
		// expected to have come from trimName(); on failure a human-readable
		// `diagnostic` names the rule that was broken.
		static bool nameIsValid(std::string const& trimmed, std::string* diagnostic);
	};

} // core
