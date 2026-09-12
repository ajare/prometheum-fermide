#include "core/SectorPosition.h"
#include "core/Sector.h"


namespace core
{

	using namespace std;

	SectorPosition::SectorPosition()
		: SectorPosition(nullptr, Vector2::ZERO)
	{
	}
	
	SectorPosition::SectorPosition(Sector const* sector, float x, float y)
		: SectorPosition(sector, { x, y })
	{
	}

	SectorPosition::SectorPosition(Sector const* sector, Vector2 const& pos)
		: mSector(sector)
		, mPosition(pos)
	{
	}

	Sector const* SectorPosition::sector() const
	{
		return mSector;
	}

	Vector2 const& SectorPosition::local() const
	{
		return mPosition;
	}

	Vector2 SectorPosition::global() const
	{
		return mSector->getPosition() + local();
	}

} // core