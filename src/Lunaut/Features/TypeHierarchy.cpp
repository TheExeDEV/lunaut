#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"

#include <algorithm>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr int kind_class = 5;

const Luau::ExternType* get_extern_type(std::optional<Luau::TypeId> type);
std::optional<Luau::TypeId> find_global_type(const Luau::GlobalTypes& globals, const std::string& name);

}
}

json workspace::get_type_hierarchy_items(const json& params)
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

	std::optional<Luau::TypeId> type;

	if (Luau::AstTypeReference* reference = node->as<Luau::AstTypeReference>())
	{
		std::optional<Luau::TypeFun> definition = reference->prefix
			? scope->lookupImportedType(reference->prefix->value, reference->name.value)
			: scope->lookupType(reference->name.value);

		if (definition)
		{
			type = definition->type;
		}
	}
	else if (Luau::AstLocal* local = Luau::findExprOrLocalAtPosition(*source, position).getLocal())
	{
		type = scope->lookup(Luau::Symbol(local));
	}
	else if (Luau::AstExpr* expr = Luau::findExprOrLocalAtPosition(*source, position).getExpr())
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

	const Luau::ExternType* extern_type = priv::get_extern_type(type);

	if (extern_type == nullptr)
	{
		return nullptr;
	}

	std::optional<Luau::TypeId> global = priv::find_global_type(frontend.globals, extern_type->name);
	json item = make_type_hierarchy_item(global ? *global : Luau::follow(*type), address);
	return item.is_null() ? json(nullptr) : json::array({item});
}

json workspace::get_supertypes(const json& params)
{
	json items = json::array();
	std::string type_name = params.value("/item/data/name"_json_pointer, "");
	std::string origin = params.value("/item/data/origin"_json_pointer, "");
	const Luau::ExternType* extern_type = priv::get_extern_type(priv::find_global_type(frontend.globals, type_name));

	if (extern_type == nullptr || !extern_type->parent)
	{
		return items;
	}

	json item = make_type_hierarchy_item(Luau::follow(*extern_type->parent), origin);

	if (!item.is_null())
	{
		items.push_back(item);
	}

	return items;
}

json workspace::get_subtypes(const json& params)
{
	json items = json::array();
	std::string type_name = params.value("/item/data/name"_json_pointer, "");
	std::string origin = params.value("/item/data/origin"_json_pointer, "");
	std::optional<Luau::TypeId> base = priv::find_global_type(frontend.globals, type_name);

	if (!priv::get_extern_type(base))
	{
		return items;
	}

	std::vector<std::pair<std::string, Luau::TypeId>> children;

	for (const auto& [child_name, definition] : frontend.globals.globalScope->exportedTypeBindings)
	{
		Luau::TypeId child = Luau::follow(definition.type);
		const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(child);

		if (extern_type != nullptr && extern_type->parent && Luau::follow(*extern_type->parent) == *base)
		{
			children.emplace_back(child_name, child);
		}
	}

	std::sort(
		children.begin(),
		children.end(),
		[](const std::pair<std::string, Luau::TypeId>& left, const std::pair<std::string, Luau::TypeId>& right)
		{ return left.first < right.first; });

	for (const auto& [child_name, child] : children)
	{
		json item = make_type_hierarchy_item(child, origin);

		if (!item.is_null())
		{
			items.push_back(item);
		}
	}

	return items;
}

json workspace::make_type_hierarchy_item(Luau::TypeId type, const std::string& origin) const
{
	const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(Luau::follow(type));

	if (extern_type == nullptr)
	{
		return nullptr;
	}

	std::optional<declarationinfo> declaration = get_type_declaration(type);
	json location = declaration ? get_location(declaration->module, declaration->location) : json(nullptr);

	if (location.is_null())
	{
		json start = {{"line", 0}, {"character", 0}};
		location = {{"uri", origin}, {"range", {{"start", start}, {"end", start}}}};
	}

	json item = {
		{"name", extern_type->name},
		{"kind", priv::kind_class},
		{"uri", location["uri"]},
		{"range", location["range"]},
		{"selectionRange", location["range"]},
		{"data", {{"name", extern_type->name}, {"origin", origin}}},
	};

	if (extern_type->parent)
	{
		if (const Luau::ExternType* parent = Luau::get<Luau::ExternType>(Luau::follow(*extern_type->parent)))
		{
			item["detail"] = "extends " + parent->name;
		}
	}

	return item;
}

namespace
{
namespace priv
{

const Luau::ExternType* get_extern_type(std::optional<Luau::TypeId> type)
{
	if (!type)
	{
		return nullptr;
	}

	Luau::TypeId followed = Luau::follow(*type);

	if (const Luau::UnionType* options = Luau::get<Luau::UnionType>(followed))
	{
		const Luau::ExternType* single = nullptr;

		for (Luau::TypeId option : options->options)
		{
			Luau::TypeId followed_option = Luau::follow(option);

			if (Luau::isNil(followed_option))
			{
				continue;
			}

			if (single != nullptr)
			{
				return nullptr;
			}

			single = Luau::get<Luau::ExternType>(followed_option);
		}

		return single;
	}

	return Luau::get<Luau::ExternType>(followed);
}

std::optional<Luau::TypeId> find_global_type(const Luau::GlobalTypes& globals, const std::string& name)
{
	const auto found = globals.globalScope->exportedTypeBindings.find(name);

	if (found == globals.globalScope->exportedTypeBindings.end())
	{
		return std::nullopt;
	}

	return Luau::follow(found->second.type);
}

}
}

}
