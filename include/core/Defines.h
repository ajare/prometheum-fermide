#pragma once

#include <cassert>
#include <cstdint>

#define CORE_VAR_UNUSED(x)							(void)x

//
// Globals
//
#define CORE_CELL_WIDTH_PIXELS						64
#define CORE_DECK_HEIGHT_PIXELS						160
#define CORE_CELL_YX_RENDER_RATIO					((float)CORE_DECK_HEIGHT_PIXELS / CORE_CELL_WIDTH_PIXELS)

// Do not change side values as rendering and other things depends on them
#define CORE_DIM_X									0
#define CORE_DIM_Y									1

#define CORE_NUM_SIDES								2  // Left & right - middle doesn't count

#define CORE_SIDE_LEFT								0
#define CORE_SIDE_RIGHT								1
#define CORE_SIDE_MIDDLE							2

#define CORE_LEVEL_LOW								0
#define CORE_LEVEL_HIGH								1

#define CORE_MAX_LAYERS								256

namespace core
{
	inline bool isFrontMostLayer(uint32_t layer)
	{
		return layer == 0;
	}

	inline bool isBackMostLayer(uint32_t layer, uint32_t layerCount)
	{
		return layer + 1 == layerCount;
	}

	inline uint32_t layerInFront(uint32_t layer)
	{
		assert(layer > 0 && "No layer in front of the front-most layer");
		return layer - 1;
	}

	inline uint32_t layerBehind(uint32_t layer)
	{
		assert(layer + 1 < CORE_MAX_LAYERS && "No layer behind the back-most layer");
		return layer + 1;
	}
}

//
// Agents
//
#define CORE_AGENT_BASE_WALK_SPEED					0.5f
#define CORE_AGENT_BASE_CLIMB_SPEED					0.25f
#define CORE_AGENT_MAX_HEIGHT						(CORE_DOOR_HEIGHT - 0.05f)
#define CORE_AGENT_MAX_WIDTH						0.4f
#define CORE_AGENT_REACH_DIST						0.25f

// Flags

//
// Doors
//
#define CORE_DOOR_OPEN_CLOSE_TIME					2.0f
#define CORE_DOOR_STAY_OPEN_TIME					5.0f
#define CORE_DOOR_HEIGHT							(CORE_CORRIDOR_HEIGHT - 0.2f)
// Tall Doors are available only to ordinary Doors authored in Rooms. Transport
// landing Doors, Corridor Doors and Facade Doors always use the regular height.
#define CORE_DOOR_TALL_HEIGHT					0.9f
#define CORE_DOOR_X_INSET							0.1f
#define CORE_DOOR_QUEUE_STOP_WIDTH					(CORE_AGENT_MAX_WIDTH + 0.1f)
// Crossing half-width about a Door vertex's x position (ticket #97): the
// physical doorway width (cell width minus the door's x insets) minus the
// agent's width, halved. A 1-cell door yields +/-0.2; a 3-cell door +/-1.2.
#define CORE_DOOR_CROSSING_HALF_WIDTH(cellsWide)		\
	(((float)(cellsWide) - 2.0f * CORE_DOOR_X_INSET - CORE_AGENT_MAX_WIDTH) * 0.5f)

//
// Bulkhead doors
//
#define CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME			6.0f
#define CORE_BULKHEAD_DOOR_STAY_OPEN_TIME			5.0f
#define CORE_BULKHEAD_DOOR_WIDTH					0.2f
#define CORE_BULKHEAD_DOOR_BUTTON_DIST				0.5f				

//
// Force bridges
//
#define CORE_FORCEBRIDGE_EXTEND_RETRACT_TIME		1.0f
#define CORE_FORCEBRIDGE_MAX_SIZE					2

//
// Ladders
//
#define CORE_LADDER_WIDTH							0.4f
#define CORE_LADDER_AGENT_SPACING					(CORE_AGENT_MAX_HEIGHT + 0.9f)
#define CORE_LADDER_HEIGHT_OFF_GROUND				(0.3f / CORE_CELL_YX_RENDER_RATIO)
#define CORE_LADDER_HEIGHT_AT_TOP					(CORE_AGENT_MAX_HEIGHT * 0.75f)
#define CORE_LADDER_MIN_RETRACTED_LENGTH			0.2f
#define CORE_LADDER_EXTEND_RETRACT_TIME				1.2f

//
// Lifts
//
#define CORE_LIFT_DOORWAY_BORDER					CORE_DOOR_X_INSET
#define CORE_LIFT_DOORWAY_HEIGHT					CORE_DOOR_HEIGHT
#define CORE_LIFT_CAR_BORDER						0.05f
#define CORE_LIFT_CAR_HEIGHT						(CORE_LIFT_DOORWAY_HEIGHT + 0.05f)
#define CORE_LIFT_SPEED								0.5f
#define CORE_LIFT_DOOR_PAUSE_TIME					0.75f
#define CORE_PLATFORM_LIFT_STOP_DURATION			10.0f

//
// Shuttles
//
#define CORE_SHUTTLE_HEIGHT							(CORE_CORRIDOR_HEIGHT + 0.1f)
#define CORE_SHUTTLE_DOORWAY_BORDER					CORE_DOOR_X_INSET
#define CORE_SHUTTLE_DOORWAY_HEIGHT					CORE_DOOR_HEIGHT
#define CORE_SHUTTLE_CAR_WIDTH						2.0f
#define CORE_SHUTTLE_CAR_HEIGHT						(CORE_SHUTTLE_DOORWAY_HEIGHT + 0.05f)
#define CORE_SHUTTLE_SPEED							0.5f

//
// Stairwells
//
#define CORE_STAIRWELL_DOORWAY_WIDTH				(0.4f + CORE_AGENT_MAX_WIDTH)
#define CORE_STAIRWELL_DOORWAY_HEIGHT				CORE_DOOR_HEIGHT

//
// Windows
//
#define CORE_WINDOW_X_INSET							0.1f
#define CORE_WINDOW_Y_OFFSET						0.2f
#define CORE_WINDOW_HEIGHT							0.3f

//
// Buttons
//
#define CORE_BUTTON_STANDARD_HEIGHT(side)			((side) == CORE_SIDE_LEFT ? CORE_BUTTON_Y_OFFSET : (CORE_BUTTON_Y_OFFSET + 0.025f))
#define CORE_BUTTON_SIZE							0.1f
#define CORE_BUTTON_Y_OFFSET						0.25f
#define CORE_BUTTON_DISABLE_TIME					1.0f

// Flags
#define CORE_BUTTON_F_AUTO_REENABLE					0x0001

//
// Locations
//
#define CORE_CORRIDOR_HEIGHT						0.7f
#define CORE_ROOM_MIN_HEIGHT						(CORE_DOOR_HEIGHT + 0.05)
#define CORE_ROOM_MAX_HEIGHT						0.9f

//
// Graph
//
#define CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME			0.1f
#define CORE_GRAPH_EDGE_UNTRAVERSABLE				999999.0f


//
// Macros
//
#define ASSERT_INDEX_OK(index)				assert(index != ~0u && "Index is -1");
#define ASSERT_LAYER_OK(layer)					assert(layer < CORE_MAX_LAYERS && "Invalid layer")
#define ASSERT_PAIR_SIDE_OK(side)					assert((side) < 2 && "Invalid threshold pair side")
#define ASSERT_DIM_OK(dim)						assert((dim == CORE_DIM_X || dim == CORE_DIM_Y) && "Invalid dimension")
#define ASSERT_SIDE_OK(side)					assert((side == CORE_SIDE_LEFT || side == CORE_SIDE_RIGHT) && "Invalid side")
#define ASSERT_LEVEL_OK(level)					assert((level == CORE_LEVEL_LOW || level == CORE_LEVEL_HIGH) && "Invalid level");
#define ASSERT_PTR_EQ_THIS(ptr)					assert(ptr.get() == this && "shared_ptr not the same as 'this'");
#define ASSERT_CONTAINTER_INDEX(index, cont)	assert(index <= cont.size() && "container index out of bounds");

#define GENERATE_LADDER_DIMS(cx, cy, dh) \
	(float)(cx + 0.5f - (CORE_LADDER_WIDTH * 0.5f)), \
	cy + CORE_LADDER_HEIGHT_OFF_GROUND, \
	CORE_LADDER_WIDTH, \
	(float)(dh - 1) + CORE_LADDER_HEIGHT_AT_TOP - CORE_LADDER_HEIGHT_OFF_GROUND
	