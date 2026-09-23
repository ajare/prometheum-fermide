#pragma once

#include <cstdint>
#include <memory>

#include "core/Lift.h"
#include "core/SectorObject.h"


namespace core
{
	enum struct LiftSectorObjectType
	{
		PlatformLift
	};

	class LiftSectorObject : public SectorObject, public VerticalEdgeCreator
	{
		LiftSectorObjectType mType;

	public:

		LiftSectorObject(LiftSectorObjectType type, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, std::vector<uint32_t> const& stopOffsets, std::shared_ptr<const Sector> sector, uint32_t* vertexIdentifer = nullptr);

		~LiftSectorObject() = default;

		std::shared_ptr<Lift> getLift() const;

		// Overridden from VerticalEdgeCreator
		[[nodiscard]] std::shared_ptr<Edge> createCrossLevelEdge(std::shared_ptr<VerticalEdgeCreator> edgeCreator) const override;

		// Overridden from SectorObject
		[[nodiscard]] std::shared_ptr<Vertex> createVertex(std::shared_ptr<SectorObject> object, std::shared_ptr<Sector> sector, void* user = nullptr) const override;
	};

} // core
