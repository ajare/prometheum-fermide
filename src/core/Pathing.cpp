#include "core/Defines.h"
#include "core/Pathing.h"
#include "core/Exceptions.h"


namespace std 
{
    template<> 
    struct hash<shared_ptr<const core::Vertex>>
    {
        std::size_t operator()(const shared_ptr<const core::Vertex>& vertex) const noexcept
        {
            return vertex->getId();
        }
    };
}

namespace core
{
    namespace pathing
    {

        using namespace std;

        typedef shared_ptr<const Vertex> node_type;

        float vertexHeuristic(Agent const* agent, node_type source, node_type target)
        {
            // Returning zero is equivalent to Dijkstra
            //return 0.0f;

            auto const& pos0 = source->getPosition();
            auto const& pos1 = target->getPosition();

            auto dist = (fabs(pos1.x - pos0.x) + fabs(pos1.y - pos0.y));
            return dist / agent->getWalkSpeed();
        }

        void addVertexToPath(
            Agent const* /* agent */,
            Graph const* /* graph */,
            node_type vertex,
            vector<PathNode>& nodes,
            unordered_map<node_type, node_type> const& /* cameFrom */,
            unordered_map<node_type, float> const& costSoFar,
            unordered_map<node_type, shared_ptr<const Edge>> const& edgeMap)
        {
            // We are moving in reverse from the last point in the Path, so things get a bit confusing.
            // vertex: the current Vertex we are processing.  From the perspective
            //     of this function, it is the "source" Vertex of "edge".
            // nextVertex: the other Vertex in "edge", in reality the source Vertex when moving along the
            //     Edge the correct way.
            auto edge = edgeMap.at(vertex);
            auto cost = costSoFar.at(vertex);
            auto nextVertex = edge->getOtherVertex(vertex);

            // As we are moving through the route in reverse, add the Vertex first, before
            // seeing if anything needs to go before it.
            nodes.push_back({ edge, vertex, cost });

        }

        shared_ptr<Path> reconstructPath(
            Agent const* agent,
            Graph const* graph,
            node_type source,
            node_type target,
            unordered_map<node_type, node_type> const& cameFrom,
            unordered_map<node_type, float> const& costSoFar,
            unordered_map<node_type, shared_ptr<const Edge>> const& edgeMap)
        {
            vector<PathNode> nodes;

            auto curVertex = target;

            if (cameFrom.find(target) == cameFrom.end())
            {
                // No path can be found
                return nullptr;
            }

            while (!curVertex->sameAs(source))
            {
                addVertexToPath(agent, graph, curVertex, nodes, cameFrom, costSoFar, edgeMap);

                curVertex = cameFrom.at(curVertex);
            }

            // Add source node last
            nodes.push_back({ nullptr, source, 0.0f });

            reverse(nodes.begin(), nodes.end());

            return make_shared<Path>(nodes);
        }

        shared_ptr<Path> findPath(Agent const* agent, Graph const* graph, node_type source, node_type target)
        {
            unordered_map<node_type, node_type> cameFrom;
            unordered_map<node_type, shared_ptr<const Edge>> edgeMap;
            unordered_map<node_type, float> costSoFar;
            PriorityQueue<node_type, float> frontier;

            // Look for first vertex if we need to
            auto const inferredSource = !source;
            if (inferredSource)
            {
                // A Sector the Graph serves no vertices for - an isolated
                // Location with no traversable threshold - offers no route at
                // all. That is an ordinary "no path" outcome, not a
                // pathfinding error, so report it the same way as any other
                // unreachable target: no Path.
                try
                {
                    source = graph->getClosestVertexInSector(agent->getSector(), agent->getGlobalPosition());
                }
                catch (GraphException const&)
                {
                    return nullptr;
                }
            }

            frontier.put(source, 0.0f);

            cameFrom[source] = source;
            costSoFar[source] = 0.0f;

            while (!frontier.empty())
            {
                auto curVertex = frontier.get();

                if (curVertex->sameAs(target))
                {
                    break;
                }

                auto const& edges = curVertex->getEdges();

                for (auto const& edge : edges)
                {
                    auto nextVertex = edge->getOtherVertex(curVertex);

                    auto edgeCost = edge->getWeight(nextVertex, agent, true);
                    if (!isfinite(edgeCost))
                    {
                        continue; // A conditional resource has no reachable preparation control.
                    }
                    auto newCost = costSoFar[curVertex] + edgeCost;

                    if (costSoFar.find(nextVertex) == costSoFar.end() || newCost < costSoFar[nextVertex])
                    {
                        costSoFar[nextVertex] = newCost;
                        auto priority = newCost + vertexHeuristic(agent, nextVertex, target);
                        frontier.put(nextVertex, priority);
                        cameFrom[nextVertex] = curVertex;
                        edgeMap[nextVertex] = edge;
                    }
                }
            }

            auto path = reconstructPath(agent, graph, source, target, cameFrom, costSoFar, edgeMap);

            // The nearest vertex can be behind the agent even though the route immediately
            // continues toward a vertex in front of it. Since movement within a sector is
            // unconstrained, start at the second vertex rather than making the agent double back.
            if (inferredSource && path && path->nodes.size() >= 2)
            {
                auto const& agentPosition = agent->getGlobalPosition();
                auto const& firstVertex = path->nodes[0].targetVertex;
                auto const& secondVertex = path->nodes[1].targetVertex;
                auto const firstDirection = firstVertex->getPosition() - agentPosition;
                auto const secondDirection = secondVertex->getPosition() - agentPosition;
                auto const directionsAreOpposite =
                    firstDirection.x * secondDirection.x + firstDirection.y * secondDirection.y < 0.0f;

                if (firstVertex->getSector() == secondVertex->getSector() && directionsAreOpposite)
                {
                    auto const skippedCost = path->nodes[1].edgeWeight;
                    path->nodes.erase(path->nodes.begin());
                    path->nodes[0].edge = nullptr;
                    for (auto& node : path->nodes)
                    {
                        node.edgeWeight -= skippedCost;
                    }
                }
            }

            return path;
        }

        shared_ptr<const Vertex> findNextVertexForVertexInPath(shared_ptr<Path> path, Vertex const* vertex, uint32_t index)
        {
            for (; index < (uint32_t)path->nodes.size(); index++)
            {
                auto const& node = path->nodes[index];

                if (vertex->getId() == node.edge->getOtherVertex(node.targetVertex)->getId())
                {
                    return node.targetVertex;
                }
            }

            return nullptr;
        }

    } // pathing
} // core