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
		const Luau::Module& module, const textdocument& document, const inlayhintsinfo& info, unsigned int first, unsigned int last);

	bool visit(Luau::AstStatLocal* node) override;
	bool visit(Luau::AstStatForIn* node) override;
	bool visit(Luau::AstExprFunction* node) override;
	bool visit(Luau::AstExprCall* node) override;

	void add_variable(Luau::AstLocal* local, std::optional<Luau::TypeId> type);
	void add_type(Luau::Position position, const Luau::ToStringResult& text);
	bool is_visible(Luau::Position position) const;

	const Luau::Module& module;
	const textdocument& document;
	const inlayhintsinfo& info;
	unsigned int first;
	unsigned int last;
	Luau::ToStringOptions options;
	json hints = json::array();
};

bool is_literal(const Luau::AstExpr* expr);
std::string get_argument_text(const Luau::AstExpr* expr);
Luau::ToStringResult to_return_string(Luau::TypePackId pack, Luau::ToStringOptions& options);

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

	priv::hint_collector collected(*module, *document, info, first, last);
	source->root->visit(&collected);
	return collected.hints;
}

namespace
{
namespace priv
{

hint_collector::hint_collector(
	const Luau::Module& module, const textdocument& document, const inlayhintsinfo& info, unsigned int first, unsigned int last)
	: module(module)
	, document(document)
	, info(info)
	, first(first)
	, last(last)
{
	options.maxTableLength = 30;
	options.maxTypeLength = static_cast<size_t>(std::max(10, info.max_length));
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

		if (!type && i < node->values.size)
		{
			if (const Luau::TypeId* found = module.astTypes.find(node->values.data[i]))
			{
				type = *found;
			}
		}

		add_variable(local, type);
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
			add_variable(local, scope ? scope->lookup(Luau::Symbol(local)) : std::nullopt);
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
		add_type(node->argLocation->end, to_return_string(function->retTypes, options));
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

void hint_collector::add_variable(Luau::AstLocal* local, std::optional<Luau::TypeId> type)
{
	if (!type || local->name == "_" || !is_visible(local->location.end))
	{
		return;
	}

	Luau::TypeId followed = Luau::follow(*type);

	if (info.hide_error_types && Luau::get<Luau::ErrorType>(followed) != nullptr)
	{
		return;
	}

	Luau::ToStringResult text = Luau::toStringDetailed(followed, options);

	if (stdx::to_lower(text.name) == stdx::to_lower(local->name.value))
	{
		return;
	}

	add_type(local->location.end, text);
}

void hint_collector::add_type(Luau::Position position, const Luau::ToStringResult& text)
{
	if (text.name.empty() || (info.hide_error_types && text.error))
	{
		return;
	}

	json hint = {
		{"position", document.to_lsp_position(position)},
		{"label", ": " + text.name},
		{"kind", kind_type},
	};

	if (info.insertable && !text.invalid && !text.truncated && !text.error && !text.cycle)
	{
		json at = document.to_lsp_position(position);
		hint["textEdits"] = {{{"range", {{"start", at}, {"end", at}}}, {"newText", ": " + text.name}}};
	}

	hints.push_back(hint);
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

Luau::ToStringResult to_return_string(Luau::TypePackId pack, Luau::ToStringOptions& options)
{
	size_t count = Luau::size(pack);
	bool tail = !Luau::finite(pack);
	bool wrap = Luau::get<Luau::TypePack>(Luau::follow(pack)) != nullptr && (tail ? count != 0 : count > 1);
	Luau::ToStringResult text = Luau::toStringDetailed(pack, options);

	if (wrap || text.name.empty())
	{
		text.name = "(" + text.name + ")";
	}

	return text;
}

}
}

}
