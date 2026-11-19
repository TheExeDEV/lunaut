#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Module.h"

#include <algorithm>

namespace lunaut
{
namespace
{
namespace priv
{

struct docsource
{
	std::string_view name;
	std::string_view url;
	bool roblox;
};

const std::vector<docsource> relevant_sources = {
	{"api-docs.luau.json", "https://luau-lsp.pages.dev/api-docs/luau-en-us.json", false},
	{"api-docs.roblox.json", "https://luau-lsp.pages.dev/api-docs/en-us.json", true},
};

const std::vector<std::pair<std::string_view, std::string_view>> relevant_tags = {
	{"@param", "Parameters"},
	{"@return", "Returns"},
	{"@error", "Errors"},
};

std::string strip_comment(std::string text);
std::vector<std::string> get_lines(const std::string& text);
std::vector<std::string> collect_comment_lines(const Luau::SourceModule& source, const std::string& text, unsigned int line);
std::string render_moonwave(const std::vector<std::string>& lines);

}
}

void documentation::load(bool roblox, const std::vector<std::filesystem::path>& files, client& owner)
{
	std::vector<std::pair<std::string, std::string>> loaded_files;

	for (const std::filesystem::path& file : files)
	{
		if (std::optional<std::string> contents = stdx::read_file(file))
		{
			loaded_files.emplace_back(file.generic_string(), *contents);
		}
		else
		{
			owner.send_log(enumitem_message_type::Warning, "documentation file not found: " + file.generic_string());
		}
	}

	for (const priv::docsource& source : priv::relevant_sources)
	{
		if (source.roblox && !roblox)
		{
			continue;
		}

		result<std::string> contents = cache::get_or_fetch(std::string(source.name), std::string(source.url), false);

		if (!contents)
		{
			owner.send_log(enumitem_message_type::Warning, "documentation unavailable: " + contents.message);
			continue;
		}

		loaded_files.emplace(loaded_files.begin(), std::string(source.name), contents.value);
	}

	for (const auto& [name, contents] : loaded_files)
	{
		try
		{
			json data = json::parse(contents);

			for (auto& [symbol, entry] : data.items())
			{
				entries[symbol] = std::move(entry);
			}

			loaded = true;
			owner.send_log(enumitem_message_type::Info, "loaded documentation " + name + " (" + std::to_string(data.size()) + " symbols)");
		}
		catch (const json::exception& error)
		{
			owner.send_log(enumitem_message_type::Warning, "malformed documentation " + name + ": " + error.what());
		}
	}
}

std::optional<std::string> documentation::get_parameter_markdown(const std::string& symbol, size_t index) const
{
	if (!entries.contains(symbol))
	{
		return std::nullopt;
	}

	const json& entry = entries[symbol];

	if (entry.contains("overloads") && entry["overloads"].is_object() && !entry["overloads"].empty())
	{
		return get_parameter_markdown(entry["overloads"].begin().value().get<std::string>(), index);
	}

	if (!entry.contains("params") || !entry["params"].is_array() || index >= entry["params"].size())
	{
		return std::nullopt;
	}

	std::string text = entry["params"][index].value("documentation", "");

	if (stdx::starts_with(text, "@") && entries.contains(text))
	{
		text = entries[text].value("documentation", "");
	}

	if (stdx::trim(text).empty())
	{
		return std::nullopt;
	}

	return text;
}

bool documentation::is_loaded() const
{
	return loaded;
}

std::optional<std::string> documentation::get_markdown(const std::string& symbol) const
{
	if (!entries.contains(symbol))
	{
		return std::nullopt;
	}

	const json& entry = entries[symbol];

	if (entry.contains("overloads") && entry["overloads"].is_object() && !entry["overloads"].empty())
	{
		std::optional<std::string> first = get_markdown(entry["overloads"].begin().value().get<std::string>());
		size_t remaining = entry["overloads"].size() - 1;

		if (!first)
		{
			return std::nullopt;
		}

		return remaining == 0 ? *first : *first + "\n\n*+" + std::to_string(remaining) + (remaining == 1 ? " overload*" : " overloads*");
	}

	std::string text = entry.value("documentation", "");

	std::string link = entry.value("learn_more_link", "");
	std::string sample = entry.value("code_sample", "");

	if (!link.empty())
	{
		text += "\n\n[Learn More](" + link + ")";
	}

	if (!sample.empty())
	{
		text += "\n\n```luau\n" + sample + "\n```";
	}

	if (stdx::trim(text).empty())
	{
		return std::nullopt;
	}

	return text;
}

std::optional<std::string> documentation::get_comment_markdown(const Luau::SourceModule& source, const std::string& text, unsigned int line)
{
	std::string rendered = priv::render_moonwave(priv::collect_comment_lines(source, text, line));

	if (stdx::trim(rendered).empty())
	{
		return std::nullopt;
	}

	return rendered;
}

std::optional<std::string> documentation::get_comment_parameter(
	const Luau::SourceModule& source, const std::string& text, unsigned int line, const std::string& name)
{
	for (const std::string& raw : priv::collect_comment_lines(source, text, line))
	{
		std::string trimmed = stdx::trim(raw);

		if (!stdx::starts_with(trimmed, "@param"))
		{
			continue;
		}

		std::string rest = stdx::trim(trimmed.substr(6));
		size_t space = rest.find(' ');

		if (space == std::string::npos || rest.substr(0, space) != name)
		{
			continue;
		}

		std::string description = stdx::trim(rest.substr(space + 1));

		if (!description.empty())
		{
			return description;
		}
	}

	return std::nullopt;
}

namespace
{
namespace priv
{

std::string strip_comment(std::string text)
{
	if (stdx::starts_with(text, "--["))
	{
		size_t open = text.find('[', 3);
		std::string level = open == std::string::npos ? "" : text.substr(3, open - 3);
		text = open == std::string::npos ? text.substr(3) : text.substr(open + 1);
		std::string close = "]" + level + "]";

		if (stdx::ends_with(text, close))
		{
			text.erase(text.size() - close.size());
		}

		return text;
	}

	size_t dashes = text.find_first_not_of('-');
	text = dashes == std::string::npos ? "" : text.substr(dashes);

	if (stdx::starts_with(text, " "))
	{
		text.erase(0, 1);
	}

	return text;
}

std::vector<std::string> get_lines(const std::string& text)
{
	std::vector<std::string> lines = stdx::split(text, '\n');

	for (std::string& line : lines)
	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
	}

	return lines;
}

std::vector<std::string> collect_comment_lines(const Luau::SourceModule& source, const std::string& text, unsigned int line)
{
	if (line == 0)
	{
		return {};
	}

	std::vector<std::string> file = get_lines(text);
	std::vector<const Luau::Comment*> attached;
	unsigned int expected = line - 1;

	for (auto it = source.commentLocations.rbegin(); it != source.commentLocations.rend(); ++it)
	{
		if (it->location.end.line > expected)
		{
			continue;
		}

		if (it->location.end.line != expected)
		{
			break;
		}

		attached.push_back(&*it);

		if (it->location.begin.line == 0)
		{
			break;
		}

		expected = it->location.begin.line - 1;
	}

	if (attached.empty())
	{
		return {};
	}

	std::reverse(attached.begin(), attached.end());
	std::vector<std::string> lines;

	for (const Luau::Comment* comment : attached)
	{
		std::string body;

		for (unsigned int index = comment->location.begin.line; index <= comment->location.end.line && index < file.size(); ++index)
		{
			std::string current = file[index];
			size_t begin = index == comment->location.begin.line ? comment->location.begin.column : 0;
			size_t end =
				index == comment->location.end.line ? std::min<size_t>(comment->location.end.column, current.size()) : current.size();
			body += current.substr(std::min(begin, current.size()), end > begin ? end - begin : 0);
			body += index == comment->location.end.line ? "" : "\n";
		}

		if (stdx::starts_with(body, "--!"))
		{
			continue;
		}

		for (const std::string& piece : stdx::split(strip_comment(body), '\n'))
		{
			lines.push_back(piece);
		}
	}

	return lines;
}

std::string render_moonwave(const std::vector<std::string>& lines)
{
	std::string body;
	std::map<std::string_view, std::string> sections;
	size_t indent = std::string::npos;

	for (const std::string& line : lines)
	{
		if (!stdx::trim(line).empty())
		{
			indent = std::min(indent, line.find_first_not_of(" \t"));
		}
	}

	for (const std::string& raw : lines)
	{
		std::string line = indent == std::string::npos || raw.size() < indent ? stdx::trim(raw) : raw.substr(indent);
		std::string trimmed = stdx::trim(line);
		bool handled = false;

		for (const auto& [tag, title] : relevant_tags)
		{
			if (stdx::starts_with(trimmed, tag) && (trimmed.size() == tag.size() || trimmed[tag.size()] == ' '))
			{
				std::string rest = stdx::trim(trimmed.substr(tag.size()));
				size_t space = rest.find(' ');
				bool named = tag == "@param" && space != std::string::npos;
				sections[title] += named ? "\n- `" + rest.substr(0, space) + "` " + stdx::trim(rest.substr(space + 1)) : "\n- " + rest;
				handled = true;
			}
		}

		if (handled)
		{
			continue;
		}

		if (stdx::starts_with(trimmed, "@"))
		{
			size_t space = trimmed.find(' ');
			std::string tag = trimmed.substr(1, space == std::string::npos ? std::string::npos : space - 1);
			std::string rest = space == std::string::npos ? "" : stdx::trim(trimmed.substr(space + 1));

			if (tag == "deprecated" || tag == "yields" || tag == "server" || tag == "client" || tag == "since" || tag == "readonly")
			{
				std::string label = tag;
				label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
				body += "\n**" + label + "**" + (rest.empty() ? "" : " " + rest) + "\n";
			}

			continue;
		}

		body += line + "\n";
	}

	for (const auto& [tag, title] : relevant_tags)
	{
		const auto found = sections.find(title);

		if (found != sections.end())
		{
			body += "\n**" + std::string(title) + "**" + found->second + "\n";
		}
	}

	std::string compact;
	size_t blank = 0;

	for (const std::string& line : stdx::split(body, '\n'))
	{
		blank = stdx::trim(line).empty() ? blank + 1 : 0;

		if (blank <= 1)
		{
			compact += line + "\n";
		}
	}

	return stdx::trim(compact);
}

}
}

}
