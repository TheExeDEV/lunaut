#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

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

std::string quote(const std::string& value);
void collect_projects(const std::filesystem::path& folder, int depth, std::vector<std::filesystem::path>& projects);
bool is_better_project(const std::filesystem::path& candidate, const std::filesystem::path& best, const std::string& preferred);

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

	std::vector<std::filesystem::path> projects;
	priv::collect_projects(root, 0, projects);

	if (projects.empty())
	{
		return std::nullopt;
	}

	std::string preferred = std::filesystem::path(info.project_file).filename().string();
	std::optional<std::filesystem::path> best;

	for (const std::filesystem::path& project : projects)
	{
		std::filesystem::path relative = project.lexically_relative(root);

		if (!best || priv::is_better_project(relative, *best, preferred))
		{
			best = relative;
		}
	}

	return best->generic_string();
}

result<nothing> rojo::generate_sourcemap(const std::filesystem::path& root, const rojoinfo& info)
{
	std::error_code error;
	std::optional<std::string> project = find_project(root, info);

	if (!project)
	{
		return result<nothing>::fail("rojo project not found: " + (root / info.project_file).generic_string());
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

	return result<nothing>::ok({});
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

bool is_better_project(const std::filesystem::path& candidate, const std::filesystem::path& best, const std::string& preferred)
{
	bool named = candidate.filename() == preferred;
	bool best_named = best.filename() == preferred;

	if (named != best_named)
	{
		return named;
	}

	std::ptrdiff_t depth = std::distance(candidate.begin(), candidate.end());
	std::ptrdiff_t best_depth = std::distance(best.begin(), best.end());

	if (depth != best_depth)
	{
		return depth < best_depth;
	}

	return candidate.generic_string() < best.generic_string();
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
