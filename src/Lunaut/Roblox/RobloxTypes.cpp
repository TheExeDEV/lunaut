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

constexpr std::string_view endpoint = "https://luau-lsp.pages.dev/type-definitions/globalTypes.";

const std::vector<std::string_view> security_levels = {"None", "LocalUserSecurity", "PluginSecurity", "RobloxScriptSecurity"};

}
}

result<std::string> robloxtypes::get_definitions_path(const std::string& security_level, bool refresh)
{
	if (std::find(priv::security_levels.begin(), priv::security_levels.end(), security_level) == priv::security_levels.end())
	{
		return result<std::string>::fail("unknown security level: " + security_level);
	}

	std::string name = "globalTypes." + security_level + ".d.luau";

#if FFLAG_ROBLOX_TYPES_DOWNLOAD
	result<std::string> contents = cache::get_or_fetch(name, std::string(priv::endpoint) + security_level + ".d.luau", refresh);

	if (!contents)
	{
		return result<std::string>::fail(contents.message);
	}
#endif

	std::optional<std::filesystem::path> file = cache::get_file(name);

	if (!file)
	{
		return result<std::string>::fail("roblox definitions are not cached: " + name);
	}

	return result<std::string>::ok(uri::normalize_path(file->generic_string()));
}

}
