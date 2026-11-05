#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <array>
#include <cstdio>

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

bool process::has_executable(const std::string& name)
{
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
