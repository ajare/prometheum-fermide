#pragma once

#include <cstdint>
#include <memory>

#include "core/Button.h"
#include "core/SectorObject.h"


namespace core
{

	enum struct ButtonAnchorType
	{
		Ground,
		UnAnchored
	};

	class ButtonSectorObject : public SectorObject
	{
		ButtonAnchorType mAnchorType;

	public:

		ButtonSectorObject(std::string const& name, uint32_t cellX, uint32_t cellY, float xOffset, float yOffset, ButtonAnchorType anchorType, std::shared_ptr<const Sector> sector, uint32_t buttonFlags, uint32_t* vertexIdentifer = nullptr);

		~ButtonSectorObject() = default;

		void _setCellPosition(uint32_t cellX, uint32_t cellY) { setCellPosition(cellX, cellY); }

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
