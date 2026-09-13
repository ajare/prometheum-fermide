#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <utility>


namespace core
{
	// Building composes one registry per entity category. A registry is the sole
	// owner of its entities and allocates monotonically increasing typed IDs.
	template<typename Id, typename Entity>
	class EntityRegistry
	{
		uint64_t mNextId{ 1 };
		std::map<Id, std::unique_ptr<Entity>> mEntities;

	public:
		Id add(std::unique_ptr<Entity> entity)
		{
			Id id{ mNextId++ };
			mEntities.emplace(id, std::move(entity));
			return id;
		}

		void restore(Id id, std::unique_ptr<Entity> entity)
		{
			mEntities.emplace(id, std::move(entity));
			if (mNextId <= id.value)
			{
				mNextId = id.value + 1;
			}
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
