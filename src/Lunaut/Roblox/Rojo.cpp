#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <algorithm>
#include <iterator>
#include <set>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr int search_depth = 4;
constexpr std::string_view project_suffix = ".project.json";

const std::set<std::string_view> skipped_folders = {
	".git",
	".vscode",
	"node_modules",
	"Packages",
	"DevPackages",
	"ServerPackages",
	"build",
	"out",
	"target",
};

struct projectchoice
{
	std::filesystem::path path;
	bool usable = false;
};

std::string quote(const std::string& value);
void collect_projects(const std::filesystem::path& folder, int depth, std::vector<std::filesystem::path>& projects);
bool is_better_project(const projectchoice& candidate, const projectchoice& best, const std::string& preferred);
std::vector<std::string> get_tree_paths(const std::filesystem::path& project);
void collect_paths(const json& node, std::vector<std::string>& paths);
bool is_usable(const std::filesystem::path& project);
void rebase_paths(json& node, const std::filesystem::path& prefix);
result<nothing> rebase_sourcemap(const std::filesystem::path& file, const std::filesystem::path& prefix);

}
}

bool rojo::is_project(const std::filesystem::path& root, const rojoinfo& info)
{
	std::error_code error;
	return find_project(root, info).has_value() || std::filesystem::exists(root / info.sourcemap_file, error);
}

std::optional<std::string> rojo::find_project(const std::filesystem::path& root, const rojoinfo& info)
{
	std::error_code error;

	if (root.empty())
	{
		return std::nullopt;
	}

	if (std::filesystem::is_regular_file(root / info.project_file, error))
	{
		return info.project_file;
	}

	std::vector<std::string> projects = find_projects(root);

	if (projects.empty())
	{
		return std::nullopt;
	}

	std::string preferred = std::filesystem::path(info.project_file).filename().string();
	std::optional<priv::projectchoice> best;

	for (const std::string& project : projects)
	{
		priv::projectchoice candidate{std::filesystem::path(project), priv::is_usable(root / project)};

		if (!best || priv::is_better_project(candidate, *best, preferred))
		{
			best = candidate;
		}
	}

	return best->path.generic_string();
}

bool rojo::has_existing_paths(const std::filesystem::path& project)
{
	return priv::is_usable(project);
}

std::vector<std::string> rojo::find_projects(const std::filesystem::path& root)
{
	std::vector<std::filesystem::path> found;
	std::vector<std::string> projects;

	if (root.empty())
	{
		return projects;
	}

	priv::collect_projects(root, 0, found);

	for (const std::filesystem::path& project : found)
	{
		projects.push_back(project.lexically_relative(root).generic_string());
	}

	std::sort(projects.begin(), projects.end());
	return projects;
}

result<nothing> rojo::generate_sourcemap(const std::filesystem::path& root, const rojoinfo& info)
{
	std::error_code error;
	std::optional<std::string> project = find_project(root, info);

	if (!project)
	{
		return result<nothing>::fail("rojo project not found: " + (root / info.project_file).generic_string());
	}

	std::filesystem::path project_path = root / *project;

	if (!priv::is_usable(project_path))
	{
		std::vector<std::string> paths = priv::get_tree_paths(project_path);

		return result<nothing>::fail(
			*project + ": no $path of the project exists next to it (\"" + paths.front() + "\" is " +
			(project_path.parent_path() / paths.front()).lexically_relative(root).generic_string() +
			"); Rojo resolves $path from the folder of the project file");
	}

	if (!process::has_executable(info.executable))
	{
		return result<nothing>::fail("rojo executable not found: " + info.executable);
	}

	std::string command = priv::quote(info.executable) + " sourcemap " + priv::quote(*project) + " --output " +
		priv::quote((root / info.sourcemap_file).generic_string());

	if (info.include_non_scripts)
	{
		command += " --include-non-scripts";
	}

	std::filesystem::path previous = std::filesystem::current_path(error);
	std::filesystem::current_path(root, error);
	result<std::string> output = process::invoke_command(command);
	std::filesystem::current_path(previous, error);

	if (!output)
	{
		return result<nothing>::fail(output.message + ": " + stdx::trim(output.value));
	}

	std::filesystem::path prefix = std::filesystem::path(*project).parent_path();

	if (prefix.empty())
	{
		return result<nothing>::ok({});
	}

	return priv::rebase_sourcemap(root / info.sourcemap_file, prefix);
}

namespace
{
namespace priv
{

void collect_projects(const std::filesystem::path& folder, int depth, std::vector<std::filesystem::path>& projects)
{
	std::error_code error;

	for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder, error))
	{
		std::string name = entry.path().filename().string();

		if (entry.is_directory(error))
		{
			if (depth < search_depth && skipped_folders.count(name) == 0)
			{
				collect_projects(entry.path(), depth + 1, projects);
			}
		}
		else if (stdx::ends_with(name, project_suffix))
		{
			projects.push_back(entry.path());
		}
	}
}

bool is_better_project(const projectchoice& candidate, const projectchoice& best, const std::string& preferred)
{
	if (candidate.usable != best.usable)
	{
		return candidate.usable;
	}

	std::ptrdiff_t depth = std::distance(candidate.path.begin(), candidate.path.end());
	std::ptrdiff_t best_depth = std::distance(best.path.begin(), best.path.end());

	if (depth != best_depth)
	{
		return depth < best_depth;
	}

	bool named = candidate.path.filename() == preferred;
	bool best_named = best.path.filename() == preferred;

	if (named != best_named)
	{
		return named;
	}

	return candidate.path.generic_string() < best.path.generic_string();
}

std::vector<std::string> get_tree_paths(const std::filesystem::path& project)
{
	std::vector<std::string> paths;
	std::optional<std::string> source = stdx::read_file(project);

	if (!source)
	{
		return paths;
	}

	json data = json::parse(*source, nullptr, false, true);

	if (data.is_object() && data.contains("tree"))
	{
		collect_paths(data["tree"], paths);
	}

	return paths;
}

void collect_paths(const json& node, std::vector<std::string>& paths)
{
	if (!node.is_object())
	{
		return;
	}

	for (const auto& [key, value] : node.items())
	{
		if (key == "$path" && value.is_string())
		{
			paths.push_back(value.get<std::string>());
		}
		else if (key == "$path" && value.is_object() && value.contains("optional") && value["optional"].is_string())
		{
			paths.push_back(value["optional"].get<std::string>());
		}
		else if (!key.empty() && key.front() != '$')
		{
			collect_paths(value, paths);
		}
	}
}

bool is_usable(const std::filesystem::path& project)
{
	std::error_code error;
	std::filesystem::path folder = project.parent_path();
	std::vector<std::string> paths = get_tree_paths(project);

	return paths.empty() ||
		std::any_of(paths.begin(), paths.end(), [&](const std::string& path) { return std::filesystem::exists(folder / path, error); });
}

void rebase_paths(json& node, const std::filesystem::path& prefix)
{
	if (node.contains("filePaths") && node["filePaths"].is_array())
	{
		for (json& path : node["filePaths"])
		{
			if (path.is_string() && !std::filesystem::path(path.get<std::string>()).is_absolute())
			{
				path = (prefix / path.get<std::string>()).lexically_normal().generic_string();
			}
		}
	}

	if (node.contains("children") && node["children"].is_array())
	{
		for (json& child : node["children"])
		{
			rebase_paths(child, prefix);
		}
	}
}

result<nothing> rebase_sourcemap(const std::filesystem::path& file, const std::filesystem::path& prefix)
{
	std::optional<std::string> source = stdx::read_file(file);

	if (!source)
	{
		return result<nothing>::fail("rojo wrote no sourcemap: " + file.generic_string());
	}

	json data = json::parse(*source, nullptr, false);

	if (data.is_discarded() || !data.is_object())
	{
		return result<nothing>::fail("rojo wrote an invalid sourcemap: " + file.generic_string());
	}

	rebase_paths(data, prefix);

	if (!stdx::write_file(file, data.dump()))
	{
		return result<nothing>::fail("cannot write sourcemap: " + file.generic_string());
	}

	return result<nothing>::ok({});
}

std::string quote(const std::string& value)
{
	if (value.find_first_of(" \t") == std::string::npos)
	{
		return value;
	}

	return "\"" + value + "\"";
}

}
}

}
