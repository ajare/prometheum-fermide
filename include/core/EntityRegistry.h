#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>


namespace core
{
	// Building composes one registry per entity category. A registry is the sole
	// owner of its entities and allocates monotonically increasing typed IDs.
	//
	// Zero is the null handle (see EntityId.h), so the allocator never issues
	// it and never wraps back to it: once the top of the range has been handed
	// out, the registry is exhausted and every later allocation is refused
	// rather than handing out a falsey ID that no caller can use (#123).
	template<typename Id, typename Entity>
	class EntityRegistry
	{
		static constexpr uint64_t kTopOfRange{ std::numeric_limits<uint64_t>::max() };

		// The next ID to issue. 0 is not an ID: it is this registry's marker
		// that the range is spent and nothing more may be allocated.
		uint64_t mNextId{ 1 };
		std::map<Id, std::unique_ptr<Entity>> mEntities;

		// Push the allocator past an identity that arrived from outside - a
		// loaded document, a carried-over entity - so that identity is never
		// issued to somebody else. At the top of the range the push lands on
		// the exhausted marker instead of wrapping around to the null handle.
		void advancePast(uint64_t issued)
		{
			if (mNextId == 0 || issued < mNextId) return;
			mNextId = issued >= kTopOfRange ? 0 : issued + 1;
		}

	public:
		// The ID the next allocation would hand out, or 0 when there is no
		// such ID left. This is the allocator's high-water mark, and the value
		// a save has to persist so a reload cannot fall back behind it.
		uint64_t nextId() const
		{
			return mNextId;
		}

		bool exhausted() const
		{
			return mNextId == 0;
		}

		// Allocate, or report that nothing can be allocated. Prefer this over
		// add() wherever the caller can carry on and refuse the user's action
		// instead of unwinding through a half-finished operation.
		std::optional<Id> tryAdd(std::unique_ptr<Entity> entity)
		{
			if (exhausted()) return std::nullopt;

			Id id{ mNextId };
			advancePast(id.value);
			mEntities.emplace(id, std::move(entity));
			return id;
		}

		Id add(std::unique_ptr<Entity> entity)
		{
			auto const id = tryAdd(std::move(entity));
			if (!id)
			{
				throw std::overflow_error("Entity ID space is exhausted");
			}
			return *id;
		}

		// Take in an entity under an identity this registry did not allocate.
		// The allocator moves past it, so a restored identity is never issued
		// again. The null ID and an identity already held are refused having
		// changed nothing.
		bool restore(Id id, std::unique_ptr<Entity> entity)
		{
			if (!id) return false;
			if (!mEntities.emplace(id, std::move(entity)).second) return false;

			advancePast(id.value);
			return true;
		}

		// Adopt a persisted high-water mark: the next ID the writer would have
		// issued, or 0 to adopt its exhausted state. It has to sit above every
		// identity this registry already holds, or the document contradicts
		// itself - allocating from it would collide with an entity the same
		// document defines. A value that fails is refused having changed
		// nothing, leaving the caller free to reject the whole load.
		bool restoreNextId(uint64_t next)
		{
			if (!mEntities.empty() && next != 0 && next <= mEntities.rbegin()->first.value)
			{
				return false;
			}
			mNextId = next;
			return true;
		}

		Entity* find(Id id)
		{
			auto found = mEntities.find(id);
			return found == mEntities.end() ? nullptr : found->second.get();
		}

		Entity const* find(Id id) const
		{
			auto found = mEntities.find(id);
			return found == mEntities.end() ? nullptr : found->second.get();
		}

		bool remove(Id id)
		{
			return mEntities.erase(id) == 1;
		}

		std::map<Id, std::unique_ptr<Entity>> const& entries() const
		{
			return mEntities;
		}

		std::map<Id, std::unique_ptr<Entity>>& entries()
		{
			return mEntities;
		}
	};

} // core
