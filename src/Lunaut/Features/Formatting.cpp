#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <atomic>
#include <stdexcept>

namespace lunaut
{
namespace
{
namespace priv
{

std::atomic<unsigned int> counter{0};

std::string quote(const std::string& value);
size_t get_offset(const std::string& text, const textdocument& document, const json& position);

}
}

json workspace::get_formatting(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr || !settings.features.formatting)
	{
		return json::array();
	}

	std::string path = uri::to_path(address);
	std::filesystem::path input = std::filesystem::temp_directory_path() / ("lunaut-format-" + std::to_string(priv::counter++) + ".luau");

	if (!stdx::write_file(input, document->get_text()))
	{
		throw std::runtime_error("cannot write a temporary file for StyLua");
	}

	std::filesystem::path directory = std::filesystem::path(path).parent_path();
	std::string command = std::string(LUNAUT_PLATFORM_WINDOWS ? "cd /d " : "cd ") + priv::quote(directory.generic_string()) + " && " +
		priv::quote(settings.features.stylua) + " --search-parent-directories --stdin-filepath " + priv::quote(path);

	if (params.contains("range"))
	{
		size_t start = priv::get_offset(document->get_text(), *document, params["range"]["start"]);
		size_t end = priv::get_offset(document->get_text(), *document, params["range"]["end"]);
		command += " --range-start " + std::to_string(start) + " --range-end " + std::to_string(end);
	}

	command += " - < " + priv::quote(input.generic_string());
	result<std::string> formatted = process::invoke_command(command);

	std::error_code error;
	std::filesystem::remove(input, error);

	if (!formatted)
	{
		throw std::runtime_error("StyLua failed (" + settings.features.stylua + "): " + stdx::trim(formatted.value));
	}

	if (formatted.value == document->get_text())
	{
		return json::array();
	}

	unsigned int last_line = 0;

	for (char c : document->get_text())
	{
		last_line += c == '\n' ? 1 : 0;
	}

	json start = {{"line", 0}, {"character", 0}};
	json end = {{"line", last_line + 1}, {"character", 0}};
	return json::array({{{"range", {{"start", start}, {"end", end}}}, {"newText", formatted.value}}});
}

namespace
{
namespace priv
{

std::string quote(const std::string& value)
{
	return "\"" + value + "\"";
}

size_t get_offset(const std::string& text, const textdocument& document, const json& position)
{
	Luau::Position converted = document.to_luau_position(position);
	size_t offset = 0;
	unsigned int line = 0;

	while (line < converted.line && offset < text.size())
	{
		if (text[offset++] == '\n')
		{
			++line;
		}
	}

	return std::min(text.size(), offset + converted.column);
}

}
}

}
