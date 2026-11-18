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

std::optional<target>
find_property(const Luau::Module& module, Luau::AstExprIndexName* index, const Luau::ModuleName& current, bool declaration);
std::optional<Luau::Location> pick_location(const Luau::Property& property, bool declaration);

}
}

json workspace::get_definition(const json& params)
{
	return find_symbol_location(params, false);
}

json workspace::find_symbol_location(const json& params, bool declaration)
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

	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, position, true);
	Luau::AstTypeReference* reference = ancestry.empty() ? nullptr : ancestry.back()->as<Luau::AstTypeReference>();

	if (reference != nullptr)
	{
		Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, position);
		std::optional<declarationinfo> alias = scope ? get_alias_declaration(name, *scope, *reference) : std::nullopt;

		if (!alias)
		{
			return nullptr;
		}

		return get_location(alias->module, alias->location);
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

	if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
	{
		const auto declared = declared_globals.find(global->name.value);

		if (declared == declared_globals.end())
		{
			return nullptr;
		}

		return get_location(declared->second.module, declared->second.location);
	}

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		if (std::optional<priv::target> property = priv::find_property(*module, index, name, declaration))
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

std::optional<target>
find_property(const Luau::Module& module, Luau::AstExprIndexName* index, const Luau::ModuleName& current, bool declaration)
{
	const Luau::TypeId* owner = module.astTypes.find(index->expr);

	if (owner == nullptr)
	{
		return std::nullopt;
	}

	Luau::TypeId followed = Luau::follow(*owner);

	if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed))
	{
		const Luau::Property* property = Luau::lookupExternTypeProp(extern_type, index->index.value);
		std::optional<Luau::Location> location = property ? pick_location(*property, declaration) : std::nullopt;

		if (!location || extern_type->definitionModuleName.empty())
		{
			return std::nullopt;
		}

		return target{extern_type->definitionModuleName, *location};
	}

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
	std::optional<Luau::Location> location = pick_location(property->second, declaration);

	if (!location)
	{
		return std::nullopt;
	}

	return target{definition_module, *location};
}

std::optional<Luau::Location> pick_location(const Luau::Property& property, bool declaration)
{
	if (declaration && property.typeLocation)
	{
		return property.typeLocation;
	}

	if (property.location)
	{
		return property.location;
	}

	return property.typeLocation;
}

}
}

}
