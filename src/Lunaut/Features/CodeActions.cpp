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

const std::set<std::string_view> relevant_unused = {"LocalUnused", "FunctionUnused", "ImportUnused"};
const std::set<std::string_view> relevant_errors = {"TypeError", "SyntaxError"};
constexpr std::string_view unknown_global = "Unknown global '";

std::string get_range_text(const textdocument& document, const json& range);
json make_action(const std::string& title, const json& diagnostic, const std::string& address, const json& edits, bool preferred);

}
}

json workspace::get_code_actions(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json actions = json::array();

	if (document == nullptr)
	{
		return actions;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	std::set<std::string> disabled;

	for (const json& diagnostic : params.value("/context/diagnostics"_json_pointer, json::array()))
	{
		std::string code = diagnostic.contains("code") && diagnostic["code"].is_string() ? diagnostic["code"].get<std::string>() : "";
		std::string message = diagnostic.value("message", "");
		const json& range = diagnostic["range"];

		if (priv::relevant_unused.count(code) != 0)
		{
			std::string text = priv::get_range_text(*document, range);

			if (!text.empty() &&
				text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos &&
				text[0] != '_')
			{
				actions.push_back(
					priv::make_action(
						"Rename to '_" + text + "' to silence " + code,
						diagnostic,
						address,
						json::array({{{"range", range}, {"newText", "_" + text}}}),
						true));
			}
		}

		if (!code.empty() && priv::relevant_errors.count(code) == 0 && disabled.insert(code).second)
		{
			json start = {{"line", 0}, {"character", 0}};
			actions.push_back(
				priv::make_action(
					"Disable " + code + " in this file",
					diagnostic,
					address,
					json::array({{{"range", {{"start", start}, {"end", start}}}, {"newText", "--!nolint " + code + "\n"}}}),
					false));
		}

		if (code == "TypeError" && stdx::starts_with(message, priv::unknown_global))
		{
			size_t end = message.find('\'', priv::unknown_global.size());
			std::string symbol = message.substr(
				priv::unknown_global.size(), end == std::string::npos ? std::string::npos : end - priv::unknown_global.size());

			if (std::optional<json> edit = make_import_edit(name, *document, symbol))
			{
				std::string statement = stdx::trim((*edit)["newText"].get<std::string>());
				actions.push_back(priv::make_action("Add '" + statement + "'", diagnostic, address, json::array({*edit}), true));
			}
		}
	}

	return actions;
}

namespace
{
namespace priv
{

std::string get_range_text(const textdocument& document, const json& range)
{
	Luau::Position begin = document.to_luau_position(range["start"]);
	Luau::Position end = document.to_luau_position(range["end"]);

	if (begin.line != end.line || end.column <= begin.column)
	{
		return {};
	}

	std::vector<std::string> lines = stdx::split(document.get_text(), '\n');

	if (begin.line >= lines.size())
	{
		return {};
	}

	const std::string& line = lines[begin.line];
	return begin.column < line.size() ? line.substr(begin.column, std::min<size_t>(end.column, line.size()) - begin.column) : "";
}

json make_action(const std::string& title, const json& diagnostic, const std::string& address, const json& edits, bool preferred)
{
	return {
		{"title", title},
		{"kind", "quickfix"},
		{"diagnostics", json::array({diagnostic})},
		{"isPreferred", preferred},
		{"edit", {{"changes", {{address, edits}}}}},
	};
}

}
}

}
