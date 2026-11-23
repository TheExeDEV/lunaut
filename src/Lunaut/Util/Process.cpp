#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#if LUNAUT_PLATFORM_WINDOWS
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace lunaut
{
namespace
{
namespace priv
{

#if LUNAUT_PLATFORM_WINDOWS
constexpr std::string_view lookup = "where ";
constexpr std::string_view silence = " >NUL 2>&1";
#else
constexpr std::string_view lookup = "command -v ";
constexpr std::string_view silence = " >/dev/null 2>&1";
#endif

FILE* open_pipe(const std::string& command);
int close_pipe(FILE* pipe);

}
}

result<std::string> process::invoke_command(const std::string& command)
{
	FILE* pipe = priv::open_pipe(command + " 2>&1");

	if (pipe == nullptr)
	{
		return result<std::string>::fail("failed to start: " + command);
	}

	std::string output;
	std::array<char, 4096> buffer{};

	while (true)
	{
		size_t read = std::fread(buffer.data(), 1, buffer.size(), pipe);

		if (read == 0)
		{
			break;
		}

		output.append(buffer.data(), read);
	}

	int code = priv::close_pipe(pipe);

	if (code != 0)
	{
		return result<std::string>{false, output, "command exited with code " + std::to_string(code) + ": " + command};
	}

	return result<std::string>::ok(output);
}

int process::invoke_visible(const std::string& command)
{
#if LUNAUT_PLATFORM_WINDOWS
	int length = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, nullptr, 0);
	std::wstring line(static_cast<size_t>(std::max(length, 1)), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, line.data(), length);

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION started{};

	if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &started))
	{
		return -1;
	}

	WaitForSingleObject(started.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(started.hProcess, &code);
	CloseHandle(started.hThread);
	CloseHandle(started.hProcess);
	return static_cast<int>(code);
#else
	int status = std::system(command.c_str());
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

std::optional<std::filesystem::path> process::get_executable_path()
{
#if LUNAUT_PLATFORM_WINDOWS
	std::wstring buffer(32768, L'\0');
	DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

	if (size == 0 || size >= buffer.size())
	{
		return std::nullopt;
	}

	buffer.resize(size);
	return std::filesystem::path(buffer);
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buffer(size, '\0');

	if (_NSGetExecutablePath(buffer.data(), &size) != 0)
	{
		return std::nullopt;
	}

	std::error_code error;
	std::filesystem::path resolved = std::filesystem::canonical(buffer.c_str(), error);
	return error ? std::nullopt : std::optional<std::filesystem::path>(resolved);
#else
	std::error_code error;
	std::filesystem::path resolved = std::filesystem::read_symlink("/proc/self/exe", error);
	return error ? std::nullopt : std::optional<std::filesystem::path>(resolved);
#endif
}

void process::set_environment(const std::string& name, const std::string& value)
{
#if LUNAUT_PLATFORM_WINDOWS
	_putenv_s(name.c_str(), value.c_str());
#else
	setenv(name.c_str(), value.c_str(), 1);
#endif
}

bool process::has_executable(const std::string& name)
{
	if (name.find_first_of("/\\") != std::string::npos)
	{
		std::error_code error;
		return std::filesystem::is_regular_file(name, error);
	}

	FILE* pipe = priv::open_pipe(std::string(priv::lookup) + name + std::string(priv::silence));

	if (pipe == nullptr)
	{
		return false;
	}

	return priv::close_pipe(pipe) == 0;
}

namespace
{
namespace priv
{

FILE* open_pipe(const std::string& command)
{
#if LUNAUT_PLATFORM_WINDOWS
	std::string wrapped = "\"" + command + "\"";
	return _popen(wrapped.c_str(), "rb");
#else
	return popen(command.c_str(), "r");
#endif
}

int close_pipe(FILE* pipe)
{
#if LUNAUT_PLATFORM_WINDOWS
	return _pclose(pipe);
#else
	return pclose(pipe);
#endif
}

}
}

}
