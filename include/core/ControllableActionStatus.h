#pragma once

#include <string>

namespace core
{

	// Order here matters: it is the severity of the status
	enum struct ControllableActionStatus
	{
		None,
		Pending,
		Paused,
		InProgress,
		CompletedSuccess,
		CompletedInterrupted,
		CompletedFailure,
		Rejected,
		Unhandled,
	};

	std::string getControllableActionStatusString(ControllableActionStatus status);

} // core

