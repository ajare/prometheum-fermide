#include <cassert>

#include "core/Edge.h"
#include "core/Vertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	Edge
	----

	This class represents a bi-directional, weighted edge in a graph, and is one of
	the main members of the	Graph class, along with Vertex.  It is designed to be
	subclassed to provide functionality specific to the Sector it is used in.

	An Edge has two Vertices, and a weight function, which depends on various
	external factors.  The Vertices may be in different Layers, in which case the
	transition along the Edge is usually instantaneous, once traversal conditions
	have been fulfilled.

	There are two functions used to handle movement of an Agent along an Edge:

	  - isTraversable()
	  - requestTraversal()

	isTraversable() should return whether the Edge can be traversed in its current
	state: for instance, if the Edge represents moving through a Door, is the Door
	open?  If this returns true, then an Agent can immediately start moving along
	the Edge.

	requestTraversal() is used when isTraversable() returns false: it tries to put
	the world in such a state that isTraversable() will return true.  As an example,
	if an Door Edge is not traversable, it will try and open the Door.  As such,
	Edges generally need a reference to an Object which they can control.  It returns
	an enum rather than true/false so that a more detailed failure reason can be returned
	for Agents to make decisions with.

	Weight is the time in seconds, to cross the Edge, and will depend on various
	dynamic factors, for instance the speed of a Lift.  The weight calculation is
	bi-directional and takes a target Vertex to determine direction.  There are times
	when we want to mark an Edge as untraversable.  This is done by returning a very
	large time value - CORE_GRAPH_EDGE_UNTRAVERSABLE - which can be used in two ways:
	  
	  - The Edge can be used, as a last resort if there are no other routes available
	  - The Edge effectively does not exist

	How Agents choose to interpret weights greater than or equal to this value is up to them.  
	They will have to handle the situation where they suddenly become cut off - eg the only
	Door out of a Location becomes locked from the outside.

	As Graphs may be copied, and their Edges and Vertices copied as well, we still
	need a way to know whether an Edge in one Graph is the same as in another - ie
	topologically the same, even if its weights and other values are different.  For
	this, we use an GUID (IdGenerator), and the Edge::sameAs() method.
	*/

	uint32_t Edge::IdGenerator = 0;

	Edge::Edge(uint32_t id, EdgeType type)
		: mId(id)
		, mType(type)
		, mVertices{ nullptr, nullptr }
	{
	}

	Edge::Edge(EdgeType type)
		: Edge(IdGenerator++, type)
	{
	}

	/***

	sameAs()
	--------

	An Agent may have its own Graph, but that Graph will only differ in Edge weights.  Thus we need a way to
	compare Edges in different Graphs to determine if they are the same topological Edge.  This is done
	with this method.  We do not override the equality operators.
	*/
	bool Edge::sameAs(shared_ptr<const Edge> other) const
	{
		return getId() == other->getId();
	}

	/***

	getId()
	-------

	An Id is unique within a single Graph, but the same across different Graphs.
	*/
	uint32_t Edge::getId() const
	{
		return mId;
	}

	/***

	getType()
	---------

	The type of an Edge typically refers to the object it represents.
	*/
	EdgeType Edge::getType() const
	{
		return mType;
	}

	/***

	isInterLayer()
	--------------

	Are the Vertices of the Edge on different layers?
	*/
	bool Edge::isInterLayer() const
	{
		return mVertices[0]->getSector()->getLayerIndex() != mVertices[1]->getSector()->getLayerIndex();
	}

	/***

	_setVertex()
	------------

	Set one of the Edge's Vertices.
	*/
	void Edge::_setVertex(uint32_t index, shared_ptr<const Vertex> vertex)
	{
		if (index >= 2)
		{
			throw GraphException(format("Edge::_setVertex({}, <Vertex>) - index={} out of bounds", index, index));
		}

		if (mVertices[index])
		{
			throw GraphException(format("Edge::_setVertex({}, <Vertex>) - index={} is aleady set", index, index));
		}

		mVertices[index] = vertex;
	}

	/***

	getVertex()
	-----------

	Get a given Vertex.
	*/
	shared_ptr<const Vertex> Edge::getVertex(uint32_t index) const
	{
		if (index >= 2)
		{
			throw GraphException(format("Edge::getVertex({}) - index={} out of bounds", index, index));
		}

		return mVertices[index];
	}

	/***

	getOtherVertex()
	----------------

	Given one Vertex, return the other.
	*/
	shared_ptr<const Vertex> Edge::getOtherVertex(shared_ptr<const Vertex> vertex) const
	{
		assert(mVertices[0] != nullptr);
		assert(mVertices[1] != nullptr);

		for (int i = 0; i < 2; ++i)
		{
			if (vertex->getId() == mVertices[i]->getId())
			{
				return mVertices[1 - i];
			}
		}

		throw GraphException(format("Edge::getOtherVertex(<Vertex>) - Vertex with index={} not found in Edge", vertex->getId()));
	}

	/***

	getLength()
	-----------

	Get Edge length, in units.
	*/
	float Edge::getLength() const
	{
		assert(mVertices[0] != nullptr);
		assert(mVertices[1] != nullptr);

		return mVertices[0]->getPosition().distanceTo(mVertices[1]->getPosition());
	}

	/***

	getDependingController()
	--------------------------

	Get all the Interactables which are required to be interacted with, in order to
	allow this Edge to be traversed.  For instance, a button which opens a Door.

	Some Interactables will only be required if passing through a specific direction,
	eg a Door which has buttons to open it will not let you use a button on the other
	side of the Door.
	*/
	shared_ptr<Controller> Edge::getDependingController(int side, uint32_t layerIndex) const
	{
		return nullptr;
	}

} // core