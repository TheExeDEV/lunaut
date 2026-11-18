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

std::optional<Luau::TypeId> strip_optional(Luau::TypeId type);

}
}

json workspace::get_type_definition(const json& params)
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

	Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, position);
	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, position, true);
	Luau::AstNode* node = ancestry.empty() ? nullptr : ancestry.back();

	if (node == nullptr || !scope)
	{
		return nullptr;
	}

	if (Luau::AstTypeReference* reference = node->as<Luau::AstTypeReference>())
	{
		std::optional<declarationinfo> alias = get_alias_declaration(name, *scope, *reference);
		return alias ? get_location(alias->module, alias->location) : json(nullptr);
	}

	std::optional<Luau::TypeId> type;

	if (Luau::AstType* annotation = node->asType())
	{
		if (const Luau::TypeId* resolved = module->astResolvedTypes.find(annotation))
		{
			type = *resolved;
		}
	}
	else
	{
		Luau::ExprOrLocal found = Luau::findExprOrLocalAtPosition(*source, position);

		if (Luau::AstLocal* local = found.getLocal())
		{
			type = scope->lookup(Luau::Symbol(local));
		}
		else if (Luau::AstExpr* expr = found.getExpr())
		{
			if (const Luau::TypeId* inferred = module->astTypes.find(expr))
			{
				type = *inferred;
			}
			else if (Luau::AstExprLocal* reference = expr->as<Luau::AstExprLocal>())
			{
				type = scope->lookup(Luau::Symbol(reference->local));
			}
			else if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
			{
				type = scope->lookup(global->name);
			}
		}
	}

	if (!type)
	{
		return nullptr;
	}

	std::optional<declarationinfo> declaration = get_type_declaration(*type);
	return declaration ? get_location(declaration->module, declaration->location) : json(nullptr);
}

std::optional<declarationinfo>
workspace::get_alias_declaration(const Luau::ModuleName& name, const Luau::Scope& scope, const Luau::AstTypeReference& reference) const
{
	std::string type_name = reference.name.value;

	if (reference.prefix)
	{
		for (const Luau::Scope* current = &scope; current != nullptr; current = current->parent.get())
		{
			const auto imported = current->importedModules.find(reference.prefix->value);

			if (imported == current->importedModules.end())
			{
				continue;
			}

			Luau::ModulePtr required = get_module(imported->second, false);

			if (!required || !required->hasModuleScope())
			{
				return std::nullopt;
			}

			const Luau::ScopePtr& exported = required->getModuleScope();
			const auto location = exported->typeAliasNameLocations.find(type_name);

			if (location == exported->typeAliasNameLocations.end())
			{
				return std::nullopt;
			}

			return declarationinfo{imported->second, location->second};
		}

		return std::nullopt;
	}

	for (const Luau::Scope* current = &scope; current != nullptr; current = current->parent.get())
	{
		if (current == frontend.globals.globalScope.get() || current == frontend.globalsForAutocomplete.globalScope.get())
		{
			break;
		}

		const auto location = current->typeAliasNameLocations.find(type_name);

		if (location != current->typeAliasNameLocations.end())
		{
			return declarationinfo{name, location->second};
		}
	}

	const auto declared = declared_types.find(type_name);

	if (declared == declared_types.end())
	{
		return std::nullopt;
	}

	return declared->second;
}

std::optional<declarationinfo> workspace::get_type_declaration(Luau::TypeId type) const
{
	std::optional<Luau::TypeId> stripped = priv::strip_optional(type);

	if (!stripped)
	{
		return std::nullopt;
	}

	Luau::TypeId followed = *stripped;

	if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
	{
		followed = Luau::follow(metatable->table);
	}

	if (const Luau::TableType* table = Luau::get<Luau::TableType>(followed))
	{
		if (table->definitionModuleName.empty())
		{
			return std::nullopt;
		}

		return declarationinfo{table->definitionModuleName, table->definitionLocation};
	}

	if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed))
	{
		const auto declared = declared_types.find(extern_type->name);

		if (declared != declared_types.end())
		{
			return declared->second;
		}

		if (extern_type->definitionModuleName.empty() || !extern_type->definitionLocation)
		{
			return std::nullopt;
		}

		return declarationinfo{extern_type->definitionModuleName, *extern_type->definitionLocation};
	}

	return std::nullopt;
}

namespace
{
namespace priv
{

std::optional<Luau::TypeId> strip_optional(Luau::TypeId type)
{
	Luau::TypeId followed = Luau::follow(type);
	const Luau::UnionType* options = Luau::get<Luau::UnionType>(followed);

	if (options == nullptr)
	{
		return followed;
	}

	std::optional<Luau::TypeId> single;

	for (Luau::TypeId option : options->options)
	{
		Luau::TypeId followed_option = Luau::follow(option);

		if (Luau::isNil(followed_option))
		{
			continue;
		}

		if (single)
		{
			return std::nullopt;
		}

		single = followed_option;
	}

	return single;
}

}
}

}
