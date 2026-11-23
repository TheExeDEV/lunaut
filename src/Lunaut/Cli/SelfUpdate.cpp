#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view latest_release_url = "https://api.github.com/repos/TheExeDEV/lunaut/releases/latest";
constexpr std::string_view installer_url = "https://raw.githubusercontent.com/TheExeDEV/lunaut/main/installers/install";
constexpr std::string_view editor_folder = "/extensions/";
constexpr std::string_view stale_suffix = ".old";

#if LUNAUT_PLATFORM_WINDOWS
constexpr std::string_view binary_name = "lunaut.exe";
#else
constexpr std::string_view binary_name = "lunaut";
#endif

std::string strip_version(const std::string& version);
std::vector<int> split_version(const std::string& version);
std::string make_installer_command();
bool is_editor_copy(const std::filesystem::path& executable);
void remove_stale_copies(const std::filesystem::path& executable);
result<std::filesystem::path> move_aside(const std::filesystem::path& executable);
void restore(const std::filesystem::path& executable, const std::filesystem::path& moved);
std::string quote(const std::string& value);

}
}

int selfupdate::invoke(const std::optional<std::string>& version, bool check_only, bool dry_run)
{
	std::string current = LUNAUT_VERSION;
	std::string target;

	if (version)
	{
		target = priv::strip_version(*version);
	}
	else
	{
		result<std::string> latest = get_latest_version();

		if (!latest)
		{
			std::cerr << "Cannot check for updates: " << latest.message << '\n';
			return 1;
		}

		target = latest.value;
	}

	bool newer = compare_versions(target, current) > 0;

	if (check_only)
	{
		if (newer)
		{
			std::cout << "lunaut " << target << " is available, this is " << current << ". Run lunaut update to install it.\n";
		}
		else
		{
			std::cout << "lunaut " << current << " is up to date\n";
		}

		return 0;
	}

	if (!version && !newer)
	{
		std::cout << "lunaut " << current << " is up to date\n";
		return 0;
	}

	std::optional<std::filesystem::path> executable = process::get_executable_path();

	if (!executable)
	{
		std::cerr << "Cannot find the path of this lunaut\n";
		return 1;
	}

	if (priv::is_editor_copy(*executable))
	{
		std::cerr << "This lunaut came with an editor extension (" << executable->generic_string()
				  << "), and the extension updates it. Update the extension instead.\n";
		return 1;
	}

	std::filesystem::path folder = executable->parent_path();
	std::string command = priv::make_installer_command();
	const std::vector<std::pair<std::string, std::string>> environment = {
		{"LUNAUT_VERSION", target},
		{"LUNAUT_INSTALL_DIR", folder.string()},
		{"LUNAUT_EDITORS", "path"},
		{"LUNAUT_NO_PATH", "1"},
	};

	std::cout << "Updating lunaut " << current << " to " << target << " in " << folder.generic_string() << '\n';

	if (dry_run)
	{
		for (const auto& [name, value] : environment)
		{
			std::cout << "  " << name << "=" << value << '\n';
		}

		std::cout << "  " << command << '\n';
		return 0;
	}

	if (std::getenv("LUNAUT_OFFLINE") != nullptr)
	{
		std::cerr << "Cannot update in offline mode (LUNAUT_OFFLINE)\n";
		return 1;
	}

	for (const auto& [name, value] : environment)
	{
		process::set_environment(name, value);
	}

	priv::remove_stale_copies(*executable);
	result<std::filesystem::path> moved = priv::move_aside(*executable);

	if (!moved)
	{
		std::cerr << moved.message << '\n';
		return 1;
	}

	std::cout.flush();
	int code = process::invoke_visible(command);

	if (code == -1)
	{
		priv::restore(*executable, moved.value);
		std::cerr << "Cannot start the installer: " << command << '\n';
		return 1;
	}

	if (code != 0)
	{
		priv::restore(*executable, moved.value);
		std::cerr << "The installer failed with exit code " << code << ", lunaut " << current << " is kept\n";
		return code;
	}

	if (!moved.value.empty())
	{
		std::error_code error;
		std::filesystem::remove(moved.value, error);
	}

	result<std::string> installed = process::invoke_command(priv::quote(executable->string()) + " version");
	std::cout << (installed ? stdx::trim(installed.value) : "lunaut " + target + " is installed") << '\n';
	return 0;
}

result<std::string> selfupdate::get_latest_version()
{
	result<std::string> response = http::get_text(std::string(priv::latest_release_url));

	if (!response)
	{
		return response;
	}

	json release = json::parse(response.value, nullptr, false);

	if (!release.is_object() || !release.contains("tag_name") || !release["tag_name"].is_string())
	{
		return result<std::string>::fail("GitHub did not return the latest release");
	}

	return result<std::string>::ok(priv::strip_version(release["tag_name"].get<std::string>()));
}

int selfupdate::compare_versions(const std::string& left, const std::string& right)
{
	std::vector<int> first = priv::split_version(left);
	std::vector<int> second = priv::split_version(right);
	size_t length = std::max(first.size(), second.size());

	first.resize(length, 0);
	second.resize(length, 0);

	if (first == second)
	{
		return 0;
	}

	return first < second ? -1 : 1;
}

namespace
{
namespace priv
{

std::string strip_version(const std::string& version)
{
	std::string trimmed = stdx::trim(version);

	if (!trimmed.empty() && (trimmed.front() == 'v' || trimmed.front() == 'V'))
	{
		trimmed.erase(trimmed.begin());
	}

	return trimmed;
}

std::vector<int> split_version(const std::string& version)
{
	std::vector<int> parts;

	for (const std::string& part : stdx::split(strip_version(version), '.'))
	{
		std::string digits;

		for (char character : part)
		{
			if (!std::isdigit(static_cast<unsigned char>(character)))
			{
				break;
			}

			digits.push_back(character);
		}

		parts.push_back(digits.empty() ? 0 : std::stoi(digits));
	}

	return parts;
}

std::string make_installer_command()
{
#if LUNAUT_PLATFORM_WINDOWS
	return "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"irm " + std::string(installer_url) + " | iex\"";
#else
	return "curl -fsSL " + std::string(installer_url) + " | sh";
#endif
}

bool is_editor_copy(const std::filesystem::path& executable)
{
	std::string path = executable.generic_string();
	std::transform(path.begin(), path.end(), path.begin(), [](unsigned char character) { return std::tolower(character); });

	return path.find(editor_folder) != std::string::npos;
}

void remove_stale_copies(const std::filesystem::path& executable)
{
	std::error_code error;
	std::string prefix = executable.filename().string() + std::string(stale_suffix);

	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(executable.parent_path(), error))
	{
		if (stdx::starts_with(entry.path().filename().string(), prefix))
		{
			std::filesystem::remove(entry.path(), error);
		}
	}
}

result<std::filesystem::path> move_aside(const std::filesystem::path& executable)
{
	if (executable.filename() != binary_name)
	{
		return result<std::filesystem::path>::ok({});
	}

	std::error_code error;
	std::filesystem::path moved = executable.string() + std::string(stale_suffix);

	for (int index = 2; std::filesystem::exists(moved, error); ++index)
	{
		moved = executable.string() + std::string(stale_suffix) + std::to_string(index);
	}

	std::filesystem::rename(executable, moved, error);

	if (error)
	{
		return result<std::filesystem::path>::fail("Cannot move " + executable.generic_string() + " aside: " + error.message());
	}

	return result<std::filesystem::path>::ok(moved);
}

void restore(const std::filesystem::path& executable, const std::filesystem::path& moved)
{
	std::error_code error;

	if (moved.empty() || std::filesystem::exists(executable, error))
	{
		return;
	}

	std::filesystem::rename(moved, executable, error);
}

std::string quote(const std::string& value)
{
	return "\"" + value + "\"";
}

}
}

}
