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

const std::vector<std::string> skipped_directories = {".git", ".vscode", "node_modules", "build", "out"};

bool match_segment(std::string_view pattern, std::string_view value);
bool match_parts(const std::vector<std::string>& pattern, size_t pi, const std::vector<std::string>& path, size_t si);

}
}

namespace stdx
{

bool has_wildcard(std::string_view pattern)
{
	return pattern.find_first_of("*?[") != std::string_view::npos;
}

bool glob_match(std::string_view pattern, std::string_view path)
{
	auto pattern_parts = split(pattern, '/');
	auto path_parts = split(path, '/');

	if (!pattern_parts.empty() && pattern_parts.front().empty())
	{
		pattern_parts.erase(pattern_parts.begin());
	}

	if (pattern.find('/') == std::string_view::npos)
	{
		pattern_parts.insert(pattern_parts.begin(), "**");
	}

	return priv::match_parts(pattern_parts, 0, path_parts, 0);
}

std::vector<std::filesystem::path>
find_files(const std::filesystem::path& root, std::string_view pattern, const std::vector<std::string>& ignore_globs)
{
	std::vector<std::filesystem::path> found;
	std::error_code error;

	if (!std::filesystem::is_directory(root, error))
	{
		return found;
	}

	auto iterator = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
	const auto end = std::filesystem::recursive_directory_iterator();

	for (; iterator != end; iterator.increment(error))
	{
		if (error)
		{
			break;
		}

		const auto& entry = *iterator;
		const auto name = entry.path().filename().string();

		if (entry.is_directory(error))
		{
			const bool skipped =
				std::find(priv::skipped_directories.begin(), priv::skipped_directories.end(), name) != priv::skipped_directories.end();
			if (skipped)
			{
				iterator.disable_recursion_pending();
			}
			continue;
		}

		const auto relative = uri::get_relative(root, entry.path().generic_string());

		bool ignored = false;
		for (const auto& ignore : ignore_globs)
		{
			ignored = ignored || glob_match(ignore, relative);
		}

		if (!ignored && glob_match(pattern, relative))
		{
			found.push_back(entry.path());
		}
	}

	std::sort(found.begin(), found.end());
	return found;
}

}

namespace
{
namespace priv
{

bool match_segment(std::string_view pattern, std::string_view value)
{
	size_t p = 0;
	size_t v = 0;
	size_t star = std::string_view::npos;
	size_t mark = 0;

	while (v < value.size())
	{
		if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v]))
		{
			++p;
			++v;
		}
		else if (p < pattern.size() && pattern[p] == '*')
		{
			star = p++;
			mark = v;
		}
		else if (star != std::string_view::npos)
		{
			p = star + 1;
			v = ++mark;
		}
		else
		{
			return false;
		}
	}

	while (p < pattern.size() && pattern[p] == '*')
	{
		++p;
	}

	return p == pattern.size();
}

bool match_parts(const std::vector<std::string>& pattern, size_t pi, const std::vector<std::string>& path, size_t si)
{
	if (pi == pattern.size())
	{
		return si == path.size();
	}

	if (pattern[pi] == "**")
	{
		for (size_t next = si; next <= path.size(); ++next)
		{
			if (match_parts(pattern, pi + 1, path, next))
			{
				return true;
			}
		}

		return false;
	}

	if (si == path.size())
	{
		return false;
	}

	return match_segment(pattern[pi], path[si]) && match_parts(pattern, pi + 1, path, si + 1);
}

}
}

}
