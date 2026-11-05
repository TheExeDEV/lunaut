#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"

namespace lunaut
{
namespace
{
namespace priv
{

struct target
{
	Luau::ModuleName module;
	Luau::Location location;
};

std::optional<target> find_property(const Luau::Module& module, Luau::AstExprIndexName* index, const Luau::ModuleName& current);

}
}

json workspace::get_definition(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr)
	{
		return nullptr;
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, false);

	const auto node = frontend.sourceNodes.find(name);

	if (node != frontend.sourceNodes.end())
	{
		for (const auto& [required, location] : node->second->requireLocations)
		{
			if (location.containsClosed(position))
			{
				std::string required_uri = resolver.get_uri(required);

				if (required_uri.empty())
				{
					return nullptr;
				}

				json start = {{"line", 0}, {"character", 0}};
				json range = {{"start", start}, {"end", start}};
				return {{"uri", required_uri}, {"range", range}};
			}
		}
	}

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (source == nullptr || !module)
	{
		return nullptr;
	}

	Luau::ExprOrLocal found = Luau::findExprOrLocalAtPosition(*source, position);

	if (Luau::AstLocal* local = found.getLocal())
	{
		return get_location(name, local->location);
	}

	Luau::AstExpr* expr = found.getExpr();

	if (expr == nullptr)
	{
		return nullptr;
	}

	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return get_location(name, local->local->location);
	}

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		if (std::optional<priv::target> property = priv::find_property(*module, index, name))
		{
			return get_location(property->module, property->location);
		}
	}

	return nullptr;
}

namespace
{
namespace priv
{

std::optional<target> find_property(const Luau::Module& module, Luau::AstExprIndexName* index, const Luau::ModuleName& current)
{
	const Luau::TypeId* owner = module.astTypes.find(index->expr);

	if (owner == nullptr)
	{
		return std::nullopt;
	}

	Luau::TypeId followed = Luau::follow(*owner);
	const Luau::TableType* table = Luau::get<Luau::TableType>(followed);

	if (table == nullptr)
	{
		if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
		{
			table = Luau::get<Luau::TableType>(Luau::follow(metatable->table));
		}
	}

	if (table == nullptr)
	{
		return std::nullopt;
	}

	const auto property = table->props.find(index->index.value);

	if (property == table->props.end())
	{
		return std::nullopt;
	}

	Luau::ModuleName definition_module = table->definitionModuleName.empty() ? current : table->definitionModuleName;

	if (property->second.location)
	{
		return target{definition_module, *property->second.location};
	}

	if (property->second.typeLocation)
	{
		return target{definition_module, *property->second.typeLocation};
	}

	return std::nullopt;
}

}
}

}
