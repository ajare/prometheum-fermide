#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/EdgeType.h"
#include "core/EdgeTraversalRequestResult.h"
#include "core/Controller.h"
#include "core/EntityId.h"


namespace core
{
	class Location;
	class Vertex;
	class Agent;

	class Edge
	{
		friend class Graph;

	private:

		static uint32_t IdGenerator;

	private:

		uint32_t mId;

		EdgeType mType;

		std::shared_ptr<const Vertex> mVertices[2];

	private:
		
		void _setVertex(uint32_t index, std::shared_ptr<const Vertex> vertex);

	protected:

		Edge(uint32_t id, EdgeType type);

	public:

		Edge(EdgeType type);

		virtual ~Edge() = default;

		// Compares Edges by their ID.
		[[nodiscard]] bool sameAs(std::shared_ptr<const Edge> other) const;

		[[nodiscard]] uint32_t getId() const;

		[[nodiscard]] EdgeType getType() const;

		[[nodiscard]] bool isInterLayer() const;

		[[nodiscard]] std::shared_ptr<const Vertex> getVertex(uint32_t index) const;

		[[nodiscard]] std::shared_ptr<const Vertex> getOtherVertex(std::shared_ptr<const Vertex> vertex) const;

		float getLength() const;

		// To be implemented by subclasses.
		[[nodiscard]] virtual std::string getDescription() const = 0;

		// To be implemented by subclasses.
		[[nodiscard]] virtual std::shared_ptr<Edge> copyWithoutVertices() = 0;

		// To be implemented by subclasses.
		[[nodiscard]] virtual bool isTraversable(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const = 0;

		// To be implemented by subclasses.
		virtual EdgeTraversalRequestResult requestTraversal(std::shared_ptr<const Vertex> targetVertex, std::shared_ptr<const Agent> agent) const = 0 ;

		// To be implemented by subclasses.  Returned weight is in seconds.
		[[nodiscard]] virtual float getWeight(std::shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const = 0;
	
		// To be implemented by subclasses.
		[[nodiscard]] virtual std::shared_ptr<Controller> getDependingController(int side, uint32_t layerIndex) const;

		// A non-zero handle selects the replacement traversal authority. Legacy
		// edge and vertex-controller preparation must not also authorize it.
		[[nodiscard]] virtual TraversalResourceId getTraversalResourceId() const { return {}; }
	};

} // core
