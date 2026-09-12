#pragma once

#include "core/Defines.h"
#include "core/VertexController.h"


namespace core
{

	class BulkheadDoorVertexController : public VertexController
	{
		std::shared_ptr<Vertex> mVertices[CORE_NUM_SIDES];

	public:

		BulkheadDoorVertexController(SectorObject const* owner, Shape controlArea[2]);

		~BulkheadDoorVertexController() = default;

		// Overridden from VertexController
		std::string getDescription() const override;

		// Overridden from VertexController
		std::vector<std::shared_ptr<VertexControllerArea>> getAreasForLayer(uint32_t layerIndex) const override;

		// Overridden from VertexController
		bool inControlArea(SectorPosition const& pos, uint32_t* index) const override;

		// Overridden from VertexController
		bool objectUseRequired() const override;

		// Overridden from VertexController
		bool canExit(Agent* agent) const;

		void setVertexForSide(int side, std::shared_ptr<Vertex> vertex);

		// Overridden from VertexController
		void handleVertexAction(Agent* agent, std::shared_ptr<Vertex> vertex, VertexActionType type) override;
	};

} // core

