#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"
#include "Luau/AstQuery.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"

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
json make_refactor(const std::string& title, const std::string& kind, const std::string& address, const json& edits);
bool is_import(const Luau::AstStat* statement, bool& service, std::string& name);
std::optional<json> make_sorted_imports(const textdocument& document, const Luau::SourceModule& source);
std::optional<json>
make_annotation(const Luau::Module& module, const Luau::SourceModule& source, const textdocument& document, Luau::Position cursor);
std::string get_relative_require(const std::string& from, const std::string& target);

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

	check_module(name, false);
	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (source == nullptr || source->root == nullptr)
	{
		return actions;
	}

	if (std::optional<json> sorted = priv::make_sorted_imports(*document, *source))
	{
		actions.push_back(priv::make_refactor("Sort imports", "source.organizeImports", address, json::array({*sorted})));
	}

	Luau::Position cursor = document->to_luau_position(params["range"]["start"]);

	if (module)
	{
		if (std::optional<json> annotation = priv::make_annotation(*module, *source, *document, cursor))
		{
			std::string type = stdx::trim((*annotation)["newText"].get<std::string>());
			actions.push_back(
				priv::make_refactor("Add type annotation '" + type + "'", "refactor.rewrite", address, json::array({*annotation})));
		}
	}

	const auto node = frontend.sourceNodes.find(name);

	if (!sourcemap::is_virtual_path(name) || node == frontend.sourceNodes.end())
	{
		return actions;
	}

	for (Luau::AstNode* ancestor : Luau::findAstAncestryOfPosition(*source, cursor))
	{
		Luau::AstExprCall* call = ancestor->as<Luau::AstExprCall>();
		Luau::AstExprGlobal* global = call != nullptr ? call->func->as<Luau::AstExprGlobal>() : nullptr;

		if (global == nullptr || global->name != "require" || call->args.size != 1 || call->args.data[0]->is<Luau::AstExprConstantString>())
		{
			continue;
		}

		for (const auto& [required, location] : node->second->requireLocations)
		{
			if (location == call->location && sourcemap::is_virtual_path(required))
			{
				std::string path = priv::get_relative_require(name, required);
				json edit = {{"range", document->to_lsp_range(call->args.data[0]->location)}, {"newText", "\"" + path + "\""}};
				actions.push_back(
					priv::make_refactor("Use string require \"" + path + "\"", "refactor.rewrite", address, json::array({edit})));
			}
		}

		break;
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

json make_refactor(const std::string& title, const std::string& kind, const std::string& address, const json& edits)
{
	return {{"title", title}, {"kind", kind}, {"edit", {{"changes", {{address, edits}}}}}};
}

bool is_import(const Luau::AstStat* statement, bool& service, std::string& name)
{
	const Luau::AstStatLocal* local = statement->as<Luau::AstStatLocal>();

	if (local == nullptr || local->vars.size != 1 || local->values.size != 1)
	{
		return false;
	}

	const Luau::AstExprCall* call = local->values.data[0]->as<Luau::AstExprCall>();

	if (call == nullptr)
	{
		return false;
	}

	name = local->vars.data[0]->name.value;

	if (const Luau::AstExprGlobal* global = call->func->as<Luau::AstExprGlobal>())
	{
		service = false;
		return global->name == "require";
	}

	if (const Luau::AstExprIndexName* index = call->func->as<Luau::AstExprIndexName>())
	{
		service = true;
		return index->index == "GetService";
	}

	return false;
}

std::optional<json> make_sorted_imports(const textdocument& document, const Luau::SourceModule& source)
{
	struct import_line
	{
		bool service;
		std::string name;
		std::string text;
	};

	std::vector<std::string> lines = stdx::split(document.get_text(), '\n');
	std::vector<import_line> imports;
	std::optional<unsigned int> first;
	unsigned int last = 0;

	for (const Luau::AstStat* statement : source.root->body)
	{
		bool service = false;
		std::string name;

		if (!is_import(statement, service, name) || statement->location.begin.line != statement->location.end.line)
		{
			break;
		}

		unsigned int line = statement->location.begin.line;

		if (first && line != last + 1)
		{
			break;
		}

		if (line >= lines.size())
		{
			break;
		}

		first = first.value_or(line);
		last = line;
		std::string content = lines[line];

		if (!content.empty() && content.back() == '\r')
		{
			content.pop_back();
		}

		imports.push_back({service, name, content});
	}

	if (imports.size() < 2)
	{
		return std::nullopt;
	}

	std::vector<import_line> sorted = imports;
	std::stable_sort(
		sorted.begin(),
		sorted.end(),
		[](const import_line& a, const import_line& b)
		{
			if (a.service != b.service)
			{
				return a.service;
			}

			return stdx::to_lower(a.name) < stdx::to_lower(b.name);
		});

	bool same = std::equal(
		sorted.begin(), sorted.end(), imports.begin(), [](const import_line& a, const import_line& b) { return a.text == b.text; });

	if (same)
	{
		return std::nullopt;
	}

	std::string replacement;

	for (const import_line& item : sorted)
	{
		replacement += item.text + "\n";
	}

	json start = {{"line", *first}, {"character", 0}};
	json end = {{"line", last + 1}, {"character", 0}};
	return json{{"range", {{"start", start}, {"end", end}}}, {"newText", replacement}};
}

std::optional<json>
make_annotation(const Luau::Module& module, const Luau::SourceModule& source, const textdocument& document, Luau::Position cursor)
{
	Luau::AstLocal* local = Luau::findExprOrLocalAtPosition(source, cursor).getLocal();

	if (local == nullptr || local->annotation != nullptr || !local->location.containsClosed(cursor))
	{
		return std::nullopt;
	}

	Luau::ScopePtr scope = Luau::findScopeAtPosition(module, local->location.begin);
	std::optional<Luau::TypeId> type = scope ? scope->lookup(Luau::Symbol(local)) : std::nullopt;

	if (!type)
	{
		return std::nullopt;
	}

	Luau::ToStringOptions options;
	options.exhaustive = true;
	Luau::ToStringResult text = Luau::toStringDetailed(Luau::follow(*type), options);

	if (text.name.empty() || text.invalid || text.error || text.cycle || text.truncated || text.name.find('*') != std::string::npos)
	{
		return std::nullopt;
	}

	json at = document.to_lsp_position(local->location.end);
	return json{{"range", {{"start", at}, {"end", at}}}, {"newText", ": " + text.name}};
}

std::string get_relative_require(const std::string& from, const std::string& target)
{
	std::vector<std::string> base = stdx::split(from, '/');
	std::vector<std::string> parts = stdx::split(target, '/');
	base.pop_back();

	size_t common = 0;

	while (common < base.size() && common < parts.size() && base[common] == parts[common])
	{
		++common;
	}

	if (common <= 1 && parts.size() > 1 && parts.front() == "game")
	{
		std::string absolute = "@game";

		for (size_t i = 1; i < parts.size(); ++i)
		{
			absolute += "/" + parts[i];
		}

		return absolute;
	}

	std::string path;

	for (size_t i = common; i < base.size(); ++i)
	{
		path += "../";
	}

	if (path.empty())
	{
		path = "./";
	}

	for (size_t i = common; i < parts.size(); ++i)
	{
		path += parts[i] + (i + 1 < parts.size() ? "/" : "");
	}

	return path;
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
