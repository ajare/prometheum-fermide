#include "core/Defines.h"
#include "core/PlatformLiftVertex.h"
#include "core/LiftSectorObject.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	PlatformLiftVertex
	------------------

	This Vertex is a specialisation of a LiftVertex, intended for use with the PlatformLift subclass of Lift.
	*/

	PlatformLiftVertex::PlatformLiftVertex(uint32_t id, shared_ptr<Sector> sector, shared_ptr<Lift> lift, float xLocationOffset, float yLocationOffset, uint32_t stopOffset)
		: LiftVertex(id, sector, lift, xLocationOffset, yLocationOffset, stopOffset)
	{
	}

	PlatformLiftVertex::PlatformLiftVertex(shared_ptr<Sector> sector, shared_ptr<Lift> lift, float xLocationOffset, float yLocationOffset, uint32_t stopOffset)
		: LiftVertex(sector, lift, xLocationOffset, yLocationOffset, stopOffset)
	{
	}

} // core