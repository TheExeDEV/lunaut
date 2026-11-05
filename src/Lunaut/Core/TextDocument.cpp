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

size_t get_sequence_length(unsigned char lead);
size_t to_byte_column(std::string_view line, size_t column);
size_t to_lsp_column(std::string_view line, size_t column);

}
}

textdocument::textdocument(std::string address, std::string text, int version)
	: address(std::move(address))
	, text(std::move(text))
	, version(version)
{
	rebuild_lines();
}

const std::string& textdocument::get_uri() const
{
	return address;
}

const std::string& textdocument::get_text() const
{
	return text;
}

int textdocument::get_version() const
{
	return version;
}

void textdocument::apply_changes(const json& changes, int next_version)
{
	for (const auto& change : changes)
	{
		const std::string replacement = change.value("text", "");

		if (!change.contains("range"))
		{
			text = replacement;
			rebuild_lines();
			continue;
		}

		const size_t start = get_offset(change["range"]["start"]);
		const size_t end = std::max(start, get_offset(change["range"]["end"]));

		text.replace(start, end - start, replacement);
		rebuild_lines();
	}

	version = next_version;
}

Luau::Position textdocument::to_luau_position(const json& position) const
{
	const size_t line = position.value("line", 0u);
	const size_t character = position.value("character", 0u);

	if (line >= lines.size())
	{
		return Luau::Position{static_cast<unsigned int>(line), static_cast<unsigned int>(character)};
	}

	const size_t start = lines[line];
	const size_t finish = line + 1 < lines.size() ? lines[line + 1] : text.size();
	const std::string_view content(text.data() + start, finish - start);

	return Luau::Position{static_cast<unsigned int>(line), static_cast<unsigned int>(priv::to_byte_column(content, character))};
}

json textdocument::to_lsp_position(const Luau::Position& position) const
{
	if (position.line >= lines.size())
	{
		return {{"line", position.line}, {"character", position.column}};
	}

	const size_t start = lines[position.line];
	const size_t finish = position.line + 1 < lines.size() ? lines[position.line + 1] : text.size();
	const std::string_view content(text.data() + start, finish - start);

	return {{"line", position.line}, {"character", priv::to_lsp_column(content, position.column)}};
}

json textdocument::to_lsp_range(const Luau::Location& location) const
{
	return {{"start", to_lsp_position(location.begin)}, {"end", to_lsp_position(location.end)}};
}

size_t textdocument::get_offset(const json& position) const
{
	const Luau::Position converted = to_luau_position(position);

	if (converted.line >= lines.size())
	{
		return text.size();
	}

	return std::min(text.size(), lines[converted.line] + converted.column);
}

void textdocument::rebuild_lines()
{
	lines.clear();
	lines.push_back(0);

	for (size_t i = 0; i < text.size(); ++i)
	{
		if (text[i] == '\n')
		{
			lines.push_back(i + 1);
		}
	}
}

namespace
{
namespace priv
{

size_t get_sequence_length(unsigned char lead)
{
	if (lead < 0x80)
	{
		return 1;
	}
	if ((lead >> 5) == 0x6)
	{
		return 2;
	}
	if ((lead >> 4) == 0xE)
	{
		return 3;
	}
	if ((lead >> 3) == 0x1E)
	{
		return 4;
	}
	return 1;
}

size_t to_byte_column(std::string_view line, size_t column)
{
#if CORE_TEXTDOCUMENT_UTF16
	size_t bytes = 0;
	size_t units = 0;

	while (bytes < line.size() && units < column && line[bytes] != '\n' && line[bytes] != '\r')
	{
		const size_t length = get_sequence_length(static_cast<unsigned char>(line[bytes]));
		units += length == 4 ? 2 : 1;
		bytes += length;
	}

	return std::min(bytes, line.size());
#else
	return std::min(column, line.size());
#endif
}

size_t to_lsp_column(std::string_view line, size_t column)
{
#if CORE_TEXTDOCUMENT_UTF16
	size_t bytes = 0;
	size_t units = 0;

	while (bytes < line.size() && bytes < column)
	{
		const size_t length = get_sequence_length(static_cast<unsigned char>(line[bytes]));
		units += length == 4 ? 2 : 1;
		bytes += length;
	}

	return units + (column > bytes ? column - bytes : 0);
#else
	return column;
#endif
}

}
}

}
