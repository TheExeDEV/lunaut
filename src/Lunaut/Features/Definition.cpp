#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Scope.h"

#include <set>

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
std::optional<Luau::ModuleName> find_expression_module(const Luau::ScopePtr& scope, Luau::AstExpr* expr);
std::optional<Luau::Location>
find_property_in_expression(const Luau::SourceModule& source, const Luau::AstExpr* expr, const std::string& property);
const Luau::AstExpr* find_local_initializer(const Luau::SourceModule& source, const Luau::AstLocal* local);
std::optional<std::string> get_key_name(const Luau::AstExpr* key);
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
		if (std::optional<Luau::ModuleName> required = priv::find_expression_module(scope, index->expr))
		{
			if (std::optional<Luau::Location> implementation = find_property_implementation(*required, index->index.value))
			{
				return get_location(*required, *implementation);
			}
		}

		if (std::optional<priv::target> property = priv::find_property(*module, index, name))
		{
			return get_location(property->module, property->location);
		}
	}

	return find_symbol_location(params, false);
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

std::optional<Luau::Property> find_property_from_type(Luau::TypeId owner, const std::string& name, Luau::TypeId& base)
{
	Luau::TypeId current = Luau::follow(owner);

	for (int depth = 0; depth < 32; ++depth)
	{
		if (const Luau::TableType* table = Luau::get<Luau::TableType>(current))
		{
			const auto found = table->props.find(name);

			if (found != table->props.end())
			{
				base = current;
				return found->second;
			}

			return std::nullopt;
		}

		if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(current))
		{
			if (const Luau::TableType* table = Luau::get<Luau::TableType>(Luau::follow(metatable->table)))
			{
				const auto found = table->props.find(name);

				if (found != table->props.end())
				{
					base = Luau::follow(metatable->table);
					return found->second;
				}
			}

			const Luau::TableType* meta = Luau::get<Luau::TableType>(Luau::follow(metatable->metatable));

			if (meta == nullptr)
			{
				return std::nullopt;
			}

			const auto index = meta->props.find("__index");

			if (index == meta->props.end() || !index->second.readTy)
			{
				return std::nullopt;
			}

			current = Luau::follow(*index->second.readTy);
			continue;
		}

		if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(current))
		{
			const auto found = extern_type->props.find(name);

			if (found != extern_type->props.end())
			{
				base = current;
				return found->second;
			}

			if (!extern_type->parent)
			{
				return std::nullopt;
			}

			current = Luau::follow(*extern_type->parent);
			continue;
		}

		if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(current))
		{
			for (Luau::TypeId part : intersection->parts)
			{
				if (std::optional<Luau::Property> property = find_property_from_type(part, name, base))
				{
					return property;
				}
			}

			return std::nullopt;
		}

		return std::nullopt;
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

	Luau::TypeId base = Luau::follow(*owner);

	if (std::optional<Luau::Property> property = find_property_from_type(base, index->index.value, base))
	{
		std::optional<Luau::ModuleName> definition_module = get_type_module(base);

		if (!definition_module)
		{
			definition_module = current;
		}

		if (property->location)
		{
			return target{*definition_module, *property->location};
		}

		if (property->typeLocation)
		{
			return target{*definition_module, *property->typeLocation};
		}
	}

	return std::nullopt;
}

std::optional<Luau::ModuleName> find_expression_module(const Luau::ScopePtr& scope, Luau::AstExpr* expr)
{
	if (expr == nullptr || scope == nullptr)
	{
		return std::nullopt;
	}

	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return find_imported_module(scope, local->local->name.value);
	}

	if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
	{
		return find_imported_module(scope, global->name.value);
	}

	return std::nullopt;
}

const Luau::AstExpr* find_local_initializer(const Luau::SourceModule& source, const Luau::AstLocal* local)
{
	if (source.root == nullptr || local == nullptr)
	{
		return nullptr;
	}

	for (Luau::AstStat* statement : source.root->body)
	{
		Luau::AstStatLocal* declaration = statement->as<Luau::AstStatLocal>();

		if (declaration == nullptr)
		{
			continue;
		}

		for (size_t index = 0; index < declaration->vars.size && index < declaration->values.size; ++index)
		{
			if (declaration->vars.data[index] == local)
			{
				return declaration->values.data[index];
			}
		}
	}

	return nullptr;
}

std::optional<std::string> get_key_name(const Luau::AstExpr* key)
{
	if (const Luau::AstExprGlobal* global = key != nullptr ? key->as<Luau::AstExprGlobal>() : nullptr)
	{
		return std::string(global->name.value);
	}

	if (const Luau::AstExprConstantString* value = key != nullptr ? key->as<Luau::AstExprConstantString>() : nullptr)
	{
		return std::string(value->value.data, value->value.size);
	}

	return std::nullopt;
}

std::optional<Luau::Location>
find_property_in_expression(const Luau::SourceModule& source, const Luau::AstExpr* expr, const std::string& property)
{
	if (expr == nullptr)
	{
		return std::nullopt;
	}

	if (const Luau::AstExprTable* table = expr->as<Luau::AstExprTable>())
	{
		for (const Luau::AstExprTable::Item& item : table->items)
		{
			if (item.key == nullptr)
			{
				continue;
			}

			std::optional<std::string> name = get_key_name(item.key);

			if (name && *name == property)
			{
				return item.key->location;
			}
		}

		return std::nullopt;
	}

	if (const Luau::AstExprCall* call = expr->as<Luau::AstExprCall>())
	{
		for (Luau::AstExpr* argument : call->args)
		{
			if (std::optional<Luau::Location> found = find_property_in_expression(source, argument, property))
			{
				return found;
			}
		}

		return std::nullopt;
	}

	if (const Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return find_property_in_expression(source, find_local_initializer(source, local->local), property);
	}

	if (const Luau::AstExprTypeAssertion* assertion = expr->as<Luau::AstExprTypeAssertion>())
	{
		return find_property_in_expression(source, assertion->expr, property);
	}

	if (const Luau::AstExprGroup* group = expr->as<Luau::AstExprGroup>())
	{
		return find_property_in_expression(source, group->expr, property);
	}

	if (const Luau::AstExprIndexExpr* index = expr->as<Luau::AstExprIndexExpr>())
	{
		return find_property_in_expression(source, index->expr, property);
	}

	return std::nullopt;
}

}
}

std::optional<Luau::Location> workspace::find_property_implementation(const Luau::ModuleName& name, const std::string& property) const
{
	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source == nullptr)
	{
		auto known = definition_sources.find(name);

		if (known != definition_sources.end())
		{
			source = &known->second;
		}
	}

	if (source == nullptr || source->root == nullptr)
	{
		return std::nullopt;
	}

	for (Luau::AstStat* statement : source->root->body)
	{
		Luau::AstStatReturn* statement_return = statement->as<Luau::AstStatReturn>();

		if (statement_return == nullptr)
		{
			continue;
		}

		for (Luau::AstExpr* value : statement_return->list)
		{
			if (std::optional<Luau::Location> found = priv::find_property_in_expression(*source, value, property))
			{
				return found;
			}
		}
	}

	std::set<const Luau::AstLocal*> returned;

	for (Luau::AstStat* statement : source->root->body)
	{
		Luau::AstStatReturn* statement_return = statement->as<Luau::AstStatReturn>();

		if (statement_return == nullptr)
		{
			continue;
		}

		for (Luau::AstExpr* value : statement_return->list)
		{
			if (Luau::AstExprLocal* local = value->as<Luau::AstExprLocal>())
			{
				returned.insert(local->local);
			}
		}
	}

	if (returned.empty())
	{
		return std::nullopt;
	}

	auto belongs_to_returned = [&](const Luau::AstExpr* expr) -> bool
	{
		if (const Luau::AstExprLocal* local = expr != nullptr ? expr->as<Luau::AstExprLocal>() : nullptr)
		{
			return returned.count(local->local) != 0;
		}

		return false;
	};

	for (Luau::AstStat* statement : source->root->body)
	{
		if (Luau::AstStatFunction* statement_function = statement->as<Luau::AstStatFunction>())
		{
			Luau::AstExprIndexName* index = statement_function->name->as<Luau::AstExprIndexName>();

			if (index != nullptr && index->index.value == property && belongs_to_returned(index->expr))
			{
				return index->indexLocation;
			}
		}

		if (Luau::AstStatAssign* statement_assign = statement->as<Luau::AstStatAssign>())
		{
			for (Luau::AstExpr* variable : statement_assign->vars)
			{
				Luau::AstExprIndexName* index = variable->as<Luau::AstExprIndexName>();

				if (index != nullptr && index->index.value == property && belongs_to_returned(index->expr))
				{
					return index->indexLocation;
				}
			}
		}
	}

	return std::nullopt;
}

}
