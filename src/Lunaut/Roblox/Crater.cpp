#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <algorithm>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view declare_file = ".crater-declare";
constexpr std::string_view default_settings_file = "crater.settings.json";

const std::vector<std::pair<std::string_view, std::string_view>> relevant_realm_folders = {
	{"client", "Packages"},
	{"server", "ServerPackages"},
	{"develop", "DevPackages"},
};

std::optional<json> read_json(const std::filesystem::path& path);
std::string get_string(const json& data, const std::string& key, std::string fallback);
std::string get_realm_folder(const json& settings, std::string_view realm, std::string_view fallback);
void add_folder(std::vector<std::string>& folders, std::string folder);

}
}

std::vector<std::string> crater::get_package_folders(const std::filesystem::path& root)
{
	std::vector<std::string> folders;
	std::optional<json> declared = priv::read_json(root / std::string(priv::declare_file));

	if (!declared || !declared->is_object())
	{
		return folders;
	}

	const json packages = declared->value("/sync/packages"_json_pointer, json::object());

	if (packages.is_object())
	{
		for (const auto& [folder, place] : packages.items())
		{
			priv::add_folder(folders, folder);
		}
	}

	const json vendor = declared->value("/vendor/folders"_json_pointer, json::object());

	if (vendor.is_object())
	{
		for (const auto& [realm, folder] : vendor.items())
		{
			if (folder.is_object())
			{
				priv::add_folder(folders, priv::get_string(folder, "path", ""));
			}
		}
	}

	std::string settings_file = priv::get_string(*declared, "path_to_crater_settings", std::string(priv::default_settings_file));
	json settings = priv::read_json(root / settings_file).value_or(json::object());

	for (const auto& [realm, fallback] : priv::relevant_realm_folders)
	{
		priv::add_folder(folders, priv::get_realm_folder(settings, realm, fallback));
	}

	return folders;
}

std::vector<std::string> crater::get_ignore_globs(const std::filesystem::path& root)
{
	std::vector<std::string> globs;

	for (const std::string& folder : get_package_folders(root))
	{
		globs.push_back(folder + "/**");
	}

	return globs;
}

namespace
{
namespace priv
{

std::optional<json> read_json(const std::filesystem::path& path)
{
	std::optional<std::string> text = stdx::read_file(path);

	if (!text)
	{
		return std::nullopt;
	}

	json data = json::parse(*text, nullptr, false, true);

	if (data.is_discarded())
	{
		return std::nullopt;
	}

	return data;
}

std::string get_string(const json& data, const std::string& key, std::string fallback)
{
	if (!data.is_object() || !data.contains(key) || !data[key].is_string())
	{
		return fallback;
	}

	return data[key].get<std::string>();
}

std::string get_realm_folder(const json& settings, std::string_view realm, std::string_view fallback)
{
	const std::string key(realm);

	if (settings.is_object() && settings.contains(key))
	{
		const json& value = settings[key];

		if (value.is_string())
		{
			return value.get<std::string>();
		}

		if (value.is_object())
		{
			return get_string(value, "path", "");
		}
	}

	return get_string(settings, "import_" + key + "_path", std::string(fallback));
}

void add_folder(std::vector<std::string>& folders, std::string folder)
{
	std::replace(folder.begin(), folder.end(), '\\', '/');

	while (stdx::starts_with(folder, "./"))
	{
		folder.erase(0, 2);
	}

	while (!folder.empty() && folder.back() == '/')
	{
		folder.pop_back();
	}

	if (folder.empty() || folder.front() == '/' || folder.find(':') != std::string::npos || folder.find("..") != std::string::npos)
	{
		return;
	}

	if (std::find(folders.begin(), folders.end(), folder) == folders.end())
	{
		folders.push_back(folder);
	}
}

}
}

}
