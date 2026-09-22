#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/EntityId.h"
#include "core/Object.h"
#include "core/Vector2.h"


namespace core
{

	class Marker : public Object
	{
		friend class Building;
		friend class MarkerSectorObject;

		MarkerId mId;
		std::string mName;
		uint32_t mCellX, mCellY;
		float mOffset;

		void setName(std::string name) { mName = std::move(name); }

		Marker(MarkerId id, std::string name, uint32_t cellX, uint32_t cellY, float xOffset);

	public:
		static constexpr size_t MaxNameBytes{ 63 };

		MarkerId getId() const { return mId; }
		MarkerId getMarkerId() const { return mId; }
		std::string const& getName() const { return mName; }

		static std::string trimName(std::string const& value);
		static bool nameIsValid(std::string const& trimmed, std::string* diagnostic);

		uint32_t getCellX() const;

		uint32_t getCellY() const;

		float getOffset() const;

		// Overridden from Object
		[[nodiscard]] std::string getDescription() const override;
	};

} // core
