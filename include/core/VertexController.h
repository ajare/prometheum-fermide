#pragma once

#include <memory>
#include <string>
#include <set>

#include "core/Vertex.h"
#include "core/Edge.h"
#include "core/Shape.h"
#include "core/SectorPosition.h"
#include "core/VertexControllerArea.h"
#include "core/VertexControllerNotificationType.h"


namespace core
{
	class Sector;
	class SectorObject;

	class VertexController
	{
	protected:

		static const uint32_t MaxAreaCount = 2;

	public:

		enum struct InterSectorExitType
		{
			None,
			X,
			Y,
		};

	private:

		SectorObject const* mwOwner;

		InterSectorExitType mInterSectorExitType;

		float mInterSectorExitOffset;

		int mExitingAgentArea;

	protected:

		std::shared_ptr<VertexControllerArea> mAreas[MaxAreaCount];

	private:

		void updateAreas(float frameTime);

		virtual void updateImpl(float frameTime);

		void onAgentExitAreaCallback(Agent* agent);

	protected:

		std::shared_ptr<VertexControllerArea> getArea(uint32_t index) const;

	public:

		VertexController(SectorObject const* owner, std::shared_ptr<VertexControllerArea> area, InterSectorExitType interSectorExitType, float interSectorExitOffset = 0.0f);

		VertexController(SectorObject const* owner, std::shared_ptr<VertexControllerArea> area0, std::shared_ptr<VertexControllerArea> area1, InterSectorExitType interSectorExitType, float interSectorExitOffset = 0.0f);

		virtual ~VertexController() = default;

		SectorObject const* getOwner() const;

		virtual std::string getDescription() const = 0;

		bool hasArea(uint32_t index) const;

		Shape const& getControlArea(uint32_t index) const;

		virtual std::vector<std::shared_ptr<VertexControllerArea>> getAreasForLayer(uint32_t layerIndex) const = 0;

		bool hasInterLayerExitArea(uint32_t index) const;

		Shape const& getInterLayerExitArea(uint32_t index) const;

		InterSectorExitType getInterSectorExitType() const;

		float getInterSectorExitOffset() const;

		virtual bool inControlArea(SectorPosition const& pos, uint32_t* index) const;

		virtual bool transitionRequiresControl(std::shared_ptr<const Vertex> fromVertex, std::shared_ptr<const Vertex> toVertex) const;

		virtual bool objectUseRequired() const = 0;

		virtual bool controllerUseRequired() const;

		virtual bool canExit(Agent* agent) const = 0;

		virtual void handleVertexAction(Agent* agent, std::shared_ptr<Vertex> vertex, VertexActionType type) = 0;

		void registerAgentForControl(Agent* agent, uint32_t index);

		void update(float frameTime);
	};

} // core

