#pragma once

#include "imgui/imgui.h"


struct MouseButtonStatus
{
	enum Button
	{
		Left,
		Right
	};

	enum class State
	{
		Unavailable,
		Clicked,
		Released,
		Down
	};

	State state[2];
	bool dragging[2];
	ImVec2 startDragPos[2];
	ImVec2 dragDelta[2];

	bool modCtrl, modShift, modAlt;
};
