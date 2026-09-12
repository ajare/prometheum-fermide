#pragma once

#include "core/VertexController.h"


namespace core
{

	class WindowVertexController : public VertexController
	{
		std::shared_ptr<Vertex> mVertex;

	public:

		WindowVertexController(SectorObject const* owner, Shape controlArea);

		~WindowVertexController() = default;

		// Overridden from VertexController
		std::string getDescription() const override;

		// Overridden from VertexController
		bool objectUseRequired() const override;

		// Overridden from VertexController
		bool canExit(Agent* agent) const;

		// Overridden from VertexController
		std::vector<std::shared_ptr<VertexControllerArea>> getAreasForLayer(uint32_t layerIndex) const override;

		void setVertex(std::shared_ptr<Vertex> vertex);

		// Overridden from VertexController
		void handleVertexAction(Agent* agent, std::shared_ptr<Vertex> vertex, VertexActionType type) override;
	};

} // core

