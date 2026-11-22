#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <cstdlib>
#include <iostream>

#if !LUNAUT_PLATFORM_WINDOWS
#include <sys/wait.h>
#endif

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view installer_url = "https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/install";

std::string make_windows_command();

std::string make_shell_command();

}
}

std::string updater::make_command() const
{
#if LUNAUT_PLATFORM_WINDOWS
	if (std::getenv("MSYSTEM") != nullptr && process::has_executable("sh"))
	{
		return priv::make_shell_command();
	}

	return priv::make_windows_command();
#else
	return priv::make_shell_command();
#endif
}

result<int> updater::invoke(bool dry_run) const
{
	const std::string command = make_command();
	std::cout << command << '\n';
	std::cout.flush();

	if (dry_run)
	{
		return result<int>::ok(0);
	}

	if (std::getenv("LUNAUT_OFFLINE") != nullptr)
	{
		return result<int>::fail("offline mode (LUNAUT_OFFLINE)");
	}

	const int status = std::system(command.c_str());

	if (status == -1)
	{
		return result<int>::fail("could not start the installer");
	}

#if LUNAUT_PLATFORM_WINDOWS
	return result<int>::ok(status);
#else
	if (WIFEXITED(status))
	{
		return result<int>::ok(WEXITSTATUS(status));
	}

	return result<int>::fail("installer terminated abnormally");
#endif
}

namespace
{
namespace priv
{

std::string make_windows_command()
{
	const std::string prefix = "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"$env:LUNAUT_EDITORS='path'; irm ";
	return prefix + std::string(installer_url) + " | iex\"";
}

std::string make_shell_command()
{
	return "sh -c \"export LUNAUT_EDITORS=path; curl -fsSL " + std::string(installer_url) + " | sh\"";
}

}
}

}
