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

const std::vector<std::string_view> module_suffixes = {".luau", ".lua"};
const std::vector<std::string_view> hidden_suffixes = {".d.luau", ".d.lua"};
const std::vector<std::string_view> init_stems = {"init", "init.server", "init.client"};

std::optional<std::string> get_module_stem(const std::filesystem::path& path);
bool is_folder(const std::filesystem::path& path);
std::optional<std::filesystem::path> find_node_path(const std::filesystem::path& target);

}
}

requiresuggester::requiresuggester(const fileresolver& resolver)
	: resolver(resolver)
{
}

std::unique_ptr<Luau::RequireNode> requiresuggester::getNode(const Luau::ModuleName& name) const
{
	std::optional<std::string> path = resolver.get_real_path(name);

	if (!path)
	{
		return nullptr;
	}

	return std::make_unique<requirenode>(resolver, std::filesystem::path(*path), true);
}

requirenode::requirenode(const fileresolver& resolver, std::filesystem::path path, bool requirer)
	: resolver(resolver)
	, path(std::move(path))
	, requirer(requirer)
{
}

std::string requirenode::getPathComponent() const
{
	if (priv::is_folder(path))
	{
		return path.filename().string();
	}

	return priv::get_module_stem(path).value_or(path.filename().string());
}

std::unique_ptr<Luau::RequireNode> requirenode::resolvePathToNode(const std::string& required) const
{
	std::optional<std::filesystem::path> target = resolver.get_require_target(path.generic_string(), required);

	if (!target)
	{
		return nullptr;
	}

	std::optional<std::filesystem::path> found = priv::find_node_path(*target);

	if (!found)
	{
		return nullptr;
	}

	return std::make_unique<requirenode>(resolver, *found, false);
}

std::vector<std::unique_ptr<Luau::RequireNode>> requirenode::getChildren() const
{
	std::vector<std::unique_ptr<Luau::RequireNode>> children;

	if (requirer || !priv::is_folder(path))
	{
		return children;
	}

	std::error_code error;
	std::vector<std::filesystem::path> entries;

	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path, error))
	{
		entries.push_back(entry.path());
	}

	std::sort(entries.begin(), entries.end());

	for (const std::filesystem::path& entry : entries)
	{
		std::string name = entry.filename().string();

		if (stdx::starts_with(name, "."))
		{
			continue;
		}

		if (priv::is_folder(entry))
		{
			children.push_back(std::make_unique<requirenode>(resolver, entry, false));
			continue;
		}

		std::optional<std::string> stem = priv::get_module_stem(entry);

		if (!stem || std::find(priv::init_stems.begin(), priv::init_stems.end(), *stem) != priv::init_stems.end())
		{
			continue;
		}

		children.push_back(std::make_unique<requirenode>(resolver, entry, false));
	}

	return children;
}

std::vector<Luau::RequireAlias> requirenode::getAvailableAliases() const
{
	return resolver.get_aliases(priv::is_folder(path) ? path : path.parent_path());
}

namespace
{
namespace priv
{

std::optional<std::string> get_module_stem(const std::filesystem::path& path)
{
	std::string name = path.filename().string();

	for (std::string_view suffix : hidden_suffixes)
	{
		if (stdx::ends_with(name, suffix))
		{
			return std::nullopt;
		}
	}

	for (std::string_view suffix : module_suffixes)
	{
		if (stdx::ends_with(name, suffix))
		{
			return name.substr(0, name.size() - suffix.size());
		}
	}

	return std::nullopt;
}

bool is_folder(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_directory(path, error);
}

std::optional<std::filesystem::path> find_node_path(const std::filesystem::path& target)
{
	if (is_folder(target))
	{
		return target;
	}

	for (std::string_view suffix : module_suffixes)
	{
		std::filesystem::path candidate(target.generic_string() + std::string(suffix));
		std::error_code error;

		if (std::filesystem::is_regular_file(candidate, error))
		{
			return candidate;
		}
	}

	return std::nullopt;
}

}
}

}
