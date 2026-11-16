#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Scope.h"

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
std::optional<Luau::ModuleName> find_imported_module(const Luau::ScopePtr& scope, const std::string& alias);
std::optional<Luau::ModuleName> get_type_module(Luau::TypeId type);

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

	const auto source_node = frontend.sourceNodes.find(name);

	if (source_node != frontend.sourceNodes.end())
	{
		for (const auto& [required, location] : source_node->second->requireLocations)
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

	Luau::ExprOrLocal type_target = Luau::findExprOrLocalAtPosition(*source, position);
	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, position, true);
	Luau::AstNode* node = ancestry.empty() ? nullptr : ancestry.back();
	Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, position);

	if (Luau::AstStatTypeAlias* statement = node != nullptr ? node->as<Luau::AstStatTypeAlias>() : nullptr)
	{
		if (statement->nameLocation.containsClosed(position) && scope != nullptr)
		{
			std::optional<Luau::TypeFun> definition = scope->lookupType(statement->name.value);

			if (definition && definition->definitionLocation)
			{
				return get_location(name, *definition->definitionLocation);
			}

			return get_location(name, statement->nameLocation);
		}
	}

	if (Luau::AstTypeReference* reference = node != nullptr ? node->as<Luau::AstTypeReference>() : nullptr)
	{
		if (scope != nullptr)
		{
			std::string alias = reference->prefix ? std::string(reference->prefix->value) : "";
			std::optional<Luau::TypeFun> definition =
				reference->prefix ? scope->lookupImportedType(alias, reference->name.value) : scope->lookupType(reference->name.value);

			if (!definition && !reference->prefix && frontend.globals.globalScope != nullptr)
			{
				definition = frontend.globals.globalScope->lookupType(reference->name.value);
			}

			if (definition && definition->definitionLocation)
			{
				Luau::ModuleName origin = name;

				if (reference->prefix)
				{
					origin = priv::find_imported_module(scope, alias).value_or(name);
				}
				else if (std::optional<Luau::ModuleName> owner = find_definition_module(*definition->definitionLocation))
				{
					origin = *owner;
				}
				else if (std::optional<Luau::ModuleName> owner = priv::get_type_module(definition->type))
				{
					origin = *owner;
				}
				else if (std::optional<Luau::ModuleName> owner = find_definition_module(definition->type))
				{
					origin = *owner;
				}

				return get_location(origin, *definition->definitionLocation);
			}

			if (!reference->prefix)
			{
				for (Luau::Scope* current = scope.get(); current != nullptr; current = current->parent.get())
				{
					const auto found_alias = current->typeAliasLocations.find(reference->name.value);

					if (found_alias != current->typeAliasLocations.end())
					{
						return get_location(name, found_alias->second);
					}
				}
			}
		}
	}

	Luau::ExprOrLocal found = type_target;

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

std::optional<Luau::ModuleName> find_imported_module(const Luau::ScopePtr& scope, const std::string& alias)
{
	for (Luau::Scope* current = scope.get(); current != nullptr; current = current->parent.get())
	{
		const auto found = current->importedModules.find(alias);

		if (found != current->importedModules.end())
		{
			return found->second;
		}
	}

	return std::nullopt;
}

std::optional<Luau::ModuleName> get_type_module(Luau::TypeId type)
{
	Luau::TypeId followed = Luau::follow(type);

	if (const Luau::TableType* table = Luau::get<Luau::TableType>(followed); table != nullptr && !table->definitionModuleName.empty())
	{
		return table->definitionModuleName;
	}

	if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
	{
		return get_type_module(metatable->table);
	}

	if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed);
		extern_type != nullptr && !extern_type->definitionModuleName.empty())
	{
		return extern_type->definitionModuleName;
	}

	return std::nullopt;
}

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
