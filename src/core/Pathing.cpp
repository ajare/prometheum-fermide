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
            Agent const* agent,
            Graph const* graph,
            node_type vertex,
            vector<PathNode>& nodes,
            unordered_map<node_type, node_type> const& cameFrom,
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
            if (!source)
            {
                source = graph->getClosestVertexInSector(agent->getSector(), agent->getGlobalPosition());
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

            return reconstructPath(agent, graph, source, target, cameFrom, costSoFar, edgeMap);
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