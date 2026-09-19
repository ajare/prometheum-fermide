// Subprocess smoke check for ticket #59: a graphics initialisation failure
// (unavailable video driver) must produce a controlled non-zero exit from the
// GUI application, not an uncaught-exception abort (SIGABRT / exit 134).
//
// The check launches the GUI executable with SDL_VIDEODRIVER set to a value
// that no SDL build can provide, so SDL_Init fails before any window exists.
// The GUI path is supplied through PF_GUI_EXECUTABLE by CTest; the check is
// skipped when the variable is absent (for example, GUI-less builds).

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	constexpr int StartupTimeoutMs = 30'000;
	constexpr char UnusableVideoDriver[] = "prometheum-fermide-no-such-video-driver";

	std::string environmentValue(char const* name)
	{
#ifdef _WIN32
		char* rawValue{ nullptr };
		size_t length{ 0 };
		if (_dupenv_s(&rawValue, &length, name) != 0 || rawValue == nullptr)
		{
			return {};
		}

		std::string value(rawValue);
		std::free(rawValue);
		return value;
#else
		char const* rawValue = std::getenv(name);
		return rawValue != nullptr ? std::string(rawValue) : std::string();
#endif
	}

#ifdef _WIN32
	void runGuiWithUnusableDriver(std::string const& guiExecutable)
	{
		_putenv_s("SDL_VIDEODRIVER", UnusableVideoDriver);

		STARTUPINFOA startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo{};

		std::string commandLine = "\"" + guiExecutable + "\"";
		if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo))
		{
			throw std::runtime_error("Could not launch the GUI executable: " + guiExecutable);
		}

		DWORD const waitResult = WaitForSingleObject(processInfo.hProcess,
			static_cast<DWORD>(StartupTimeoutMs));
		if (waitResult == WAIT_TIMEOUT)
		{
			TerminateProcess(processInfo.hProcess, 0);
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
			throw std::runtime_error("GUI executable did not fail fast with an unusable video driver.");
		}

		DWORD exitCode{ 0 };
		GetExitCodeProcess(processInfo.hProcess, &exitCode);
		CloseHandle(processInfo.hProcess);
		CloseHandle(processInfo.hThread);

		if (exitCode == 0)
		{
			throw std::runtime_error("GUI exited with 0 despite an unusable video driver.");
		}

		// Exit code 3 is the C runtime abort() code; 0xC0000000-range codes
		// are hard crashes. Both mean the failure bypassed the handler.
		if (exitCode == 3)
		{
			throw std::runtime_error("GUI aborted instead of exiting in a controlled way.");
		}

		if ((exitCode & 0xC0000000) == 0xC0000000)
		{
			throw std::runtime_error("GUI crashed with exit code 0x"
				+ std::to_string(exitCode) + " instead of exiting in a controlled way.");
		}
	}
#else
	void runGuiWithUnusableDriver(std::string const& guiExecutable)
	{
		pid_t const child = fork();
		if (child == -1)
		{
			throw std::runtime_error("Could not fork the GUI smoke child process.");
		}

		if (child == 0)
		{
			setenv("SDL_VIDEODRIVER", UnusableVideoDriver, 1);
			unsetenv("DISPLAY");
			unsetenv("WAYLAND_DISPLAY");
			execl(guiExecutable.c_str(), guiExecutable.c_str(), static_cast<char*>(nullptr));
			// Only reached when exec itself failed.
			_exit(127);
		}

		int status = 0;
		for (int waitedMs = 0; ; waitedMs += 100)
		{
			pid_t const result = waitpid(child, &status, WNOHANG);
			if (result == child)
			{
				break;
			}

			if (result == -1)
			{
				throw std::runtime_error("waitpid failed for the GUI smoke child process.");
			}

			if (waitedMs >= StartupTimeoutMs)
			{
				kill(child, SIGKILL);
				waitpid(child, &status, 0);
				throw std::runtime_error("GUI executable did not fail fast with an unusable video driver.");
			}

			usleep(100 * 1000);
		}

		if (WIFSIGNALED(status))
		{
			throw std::runtime_error("GUI terminated by signal "
				+ std::to_string(WTERMSIG(status))
				+ "; SIGABRT means the startup failure bypassed the handler.");
		}

		if (!WIFEXITED(status))
		{
			throw std::runtime_error("GUI did not exit normally.");
		}

		if (WEXITSTATUS(status) == 0)
		{
			throw std::runtime_error("GUI exited with 0 despite an unusable video driver.");
		}
	}
#endif
}

void runGraphicsStartupSmokeChecks()
{
	auto const guiExecutable = environmentValue("PF_GUI_EXECUTABLE");
	if (guiExecutable.empty())
	{
		std::cout << "SKIP: graphics startup smoke check (PF_GUI_EXECUTABLE is not set)\n";
		return;
	}

	runGuiWithUnusableDriver(guiExecutable);

	std::cout << "PASS: graphics startup failure exits with a controlled non-zero code\n";
}
