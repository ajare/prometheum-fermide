#include <cassert>

#include "core/Defines.h"
#include "core/Window.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/***

	Window
	------

	This class represents a Window in a Location.  It may be on either Layer, and may have either another
	Location behind it, or nothing (ie looking into the void/space).

	This class is similar to Door in that it can in theory be opened, but because it has some extra states,
	due to extra styles, it needs to be implemented as a separate Shape subclass, and not an OpenableObject.

	Construction arguments:

	- cellX and cellY are global, not relative to the Location that it's in.
	- cellsWide should generally be 1, but in theory there's no reason why it can't be any value greater than zero.
	- locations[2] is the fore and back Location (see CORE_LAYER_FORE / CORE_LAYER_BACK)
	*/
	Window::Window(uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t decksHigh, shared_ptr<const Sector> sectors[2])
		: Object((float)cellX + CORE_WINDOW_X_INSET, (float)cellY + CORE_WINDOW_Y_OFFSET, cellsWide - CORE_WINDOW_X_INSET * 2.0f, (decksHigh - 1) + CORE_WINDOW_HEIGHT)
		, mCellsWide(cellsWide)
		, mDecksHigh(decksHigh)
		, mState(State::Closed)
		, mStyle(Style::Clear)
		, mSectors{ sectors[0], sectors[1] }
	{
	}

	/***

	getCellsWide()
	--------------

	Get the width of the Window, in cells.
	*/
	uint32_t Window::getCellsWide() const
	{
		return mCellsWide;
	}

	/***

	getDecksHigh()
	--------------

	Get the height of the Window, in cells.
	*/
	uint32_t Window::getDecksHigh() const
	{
		return mDecksHigh;
	}

	/***

	getState()
	----------

	Get the state of the Window.  Similar to Door, but there are some extra states
	to deal with the different styles.  For instance, moving from untinted to tinted.
	*/
	Window::State const& Window::getState() const
	{
		return mState;
	}

	/***

	getStyle()
	----------

	While Windows are generally clear, some may be tinted of frosted.
	*/
	Window::Style Window::getStyle() const
	{
		return mStyle;
	}

	/***

	getSector()
	-----------

	Get the Location, for the given Layer.
	*/
	shared_ptr<const Sector> Window::getSector(uint32_t layerIndex) const
	{
		ASSERT_LAYER_OK(layerIndex);

		return mSectors[layerIndex];
	}

	void Window::setState(State state, Style style)
	{
		mState = state;
		mStyle = style;
	}

	void Window::configureTraversal(bool enabled, TraversalResourceId resource)
	{
		mTraversalConfigured = enabled;
		mTraversalResource = enabled ? resource : TraversalResourceId{};
	}

	bool Window::isNormallyTraversable() const
	{
		return mTraversalConfigured && mState == State::Open && mStyle == Style::Clear;
	}

	std::string Window::getDescription() const
	{
		return "Window";
	}


} // core