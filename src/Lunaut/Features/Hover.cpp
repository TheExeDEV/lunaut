#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"

namespace lunaut
{
namespace
{
namespace priv
{

std::optional<Luau::TypeId> find_local_type(const Luau::Module& module, Luau::AstLocal* local, Luau::Position position);
std::string get_expression_name(Luau::AstExpr* expr);
std::string describe(const std::string& name, Luau::TypeId type, std::string_view keyword);

}
}

json workspace::get_hover(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr)
	{
		return nullptr;
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, true);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, true);

	if (source == nullptr || !module || Luau::isWithinComment(*source, position))
	{
		return nullptr;
	}

	Luau::ExprOrLocal target = Luau::findExprOrLocalAtPosition(*source, position);
	std::optional<Luau::TypeId> type;
	std::string label;
	Luau::Location range;
	std::string_view keyword;

	if (Luau::AstLocal* local = target.getLocal())
	{
		type = priv::find_local_type(*module, local, position);
		label = local->name.value;
		range = local->location;
		keyword = local->isConst ? "const " : "local ";
	}
	else if (Luau::AstExpr* expr = target.getExpr())
	{
		if (const Luau::TypeId* found = module->astTypes.find(expr))
		{
			type = *found;
		}

		label = priv::get_expression_name(expr);
		range = expr->location;

		if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
		{
			keyword = local->local->isConst ? "const " : "local ";
		}
	}

	if (!type)
	{
		return nullptr;
	}

	return {
		{"contents", protocol::make_markdown("```luau\n" + priv::describe(label, *type, keyword) + "\n```")},
		{"range", document->to_lsp_range(range)},
	};
}

namespace
{
namespace priv
{

std::optional<Luau::TypeId> find_local_type(const Luau::Module& module, Luau::AstLocal* local, Luau::Position position)
{
	for (Luau::ScopePtr scope = Luau::findScopeAtPosition(module, position); scope; scope = scope->parent)
	{
		const auto found = scope->bindings.find(Luau::Symbol(local));

		if (found != scope->bindings.end())
		{
			return found->second.typeId;
		}
	}

	for (const auto& [location, scope] : module.scopes)
	{
		const auto found = scope->bindings.find(Luau::Symbol(local));

		if (found != scope->bindings.end())
		{
			return found->second.typeId;
		}
	}

	return std::nullopt;
}

std::string get_expression_name(Luau::AstExpr* expr)
{
	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return local->local->name.value;
	}

	if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
	{
		return global->name.value;
	}

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		return index->index.value;
	}

	return {};
}

std::string describe(const std::string& name, Luau::TypeId type, std::string_view keyword)
{
	Luau::ToStringOptions options;
	options.functionTypeArguments = true;
	options.useLineBreaks = false;

	Luau::TypeId followed = Luau::follow(type);

	if (const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(followed))
	{
		return "function " + Luau::toStringNamedFunction(name.empty() ? "_" : name, *function, options);
	}

	std::string text = Luau::toString(followed, options);

	if (name.empty())
	{
		return text;
	}

	return std::string(keyword) + name + ": " + text;
}

}
}

}
