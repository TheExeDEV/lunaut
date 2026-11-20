#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"
#include "Luau/AstQuery.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"
#include "Luau/TypePack.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr int kind_type = 1;
constexpr int kind_parameter = 2;

struct hint_collector final : Luau::AstVisitor
{
	hint_collector(
		workspace& owner,
		const Luau::Module& module,
		const textdocument& document,
		const inlayhintsinfo& info,
		unsigned int first,
		unsigned int last);

	bool visit(Luau::AstStatLocal* node) override;
	bool visit(Luau::AstStatForIn* node) override;
	bool visit(Luau::AstExprFunction* node) override;
	bool visit(Luau::AstExprCall* node) override;

	void add_variable(
		Luau::AstLocal* local, std::optional<Luau::TypeId> type, const Luau::ScopePtr& scope, const Luau::AstExpr* initializer = nullptr);
	void add_type(Luau::Position position, const Luau::ToStringResult& text, const json& edits = json());
	bool is_visible(Luau::Position position) const;
	bool is_annotated(Luau::Position position) const;

	workspace& owner;
	const Luau::Module& module;
	const textdocument& document;
	const inlayhintsinfo& info;
	unsigned int first;
	unsigned int last;
	Luau::ToStringOptions options;
	std::vector<size_t> line_starts;
	json hints = json::array();
};

bool is_literal(const Luau::AstExpr* expr);
std::string get_argument_text(const Luau::AstExpr* expr);
bool is_identifier(const std::string& name);
bool is_qualified_name(const std::string& name);
std::string last_name_part(const std::string& name);
std::string name_prefix(const std::string& name);
bool raw_annotation_visible(const Luau::ScopePtr& scope, const std::string& text);
std::optional<std::string> find_imported_symbol(const Luau::ScopePtr& scope, const Luau::ModuleName& module);
std::string replace_error_type(const std::string& text);
std::string shorten(const std::string& text, size_t limit);

}
}

json workspace::get_inlay_hints(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	const inlayhintsinfo& info = settings.inlay_hints;
	bool enabled =
		info.variable_types || info.parameter_types || info.function_return_types || info.parameter_names != enumitem_parameter_hints::None;

	if (document == nullptr || !enabled)
	{
		return json::array();
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, true);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, true);

	if (source == nullptr || !module || source->root == nullptr)
	{
		return json::array();
	}

	unsigned int first = params.value("/range/start/line"_json_pointer, 0u);
	unsigned int last = params.value("/range/end/line"_json_pointer, 0xFFFFFFFFu);

	priv::hint_collector collected(*this, *module, *document, info, first, last);
	source->root->visit(&collected);
	return collected.hints;
}

namespace
{
namespace priv
{

hint_collector::hint_collector(
	workspace& owner,
	const Luau::Module& module,
	const textdocument& document,
	const inlayhintsinfo& info,
	unsigned int first,
	unsigned int last)
	: owner(owner)
	, module(module)
	, document(document)
	, info(info)
	, first(first)
	, last(last)
{
	options.maxTableLength = 30;
	options.maxTypeLength = static_cast<size_t>(std::max(10, info.max_length));
	line_starts.push_back(0);

	for (size_t index = 0; index < document.get_text().size(); ++index)
	{
		if (document.get_text()[index] == '\n')
		{
			line_starts.push_back(index + 1);
		}
	}
}

bool hint_collector::visit(Luau::AstStatLocal* node)
{
	if (!info.variable_types || node->location.end.line < first || node->location.begin.line > last)
	{
		return true;
	}

	Luau::ScopePtr scope = Luau::findScopeAtPosition(module, node->location.begin);

	for (size_t i = 0; i < node->vars.size; ++i)
	{
		Luau::AstLocal* local = node->vars.data[i];

		if (local->annotation != nullptr)
		{
			continue;
		}

		if (i < node->values.size && node->values.data[i]->is<Luau::AstExprFunction>())
		{
			continue;
		}

		std::optional<Luau::TypeId> type = scope ? scope->lookup(Luau::Symbol(local)) : std::nullopt;

		if ((!type || typenormalizer::is_refined(*type)) && i < node->values.size)
		{
			if (std::optional<Luau::TypeId> found = owner.get_initializer_type(module, node->values.data[i]))
			{
				type = found;
			}
		}

		add_variable(local, type, scope, i < node->values.size ? node->values.data[i] : nullptr);
	}

	return true;
}

bool hint_collector::visit(Luau::AstStatForIn* node)
{
	if (!info.variable_types || node->location.end.line < first || node->location.begin.line > last)
	{
		return true;
	}

	Luau::ScopePtr scope = Luau::findScopeAtPosition(module, node->body->location.begin);

	for (Luau::AstLocal* local : node->vars)
	{
		if (local->annotation == nullptr)
		{
			add_variable(local, scope ? scope->lookup(Luau::Symbol(local)) : std::nullopt, scope);
		}
	}

	return true;
}

bool hint_collector::visit(Luau::AstExprFunction* node)
{
	const Luau::TypeId* type = module.astTypes.find(node);
	const Luau::FunctionType* function = type != nullptr ? Luau::get<Luau::FunctionType>(Luau::follow(*type)) : nullptr;

	if (function == nullptr)
	{
		return true;
	}

	if (info.function_return_types && node->returnAnnotation == nullptr && node->argLocation && node->body->body.size > 0 &&
		is_visible(node->argLocation->end))
	{
		Luau::ScopePtr scope = Luau::findScopeAtPosition(module, node->location.begin);
		typedisplayinfo display =
			owner.get_type_display(function->retTypes, module, scope, static_cast<size_t>(std::max(10, info.max_length)));
		Luau::ToStringResult text{display.text};
		text.truncated = display.truncated;
		add_type(node->argLocation->end, text);
	}

	if (!info.parameter_types)
	{
		return true;
	}

	auto [arguments, tail] = Luau::flatten(function->argTypes);
	size_t index = node->self != nullptr && function->hasSelf ? 1 : 0;

	for (Luau::AstLocal* argument : node->args)
	{
		if (index >= arguments.size())
		{
			break;
		}

		if (argument->annotation == nullptr && argument->name != "_" && is_visible(argument->location.end))
		{
			add_type(argument->location.end, Luau::toStringDetailed(arguments[index], options));
		}

		++index;
	}

	return true;
}

bool hint_collector::visit(Luau::AstExprCall* node)
{
	if (info.parameter_names == enumitem_parameter_hints::None || node->location.end.line < first || node->location.begin.line > last)
	{
		return true;
	}

	const Luau::TypeId* type = module.astTypes.find(node->func);
	const Luau::FunctionType* function = type != nullptr ? Luau::get<Luau::FunctionType>(Luau::follow(*type)) : nullptr;

	if (function == nullptr || function->argNames.empty())
	{
		return true;
	}

	size_t index = node->self ? 1 : 0;

	for (Luau::AstExpr* argument : node->args)
	{
		if (index >= function->argNames.size())
		{
			break;
		}

		const std::optional<Luau::FunctionArgument>& parameter = function->argNames[index++];

		if (!parameter || parameter->name.empty() || parameter->name == "_")
		{
			continue;
		}

		if (info.parameter_names == enumitem_parameter_hints::Literals && !is_literal(argument))
		{
			continue;
		}

		if (info.hide_matching_names && stdx::to_lower(get_argument_text(argument)) == stdx::to_lower(parameter->name))
		{
			continue;
		}

		hints.push_back({
			{"position", document.to_lsp_position(argument->location.begin)},
			{"label", parameter->name + ":"},
			{"kind", kind_parameter},
			{"paddingRight", true},
		});
	}

	return true;
}

void hint_collector::add_variable(
	Luau::AstLocal* local, std::optional<Luau::TypeId> type, const Luau::ScopePtr& scope, const Luau::AstExpr* initializer)
{
	if (!type || local->name == "_" || !is_visible(local->location.end) || is_annotated(local->location.end))
	{
		return;
	}

	Luau::TypeId followed = typenormalizer::strip_refinements(*type);

	if (info.hide_error_types && Luau::get<Luau::ErrorType>(followed) != nullptr)
	{
		return;
	}

	typedisplayinfo display =
		owner.get_type_display(followed, module, scope, local->annotation, initializer, static_cast<size_t>(std::max(10, info.max_length)));

	if (display.text.empty())
	{
		return;
	}

	if (!display.alias && stdx::to_lower(display.text) == stdx::to_lower(local->name.value))
	{
		return;
	}

	Luau::ToStringResult text{display.text};
	text.truncated = display.truncated;
	text.cycle = display.text.find("*CYCLE*") != std::string::npos;
	text.invalid = display.text.find("*no-refine*") != std::string::npos;
	json edits = json();

	if (local->annotation == nullptr && display.alias)
	{
		bool optional = !display.text.empty() && display.text.back() == '?';
		bool local_module = display.module && *display.module == module.name;

		if (!priv::is_qualified_name(display.name))
		{
			if (priv::raw_annotation_visible(scope, display.name))
			{
				json at = document.to_lsp_position(local->location.end);
				edits = json::array({{{"range", {{"start", at}, {"end", at}}}, {"newText", ": " + display.name + (optional ? "?" : "")}}});
			}
			else
			{
				text.invalid = true;
			}
		}
		else
		{
			std::string base = priv::last_name_part(display.name);
			std::string suffix = optional ? "?" : "";
			bool in_scope = scope != nullptr && scope->lookupType(base);

			if (display.module && !display.definition_file && !local_module && display.exported)
			{
				std::optional<std::string> imported = priv::find_imported_symbol(scope, *display.module);

				if (imported)
				{
					json at = document.to_lsp_position(local->location.end);
					edits = json::array({{{"range", {{"start", at}, {"end", at}}}, {"newText", ": " + *imported + "." + base + suffix}}});
				}
				else if (info.auto_require)
				{
					std::string prefix = priv::name_prefix(display.name);
					std::string symbol = priv::is_identifier(prefix) ? prefix : owner.get_import_symbol(*display.module);

					if (priv::is_identifier(symbol))
					{
						if (std::optional<std::string> statement = owner.get_import_statement(module.name, *display.module, symbol))
						{
							json import_at = {{"line", owner.get_import_line(module.name, document)}, {"character", 0}};
							json at = document.to_lsp_position(local->location.end);
							edits = json::array(
								{{{"range", {{"start", import_at}, {"end", import_at}}}, {"newText", *statement + "\n"}},
								 {{"range", {{"start", at}, {"end", at}}}, {"newText", ": " + symbol + "." + base + suffix}}});
						}
					}
				}

				if (edits.is_null())
				{
					text.invalid = true;
				}
			}
			else if (!in_scope && !local_module)
			{
				text.invalid = true;
			}
		}
	}

	add_type(local->location.end, text, edits);
}

void hint_collector::add_type(Luau::Position position, const Luau::ToStringResult& text, const json& edits)
{
	if (text.name.empty() || (info.hide_error_types && text.error))
	{
		return;
	}

	std::string name = owner.get_settings().features.unknown_error ? replace_error_type(text.name) : text.name;
	std::string label = shorten(name, static_cast<size_t>(std::max(10, info.max_length)));
	const bool shortened = label.size() != name.size();

	json hint = {
		{"position", document.to_lsp_position(position)},
		{"label", ": " + label},
		{"kind", kind_type},
	};

	if (shortened)
	{
		hint["tooltip"] = name;
	}

	const bool negated = name.find('~') != std::string::npos;

	if (info.insertable && !shortened && !text.invalid && !text.truncated && !text.error && !text.cycle && !negated)
	{
		if (!edits.is_null())
		{
			hint["textEdits"] = edits;
		}
		else
		{
			json at = document.to_lsp_position(position);
			hint["textEdits"] = {{{"range", {{"start", at}, {"end", at}}}, {"newText", ": " + name}}};
		}
	}

	hints.push_back(hint);
}

bool hint_collector::is_annotated(Luau::Position position) const
{
	if (position.line >= line_starts.size())
	{
		return false;
	}

	const std::string& text = document.get_text();
	size_t offset = line_starts[position.line] + position.column;

	while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t'))
	{
		++offset;
	}

	return offset < text.size() && text[offset] == ':';
}

std::string shorten(const std::string& text, size_t limit)
{
	if (text.size() <= limit)
	{
		return text;
	}

	size_t cut = limit;

	while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
	{
		--cut;
	}

	return text.substr(0, cut) + "...";
}

bool is_identifier(const std::string& name)
{
	if (name.empty() || (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_'))
	{
		return false;
	}

	return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool raw_annotation_visible(const Luau::ScopePtr& scope, const std::string& text)
{
	if (!scope)
	{
		return false;
	}

	static const std::set<std::string> keywords = {
		"any",
		"boolean",
		"false",
		"function",
		"never",
		"nil",
		"number",
		"read",
		"self",
		"string",
		"true",
		"unknown",
		"write",
	};

	size_t index = 0;

	while (index < text.size())
	{
		char current = text[index];

		if (current == '"' || current == '\'')
		{
			char quote = current;
			++index;

			while (index < text.size() && text[index] != quote)
			{
				if (text[index] == '\\')
				{
					++index;
				}

				++index;
			}

			++index;
			continue;
		}

		if (!std::isalpha(static_cast<unsigned char>(current)) && current != '_')
		{
			++index;
			continue;
		}

		size_t start = index;

		while (index < text.size() && (std::isalnum(static_cast<unsigned char>(text[index])) || text[index] == '_' || text[index] == '.'))
		{
			++index;
		}

		std::string token = text.substr(start, index - start);
		std::vector<std::string> parts = stdx::split(token, '.');

		if (parts.empty() || keywords.count(parts.front()) != 0)
		{
			continue;
		}

		if (parts.size() == 1)
		{
			if (!scope->lookupType(parts.front()))
			{
				return false;
			}
		}
		else if (!scope->lookupImportedType(parts.front(), parts[1]))
		{
			return false;
		}
	}

	return true;
}

bool is_qualified_name(const std::string& name)
{
	if (name.empty())
	{
		return false;
	}

	size_t start = 0;

	while (start <= name.size())
	{
		size_t dot = name.find('.', start);
		std::string part = dot == std::string::npos ? name.substr(start) : name.substr(start, dot - start);

		if (!is_identifier(part))
		{
			return false;
		}

		if (dot == std::string::npos)
		{
			break;
		}

		start = dot + 1;
	}

	return true;
}

std::string last_name_part(const std::string& name)
{
	size_t dot = name.rfind('.');
	return dot == std::string::npos ? name : name.substr(dot + 1);
}

std::string name_prefix(const std::string& name)
{
	size_t dot = name.rfind('.');
	return dot == std::string::npos ? std::string() : name.substr(0, dot);
}

std::optional<std::string> find_imported_symbol(const Luau::ScopePtr& scope, const Luau::ModuleName& module)
{
	for (Luau::Scope* current = scope.get(); current != nullptr; current = current->parent.get())
	{
		for (const auto& [symbol, target] : current->importedModules)
		{
			if (target == module)
			{
				return symbol;
			}
		}
	}

	return std::nullopt;
}

bool hint_collector::is_visible(Luau::Position position) const
{
	return position.line >= first && position.line <= last;
}

bool is_literal(const Luau::AstExpr* expr)
{
	return expr->is<Luau::AstExprConstantBool>() || expr->is<Luau::AstExprConstantString>() || expr->is<Luau::AstExprConstantNumber>() ||
		expr->is<Luau::AstExprConstantNil>();
}

std::string get_argument_text(const Luau::AstExpr* expr)
{
	if (const Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return local->local->name.value;
	}

	if (const Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
	{
		return global->name.value;
	}

	if (const Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		return index->index.value;
	}

	return {};
}

std::string replace_error_type(const std::string& text)
{
	constexpr std::string_view marker = "*error-type*";
	std::string result;
	size_t offset = 0;

	while (true)
	{
		size_t found = text.find(marker, offset);

		if (found == std::string::npos)
		{
			result.append(text, offset, std::string::npos);
			break;
		}

		result.append(text, offset, found - offset);
		result += "unknown";
		offset = found + marker.size();
	}

	return result;
}

}
}
}
