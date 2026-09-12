#include "core/ControllableActionStatus.h"


namespace core
{

	using namespace std;

	string getControllableActionStatusString(ControllableActionStatus status)
	{
		switch (status)
		{
		case ControllableActionStatus::None:
			return "None";

		case ControllableActionStatus::Pending:
			return "Pending";

		case ControllableActionStatus::InProgress:
			return "InProgress";

		case ControllableActionStatus::CompletedSuccess:
			return "CompletedSuccess";

		case ControllableActionStatus::CompletedInterrupted:
			return "CompletedInterrupted";

		case ControllableActionStatus::CompletedFailure:
			return "CompletedFailure";

		case ControllableActionStatus::Rejected:
			return "Rejected";

		case ControllableActionStatus::Unhandled:
			return "Unhandled";

		default:
			return "???";
		}
	}

} // core