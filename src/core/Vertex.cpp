#include "core/Vertex.h"
#include "core/VertexType.h"
#include "core/VertexController.h"
#include "core/Edge.h"
#include "core/Sector.h"
#include "core/Pathing.h"
#include "core/Object.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	
	Vertex
	------

	This class represents a Vertex in a graph, and will be connected to one or more
	Edges.

	Vertices have both a type and a sub-type.  The type defines what kind of Sector
	they are used in, for instance a Location or a Lift.  The sub-type defines the
	type of object linked to the Vertex - eg a Lift, a ForceBridge, and so on.
	Types and sub-types are used to avoid proliferation	of subclasses.

	Most Vertices are used for path-finding by Agents moving on foot, and so their y
	position should be aligned with the floor.

	A Vertex has a few extra properties beyond that of a regular one.  Specifically it
	has an area - define as a size plus an offset from its centre - which can be used
	by an Agent to determine if it is "close enough" to a Vertex.  Height often isn't
	really needed for horizontal movement, so usually the height for Location Vertices
	is similar to Agent height.

	As Graphs may be copied, and their Edges and Vertices copied as well, we still
	need a way to know whether a Vertex in one Graph is the same as in another - ie
	topologically the same, even if its weights and other values are different.  For
	this, we use an GUID (IdGenerator), and the Vertex::sameAs() method.
	*/

	uint32_t Vertex::IdGenerator = 0;

	Vertex::Vertex(uint32_t id, VertexType type, VertexSubType subType, shared_ptr<Sector> sector, float xSectorOffset, float ySectorOffset)
		: mId(id)
		, mType(type)
		, mSubType(subType)
		, mSector(sector)
		, mSectorOffset(xSectorOffset, ySectorOffset)
	{
	}

	Vertex::Vertex(VertexType type, VertexSubType subType, shared_ptr<Sector> sector, float xSectorOffset, float ySectorOffset)
		: Vertex(IdGenerator++, type, subType, sector, xSectorOffset, ySectorOffset)
	{
	}

	/***

	sameAs()
	--------

	An Agent may have its own Graph, but that Graph will only differ in Edge weights.  Thus we need a way to
	compare Vertices in different Graphs to determine if they are the same topological Vertex.  This is done
	with this method.  We do not override the equality operators.
	*/
	bool Vertex::sameAs(shared_ptr<const Vertex> other) const
	{
		return getId() == other->getId();
	}

	/***

	getId()
	-------

	An Id is unique within a single Graph, but the same across different Graphs.
	*/
	uint32_t Vertex::getId() const
	{
		return mId;
	}

	/***

	getType()
	---------

	The type of a Vertex refers to the Sector type that it is in.
	*/
	VertexType Vertex::getType() const
	{
		return mType;
	}

	/***

	getSubType()
	------------

	The sub-type of a Vertex refers to the type of Object the Vertex is for.
	*/
	VertexSubType Vertex::getSubType() const
	{
		return mSubType;
	}

	/***

	getSpec()
	---------

	Used for display and debugging.
	*/
	string Vertex::getSpec() const
	{
		string typeStr = getVertexTypeString(mType);
		string subTypeStr = getSubVertexTypeString(mSubType);

		return format("[{}] {}/{}", mId, typeStr, subTypeStr);
	}

	/***

	getSector()
	-----------

	Get the Sector the Vertex is in.
	*/
	shared_ptr<Sector> Vertex::getSector() const
	{
		return mSector;
	}

	/***

	getSectorOffset()
	-----------------

	Get the offset in units from the Sector's origin.
	*/
	Vector2 const& Vertex::getSectorOffset() const
	{
		return mSectorOffset;
	}

	/***

	getPosition()
	-------------

	Get the global position of the Vertex, in units.
	*/
	Vector2 Vertex::getPosition() const
	{
		Vector2 cellPos{ (float)mSector->getCellX(), (float)mSector->getCellY() };
		return cellPos + getSectorOffset();
	}

	/***

	getEdges()
	---------

	Get all the Edges the Vertex is a part of.
	*/
	vector<shared_ptr<const Edge>> const& Vertex::getEdges() const
	{
		return mEdges;
	}

	shared_ptr<VertexController> Vertex::getController() const
	{
		return mController;
	}

	shared_ptr<Object> Vertex::getObject() const
	{
		return mObject;
	}

	void Vertex::setObject(std::shared_ptr<Object> object)
	{
		mObject = object;
	}

	/***

	_addEdge()
	----------

	Add an Edge to the Vertex.
	*/
	void Vertex::_addEdge(shared_ptr<const Edge> edge)
	{
		for (auto curEdge : mEdges)
		{
			if (curEdge->getId() == edge->getId())
			{
				throw GraphException(format("Vertex::_addEdge(<edge>) - Edge with ID {} already in list", edge->getId()));
			}
		}

		mEdges.push_back(edge);
	}

	void Vertex::_setController(shared_ptr<VertexController> controller)
	{
		mController = controller;
	}

} // core