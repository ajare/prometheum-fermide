#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <string>

#include "core/VertexType.h"
#include "core/VertexActionType.h"
#include "core/Vector2.h"


namespace core
{
	class Edge;
	class Agent;
	class Object;
	class Sector;
	class VertexController;

	class Vertex
	{
		static uint32_t IdGenerator;

	private:

		uint32_t mId;

		VertexType mType;

		VertexSubType mSubType;

		// Reference to the Sector this Vertex is within
		std::shared_ptr<Sector> mSector;

		// Offset within the Sector, not global position.  This is used to calculate
		// the Vertex's position within the world.
		Vector2 mSectorOffset;

		std::shared_ptr<Object> mObject;

		std::vector<std::shared_ptr<const Edge>> mEdges;

		std::shared_ptr<VertexController> mController;

	protected:

		Vector2 const& getBoundsOffset() const;

		Vertex(uint32_t id, VertexType type, VertexSubType subType, std::shared_ptr<Sector> sector, float xSectorOffset, float ySectorOffset);

	public:

		Vertex(VertexType type, VertexSubType subType, std::shared_ptr<Sector> sector, float xSectorOffset, float ySectorOffset);

		virtual ~Vertex() = default;

		// Compares Vertices by their ID.
		bool sameAs(std::shared_ptr<const Vertex> other) const;

		uint32_t getId() const;

		VertexType getType() const;

		VertexSubType getSubType() const;

		virtual std::string getDescription() const = 0;

		virtual std::string getSpec() const;

		std::shared_ptr<Sector> getSector() const;

		Vector2 const& getSectorOffset() const;

		Vector2 getPosition() const;

		std::vector<std::shared_ptr<const Edge>> const& getEdges() const;

		void _setController(std::shared_ptr<VertexController> controller);

		std::shared_ptr<VertexController> getController() const;

		void setObject(std::shared_ptr<Object> object);

		std::shared_ptr<Object> getObject() const;

		void _addEdge(std::shared_ptr<const Edge> edge);

		virtual std::shared_ptr<Vertex> copyWithoutEdges() = 0;
	};

} // core
