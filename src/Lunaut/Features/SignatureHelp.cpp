#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"
#include "Luau/ToString.h"
#include "Luau/TypePack.h"

namespace lunaut
{
namespace
{
namespace priv
{

Luau::AstExprCall* find_call(const Luau::SourceModule& source, Luau::Position position);
std::string get_callee_name(Luau::AstExpr* expr);

}
}

json workspace::get_signature_help(const json& params)
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

	if (source == nullptr || !module)
	{
		return nullptr;
	}

	Luau::AstExprCall* call = priv::find_call(*source, position);

	if (call == nullptr)
	{
		return nullptr;
	}

	const Luau::TypeId* callee = module->astTypes.find(call->func);

	if (callee == nullptr)
	{
		return nullptr;
	}

	const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(Luau::follow(*callee));

	if (function == nullptr)
	{
		return nullptr;
	}

	Luau::ToStringOptions options;
	options.functionTypeArguments = true;
	options.hideFunctionSelfArgument = call->self;

	std::string label = Luau::toStringNamedFunction(priv::get_callee_name(call->func), *function, options);
	auto [arguments, tail] = Luau::flatten(function->argTypes);
	size_t skip = call->self && function->hasSelf ? 1 : 0;
	json parameters = json::array();

	for (size_t i = skip; i < arguments.size(); ++i)
	{
		std::string parameter = Luau::toString(arguments[i], options);

		if (i < function->argNames.size() && function->argNames[i])
		{
			parameter = function->argNames[i]->name + ": " + parameter;
		}

		parameters.push_back({{"label", parameter}});
	}

	size_t active = 0;

	for (Luau::AstExpr* argument : call->args)
	{
		if (argument->location.end < position)
		{
			++active;
		}
	}

	return {
		{"signatures", {{{"label", label}, {"parameters", parameters}}}},
		{"activeSignature", 0},
		{"activeParameter", parameters.empty() ? 0 : std::min(active, parameters.size() - 1)},
	};
}

namespace
{
namespace priv
{

Luau::AstExprCall* find_call(const Luau::SourceModule& source, Luau::Position position)
{
	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(source, position);

	for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
	{
		Luau::AstExprCall* call = (*it)->as<Luau::AstExprCall>();

		if (call != nullptr && call->argLocation.containsClosed(position))
		{
			return call;
		}
	}

	return nullptr;
}

std::string get_callee_name(Luau::AstExpr* expr)
{
	if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
	{
		return global->name.value;
	}

	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return local->local->name.value;
	}

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		return index->index.value;
	}

	return "function";
}

}
}

}
