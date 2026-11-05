#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Common.h"
#include "Luau/Scope.h"
#include "Luau/Type.h"

#include <algorithm>

LUAU_FASTFLAG(LuauSolverV2)

namespace lunaut
{
namespace
{
namespace priv
{

const std::string generated_tag = "@lunaut/sourcemap";

const std::vector<std::pair<std::string_view, std::vector<std::string_view>>> relevant_player_containers = {
	{"PlayerGui", {"StarterGui"}},
	{"StarterGear", {"StarterPack"}},
	{"PlayerScripts", {"StarterPlayer", "StarterPlayerScripts"}},
};

std::optional<Luau::TypeId> get_class_type(const Luau::GlobalTypes& globals, const std::string& name);
const sourcenode* find_path(const sourcenode* root, const std::vector<std::string_view>& path);
void clear_generated(const Luau::GlobalTypes& globals);
Luau::Property make_child_property(Luau::TypeId type);

}
}

void instancetypes::apply(Luau::Frontend& frontend, const sourcemap& map, bool strict_datamodel_types)
{
	clear(frontend, map);

	if (!map.is_loaded())
	{
		return;
	}

	apply_globals(frontend.globals, map);

	if (!FFlag::LuauSolverV2)
	{
		apply_globals(frontend.globalsForAutocomplete, map);
	}

	const sourcemap* source = &map;

	frontend.prepareModuleScope =
		[this, &frontend, source, strict_datamodel_types](const Luau::ModuleName& name, const Luau::ScopePtr& scope, bool for_autocomplete)
	{
		const Luau::GlobalTypes& globals = (for_autocomplete && !FFlag::LuauSolverV2) ? frontend.globalsForAutocomplete : frontend.globals;
		bool expressive = strict_datamodel_types || FFlag::LuauSolverV2;

		if (!expressive && !for_autocomplete)
		{
			scope->bindings[Luau::AstName("script")] = Luau::Binding{globals.builtinTypes->anyType};
			scope->bindings[Luau::AstName("workspace")] = Luau::Binding{globals.builtinTypes->anyType};
			scope->bindings[Luau::AstName("game")] = Luau::Binding{globals.builtinTypes->anyType};
			return;
		}

		const sourcenode* node = sourcemap::is_virtual_path(name) ? source->get_by_virtual_path(name) : source->get_by_real_path(name);

		if (node != nullptr)
		{
			scope->bindings[Luau::AstName("script")] = Luau::Binding{get_node_type(globals, node)};
		}
	};
}

void instancetypes::clear(Luau::Frontend& frontend, const sourcemap& map)
{
	for (const auto& [name, node] : frontend.sourceNodes)
	{
		frontend.markDirty(name);
		node->setInvalidModuleDependency(true, false);
		node->setInvalidModuleDependency(true, true);
	}

	priv::clear_generated(frontend.globals);

	if (!FFlag::LuauSolverV2)
	{
		priv::clear_generated(frontend.globalsForAutocomplete);
	}

	if (const sourcenode* root = map.get_root())
	{
		root->clear_types();
	}

	frontend.prepareModuleScope = nullptr;
}

void instancetypes::apply_globals(const Luau::GlobalTypes& globals, const sourcemap& map)
{
	const sourcenode* root = map.get_root();
	get_node_type(globals, root);

	if (root->class_name != "DataModel")
	{
		return;
	}

	if (std::optional<Luau::TypeId> datamodel = priv::get_class_type(globals, "DataModel"))
	{
		add_children(globals, *datamodel, root);
	}

	for (const std::unique_ptr<sourcenode>& service : root->children)
	{
		if (std::optional<Luau::TypeId> type = priv::get_class_type(globals, service->class_name))
		{
			add_children(globals, *type, service.get());
		}
	}

	std::optional<Luau::TypeId> player = priv::get_class_type(globals, "Player");

	if (!player)
	{
		return;
	}

	Luau::ExternType* player_class = Luau::getMutable<Luau::ExternType>(*player);

	if (player_class == nullptr)
	{
		return;
	}

	for (const auto& [container, path] : priv::relevant_player_containers)
	{
		std::optional<Luau::TypeId> container_type = priv::get_class_type(globals, std::string(container));

		if (!container_type)
		{
			continue;
		}

		if (const sourcenode* source = priv::find_path(root, path))
		{
			add_children(globals, *container_type, source);
		}

		Luau::Property property = Luau::makeProperty(*container_type);
		property.tags.push_back(priv::generated_tag);
		player_class->props[std::string(container)] = property;
	}
}

Luau::TypeId instancetypes::get_node_type(const Luau::GlobalTypes& globals, const sourcenode* node)
{
	const auto found = node->types.find(&globals);

	if (found != node->types.end())
	{
		return found->second;
	}

	Luau::LazyType lazy(
		[this, &globals, node](Luau::LazyType& self)
		{
			if (self.unwrapped.load())
			{
				return;
			}

			std::optional<Luau::TypeId> instance = priv::get_class_type(globals, "Instance");
			std::optional<Luau::TypeId> base = priv::get_class_type(globals, node->class_name);
			const Luau::ExternType* instance_class = instance ? Luau::get<Luau::ExternType>(*instance) : nullptr;

			if (!base || instance_class == nullptr)
			{
				self.unwrapped = globals.builtinTypes->anyType;
				return;
			}

			Luau::TypeId type = arena.addType(
				Luau::ExternType{
					node->class_name,
					{},
					*base,
					instance_class->metatable,
					{},
					{},
					instance_class->definitionModuleName,
					instance_class->definitionLocation,
				});

			Luau::ExternType* extern_class = Luau::getMutable<Luau::ExternType>(type);

			if (node->parent != nullptr)
			{
				Luau::TypeId parent = get_node_type(globals, node->parent);
				extern_class->props["Parent"] = FFlag::LuauSolverV2 ? Luau::Property::rw(parent, *instance) : Luau::makeProperty(parent);
			}

			for (const std::unique_ptr<sourcenode>& child : node->children)
			{
				extern_class->props[child->name] = priv::make_child_property(get_node_type(globals, child.get()));
			}

			self.unwrapped = type;
		});

	Luau::TypeId type = arena.addType(std::move(lazy));
	node->types.insert_or_assign(&globals, type);
	return type;
}

void instancetypes::add_children(const Luau::GlobalTypes& globals, Luau::TypeId type, const sourcenode* node)
{
	Luau::ExternType* extern_class = Luau::getMutable<Luau::ExternType>(type);

	if (extern_class == nullptr)
	{
		return;
	}

	for (const std::unique_ptr<sourcenode>& child : node->children)
	{
		extern_class->props[child->name] = priv::make_child_property(get_node_type(globals, child.get()));
	}
}

namespace
{
namespace priv
{

std::optional<Luau::TypeId> get_class_type(const Luau::GlobalTypes& globals, const std::string& name)
{
	std::optional<Luau::TypeFun> found = globals.globalScope->lookupType(name);

	if (!found)
	{
		return std::nullopt;
	}

	return found->type;
}

const sourcenode* find_path(const sourcenode* root, const std::vector<std::string_view>& path)
{
	const sourcenode* node = root;

	for (std::string_view name : path)
	{
		if (node == nullptr)
		{
			return nullptr;
		}

		node = node->find_child(name);
	}

	return node;
}

void clear_generated(const Luau::GlobalTypes& globals)
{
	for (const auto& [name, definition] : globals.globalScope->exportedTypeBindings)
	{
		Luau::ExternType* extern_class = Luau::getMutable<Luau::ExternType>(definition.type);

		if (extern_class == nullptr)
		{
			continue;
		}

		for (auto it = extern_class->props.begin(); it != extern_class->props.end();)
		{
			bool generated = std::find(it->second.tags.begin(), it->second.tags.end(), generated_tag) != it->second.tags.end();
			it = generated ? extern_class->props.erase(it) : std::next(it);
		}
	}
}

Luau::Property make_child_property(Luau::TypeId type)
{
	return Luau::Property{type, false, {}, std::nullopt, {generated_tag}, std::nullopt};
}

}
}

}
