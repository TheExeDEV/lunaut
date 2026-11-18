#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Module.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"
#include "Luau/Type.h"

#include <functional>
#include <set>

namespace lunaut
{
namespace
{
namespace priv
{

std::string get_type_kind(Luau::TypeId type)
{
	type = Luau::follow(type);

	if (Luau::get<Luau::PrimitiveType>(type))
	{
		return "primitive";
	}

	if (Luau::get<Luau::SingletonType>(type))
	{
		return "singleton";
	}

	if (Luau::get<Luau::FunctionType>(type))
	{
		return "function";
	}

	if (Luau::get<Luau::TableType>(type))
	{
		return "table";
	}

	if (Luau::get<Luau::MetatableType>(type))
	{
		return "metatable";
	}

	if (Luau::get<Luau::ExternType>(type))
	{
		return "extern";
	}

	if (Luau::get<Luau::UnionType>(type))
	{
		return "union";
	}

	if (Luau::get<Luau::IntersectionType>(type))
	{
		return "intersection";
	}

	if (Luau::get<Luau::LazyType>(type))
	{
		return "lazy";
	}

	if (Luau::get<Luau::GenericType>(type))
	{
		return "generic";
	}

	if (Luau::get<Luau::FreeType>(type))
	{
		return "free";
	}

	if (Luau::get<Luau::BlockedType>(type))
	{
		return "blocked";
	}

	if (Luau::get<Luau::PendingExpansionType>(type))
	{
		return "pending";
	}

	if (Luau::get<Luau::TypeFunctionInstanceType>(type))
	{
		return "type_function";
	}

	if (Luau::get<Luau::NegationType>(type))
	{
		return "negation";
	}

	if (Luau::get<Luau::AnyType>(type))
	{
		return "any";
	}

	if (Luau::get<Luau::UnknownType>(type))
	{
		return "unknown";
	}

	if (Luau::get<Luau::NeverType>(type))
	{
		return "never";
	}

	if (Luau::get<Luau::NoRefineType>(type))
	{
		return "no_refine";
	}

	return "other";
}

}
}

json workspace::get_type_stats()
{
	for (const auto& [path, document] : resolver.get_documents())
	{
		try
		{
			check_module(resolver.get_module_name(document.get_uri()), false);
		}
		catch (const std::exception& error)
		{
			owner.send_log(enumitem_message_type::Warning, "type statistics: checking " + path + " failed: " + error.what());
		}
		catch (...)
		{
			owner.send_log(enumitem_message_type::Warning, "type statistics: checking " + path + " failed");
		}
	}

	size_t total = 0;
	size_t module_types = 0;
	size_t global_types = 0;

	json origins = json::array();
	json hierarchy = json::array();
	json aliases = json::array();
	std::map<std::string, size_t> kinds;
	std::map<std::string, std::vector<std::string>> children;
	std::set<Luau::TypeId> seen;
	std::set<std::pair<std::string, std::string>> edges;

	std::function<void(Luau::TypeId)> account = [&](Luau::TypeId type)
	{
		type = Luau::follow(type);

		if (!seen.insert(type).second)
		{
			return;
		}

		++kinds[priv::get_type_kind(type)];

		const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(type);

		if (extern_type == nullptr || !extern_type->parent)
		{
			return;
		}

		const Luau::ExternType* parent = Luau::get<Luau::ExternType>(Luau::follow(*extern_type->parent));

		if (parent == nullptr)
		{
			return;
		}

		if (edges.emplace(extern_type->name, parent->name).second)
		{
			hierarchy.push_back(json{{"name", extern_type->name}, {"parent", parent->name}});
			children[parent->name].push_back(extern_type->name);
		}
	};

	for (const loadeddefinitioninfo& definition : definitions)
	{
		const auto module = definition_modules.find(definition.name);
		size_t count = module != definition_modules.end() && module->second ? module->second->interfaceTypes.types.size() : 0;

		origins.push_back(
			json{
				{"name", definition.name},
				{"kind", std::string(enum_definition_source::get_name(definition.source))},
				{"path", definition.path},
				{"priority", definition.priority},
				{"types", count},
			});

		total += count;

		if (module == definition_modules.end() || !module->second)
		{
			continue;
		}

		for (const auto& [name, definition_type] : module->second->exportedTypeBindings)
		{
			account(definition_type.type);
			aliases.push_back(
				json{
					{"module", definition.name},
					{"name", name},
					{"kind", priv::get_type_kind(definition_type.type)},
					{"exported", true},
				});
		}
	}

	for (const auto& [name, source_node] : frontend.sourceNodes)
	{
		Luau::ModulePtr module = frontend.moduleResolver.getModule(name);

		if (!module || definition_modules.count(name) != 0)
		{
			continue;
		}

		size_t internal = module->internalTypes ? module->internalTypes->types.size() : 0;
		size_t interface = module->interfaceTypes.types.size();

		origins.push_back(
			json{
				{"name", name},
				{"kind", "module"},
				{"path", resolver.get_real_path(name).value_or(name)},
				{"internal", internal},
				{"interface", interface},
				{"exported", module->exportedTypeBindings.size()},
			});

		total += internal + interface;
		module_types += internal + interface;

		for (const auto& [exported, definition_type] : module->exportedTypeBindings)
		{
			account(definition_type.type);
			aliases.push_back(
				json{
					{"module", name},
					{"name", exported},
					{"kind", priv::get_type_kind(definition_type.type)},
					{"exported", true},
				});
		}

		for (const auto& [expression, type] : module->astTypes)
		{
			account(type);
		}

		for (const auto& [ast_type, type] : module->astResolvedTypes)
		{
			account(type);
		}
	}

	if (frontend.globals.globalScope != nullptr)
	{
		for (const auto& [symbol, binding] : frontend.globals.globalScope->bindings)
		{
			account(binding.typeId);
			++global_types;
		}

		for (const auto& [name, definition_type] : frontend.globals.globalScope->exportedTypeBindings)
		{
			account(definition_type.type);
			++global_types;
			aliases.push_back(
				json{
					{"module", "@globals"},
					{"name", name},
					{"kind", priv::get_type_kind(definition_type.type)},
					{"exported", true},
				});
		}
	}

	json kind_counts = json::object();

	for (const auto& [kind, count] : kinds)
	{
		kind_counts[kind] = count;
	}

	json subtypes = json::array();

	for (const auto& [name, items] : children)
	{
		subtypes.push_back(json{{"name", name}, {"children", items}});
	}

	return {
		{"total", total},
		{"modules", frontend.sourceNodes.size()},
		{"definitions", definitions.size()},
		{"types",
		 {
			 {"total", total},
			 {"modules", module_types},
			 {"globals", global_types},
			 {"kinds", kind_counts},
		 }},
		{"origins", origins},
		{"hierarchy", hierarchy},
		{"subtypes", subtypes},
		{"relations", hierarchy},
		{"aliases", aliases},
		{"analyzer", analyzer.to_json()},
	};
}

}
