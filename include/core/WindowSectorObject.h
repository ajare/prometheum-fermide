#pragma once

#include <cstdint>
#include <memory>

#include "core/SectorObject.h"


namespace core
{
	class Window;

	class WindowSectorObject : public SectorObject
	{
	public:

		WindowSectorObject(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh, std::shared_ptr<const Sector> sectors[2], uint32_t* vertexIdentifer = nullptr);

		~WindowSectorObject() = default;

		std::shared_ptr<const Window> getWindow() const;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<VertexController> createVertexController(Building const* building, std::vector<std::shared_ptr<Vertex>> const& vertices, std::map<std::shared_ptr<Controller>, std::shared_ptr<Vertex>> const& controllerVertexLookup) const override;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
