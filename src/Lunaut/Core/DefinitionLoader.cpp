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

constexpr std::string_view priority_directive = "--!priority";
constexpr std::string_view discovery_pattern = "**/*.d.luau";
constexpr size_t directive_lines = 32;

struct collector
{
	std::vector<definitioninfo> items;
	std::set<std::string> seen;

	void add(definitioninfo info, const std::filesystem::path& file);
};

std::pair<std::filesystem::path, std::string> split_pattern(const std::filesystem::path& root, const std::string& pattern);
std::filesystem::path resolve_path(const std::filesystem::path& root, const std::string& path);

}
}

std::vector<definitioninfo>
definitionloader::collect(const std::filesystem::path& root, const settingsinfo& settings, bool roblox, client& owner)
{
	priv::collector collected;

	if (roblox && settings.roblox.download_types)
	{
		result<std::string> path = robloxtypes::get_definitions_path(settings.roblox.security_level, false);

		if (path)
		{
			collected.add(
				definitioninfo{path.value, "roblox", settings.roblox.types_priority, false, enumitem_definition_source::Roblox},
				path.value);
		}
		else
		{
			owner.send_window_message(enumitem_message_type::Warning, "Roblox definitions unavailable: " + path.message);
		}
	}

	for (const definitioninfo& definition : settings.definitions)
	{
		if (!stdx::has_wildcard(definition.path))
		{
			std::filesystem::path file = priv::resolve_path(root, definition.path);
			std::error_code error;

			if (!std::filesystem::exists(file, error))
			{
				owner.send_window_message(enumitem_message_type::Error, "definitions file not found: " + file.generic_string());
				continue;
			}

			collected.add(definition, file);
			continue;
		}

		auto [base, pattern] = priv::split_pattern(root, definition.path);

		for (const std::filesystem::path& file : stdx::find_files(base, pattern, settings.ignore_globs))
		{
			definitioninfo matched = definition;
			matched.name = "";
			collected.add(matched, file);
		}
	}

#if FFLAG_DEFINITIONS_DISCOVERY
	if (settings.discover_definitions)
	{
		for (const std::filesystem::path& file : stdx::find_files(root, priv::discovery_pattern, settings.ignore_globs))
		{
			collected.add(definitioninfo{file.generic_string(), "", std::nullopt, false, enumitem_definition_source::Discovered}, file);
		}
	}
#endif

	std::stable_sort(
		collected.items.begin(),
		collected.items.end(),
		[](const definitioninfo& a, const definitioninfo& b) { return a.priority.value_or(0) > b.priority.value_or(0); });

	return collected.items;
}

std::optional<int> definitionloader::get_directive_priority(const std::string& source)
{
	std::vector<std::string> lines = stdx::split(source, '\n');
	size_t limit = std::min(lines.size(), priv::directive_lines);

	for (size_t i = 0; i < limit; ++i)
	{
		std::string line = stdx::trim(lines[i]);

		if (line.empty())
		{
			continue;
		}

		if (!stdx::starts_with(line, "--"))
		{
			break;
		}

		if (!stdx::starts_with(line, priv::priority_directive))
		{
			continue;
		}

		try
		{
			return std::stoi(stdx::trim(line.substr(priv::priority_directive.size())));
		}
		catch (const std::exception&)
		{
			return std::nullopt;
		}
	}

	return std::nullopt;
}

std::string definitionloader::get_package_name(const definitioninfo& info, const std::filesystem::path& path)
{
	std::string name = info.name;

	if (name.empty())
	{
		name = path.filename().string();

		for (std::string_view suffix : {".d.luau", ".d.lua", ".luau", ".lua"})
		{
			if (stdx::ends_with(name, suffix))
			{
				name.erase(name.size() - suffix.size());
				break;
			}
		}
	}

	if (!stdx::starts_with(name, "@"))
	{
		name.insert(name.begin(), '@');
	}

	return name;
}

namespace
{
namespace priv
{

void collector::add(definitioninfo info, const std::filesystem::path& file)
{
	std::error_code error;
	std::filesystem::path absolute = std::filesystem::absolute(file, error);
	std::string normalized = uri::normalize_path((error ? file : absolute).generic_string());

	if (!seen.insert(normalized).second)
	{
		return;
	}

	info.path = normalized;

	if (!info.priority)
	{
		if (std::optional<std::string> contents = stdx::read_file(file))
		{
			info.priority = definitionloader::get_directive_priority(*contents);
		}
	}

	items.push_back(info);
}

std::pair<std::filesystem::path, std::string> split_pattern(const std::filesystem::path& root, const std::string& pattern)
{
	std::vector<std::string> parts = stdx::split(uri::normalize_path(pattern), '/');
	std::filesystem::path base;
	size_t index = 0;

	for (; index < parts.size() && !stdx::has_wildcard(parts[index]); ++index)
	{
		base /= parts[index].empty() ? "/" : parts[index];
	}

	std::string rest;

	for (size_t i = index; i < parts.size(); ++i)
	{
		rest += (rest.empty() ? "" : "/") + parts[i];
	}

	return {resolve_path(root, base.generic_string()), "/" + rest};
}

std::filesystem::path resolve_path(const std::filesystem::path& root, const std::string& path)
{
	std::filesystem::path candidate(path);

	if (candidate.is_absolute())
	{
		return candidate;
	}

	return root / candidate;
}

}
}

}
