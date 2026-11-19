#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Lexer.h"
#include "Luau/PrettyPrinter.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"
#include "Luau/VisitType.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace lunaut
{

const Luau::AstExpr* typelookup::find_initializer(const Luau::SourceModule& source, const Luau::AstLocal* local)
{
	if (local == nullptr)
	{
		return nullptr;
	}

	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(source, local->location.begin, true);

	for (Luau::AstNode* node : ancestry)
	{
		Luau::AstStatLocal* statement = node->as<Luau::AstStatLocal>();

		if (statement == nullptr)
		{
			continue;
		}

		for (size_t index = 0; index < statement->vars.size && index < statement->values.size; ++index)
		{
			if (statement->vars.data[index] == local)
			{
				return statement->values.data[index];
			}
		}
	}

	return nullptr;
}

std::optional<namedtypeinfo> typelookup::get_reference_alias(const Luau::ScopePtr& scope, const Luau::AstType* annotation)
{
	if (annotation == nullptr)
	{
		return std::nullopt;
	}

	if (const Luau::AstTypeUnion* union_type = annotation->as<Luau::AstTypeUnion>())
	{
		const Luau::AstType* option = nullptr;
		bool optional = false;

		for (Luau::AstType* part : union_type->types)
		{
			if (part->is<Luau::AstTypeOptional>() ||
				(part->as<Luau::AstTypeReference>() != nullptr && !part->as<Luau::AstTypeReference>()->prefix &&
				 part->as<Luau::AstTypeReference>()->name == "nil"))
			{
				optional = true;
				continue;
			}

			if (option != nullptr)
			{
				return std::nullopt;
			}

			option = part;
		}

		if (option == nullptr)
		{
			return std::nullopt;
		}

		std::optional<namedtypeinfo> alias = typelookup::get_reference_alias(scope, option);

		if (alias && optional)
		{
			alias->display = (alias->display.empty() ? alias->name : alias->display) + "?";
		}

		return alias;
	}

	const Luau::AstTypeReference* reference = annotation->as<Luau::AstTypeReference>();

	if (reference == nullptr || reference->hasParameterList)
	{
		return std::nullopt;
	}

	std::optional<Luau::TypeFun> definition = reference->prefix ? scope->lookupImportedType(reference->prefix->value, reference->name.value)
																: scope->lookupType(reference->name.value);

	if (!definition)
	{
		return std::nullopt;
	}

	if (Luau::get<Luau::GenericType>(Luau::follow(definition->type)) != nullptr)
	{
		return std::nullopt;
	}

	for (const Luau::GenericTypeDefinition& parameter : definition->typeParams)
	{
		if (!parameter.defaultValue)
		{
			return std::nullopt;
		}
	}

	for (const Luau::GenericTypePackDefinition& parameter : definition->typePackParams)
	{
		if (!parameter.defaultValue)
		{
			return std::nullopt;
		}
	}

	std::string name = reference->prefix ? std::string(reference->prefix->value) + "." + reference->name.value : reference->name.value;
	return namedtypeinfo{name, *definition};
}

std::optional<Luau::Property> typelookup::find_property(Luau::TypeId owner, const std::string& name, Luau::TypeId& base)
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
					base = current;
					return found->second;
				}
			}

			const Luau::TableType* meta = Luau::get<Luau::TableType>(Luau::follow(metatable->metatable));
			const auto index = meta != nullptr ? meta->props.find("__index") : decltype(meta->props.end()){};

			if (meta == nullptr || index == meta->props.end() || !index->second.readTy)
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
				if (std::optional<Luau::Property> property = typelookup::find_property(part, name, base))
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

std::optional<Luau::ModuleName> typelookup::get_definition_module(Luau::TypeId type)
{
	Luau::TypeId followed = Luau::follow(type);

	if (const Luau::TableType* table = Luau::get<Luau::TableType>(followed); table != nullptr && !table->definitionModuleName.empty())
	{
		return table->definitionModuleName;
	}

	if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
	{
		return typelookup::get_definition_module(metatable->table);
	}

	if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed);
		extern_type != nullptr && !extern_type->definitionModuleName.empty())
	{
		return extern_type->definitionModuleName;
	}

	return std::nullopt;
}

std::optional<Luau::ModuleName> typelookup::find_imported_module(const Luau::ScopePtr& scope, const std::string& alias)
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

}
