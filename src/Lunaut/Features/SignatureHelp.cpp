#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"
#include "Luau/PrettyPrinter.h"
#include "Luau/ToString.h"
#include "Luau/TypePack.h"

namespace lunaut
{
namespace
{
namespace priv
{

Luau::AstExprCall* find_call(const Luau::SourceModule& source, Luau::Position position);

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

	std::string signature = Luau::toStringNamedFunction("", *function, options);

	for (size_t found = signature.find("_: "); found != std::string::npos; found = signature.find("_: ", found))
	{
		signature.erase(found, 3);
	}

	std::string label = "function " + stdx::trim(Luau::toString(call->func)) + signature;
	Luau::AstExprIndexName* index = call->func->as<Luau::AstExprIndexName>();
	Luau::Position anchor = index != nullptr ? index->indexLocation.begin : call->func->location.begin;
	std::optional<std::string> symbol =
		settings.features.documentation ? Luau::getDocumentationSymbolAtPosition(*source, *module, anchor) : std::nullopt;
	auto [arguments, tail] = Luau::flatten(function->argTypes);
	size_t skip = call->self && function->hasSelf ? 1 : 0;
	json parameters = json::array();

	std::optional<std::pair<Luau::ModuleName, Luau::Location>> definition;
	const Luau::SourceModule* definition_source = nullptr;
	std::optional<std::string> definition_text;

	if (function->definition && function->definition->definitionModuleName)
	{
		definition = std::make_pair(*function->definition->definitionModuleName, function->definition->definitionLocation);
		definition_source = frontend.getSourceModule(definition->first);

		if (definition_source != nullptr)
		{
			if (std::optional<textdocument> text = get_text_for(definition->first))
			{
				definition_text = text->get_text();
			}
		}
	}

	for (size_t i = skip; i < arguments.size(); ++i)
	{
		std::string parameter = Luau::toString(arguments[i], options);

		if (i < function->argNames.size() && function->argNames[i])
		{
			parameter = function->argNames[i]->name + ": " + parameter;
		}

		json entry = {{"label", parameter}};

		if (symbol)
		{
			if (std::optional<std::string> notes = docs.get_parameter_markdown(*symbol, i))
			{
				entry["documentation"] = protocol::make_markdown(*notes);
			}
		}

		if (!entry.contains("documentation") && definition_source && definition_text && i < function->argNames.size() &&
			function->argNames[i])
		{
			if (std::optional<std::string> notes = documentation::get_comment_parameter(
					*definition_source, *definition_text, definition->second.begin.line, function->argNames[i]->name))
			{
				entry["documentation"] = protocol::make_markdown(*notes);
			}
		}

		parameters.push_back(entry);
	}

	size_t active = 0;

	for (Luau::AstExpr* argument : call->args)
	{
		if (argument->location.end < position)
		{
			++active;
		}
	}

	json signature_info = {{"label", label}, {"parameters", parameters}};

	if (symbol)
	{
		if (std::optional<std::string> notes = docs.get_markdown(*symbol))
		{
			signature_info["documentation"] = protocol::make_markdown(*notes);
		}
	}

	return {
		{"signatures", json::array({signature_info})},
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

}
}

}
