#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"

#include <algorithm>
#include <stdexcept>

namespace lunaut
{
namespace
{
namespace priv
{

const std::set<std::string_view> reserved_words = {
	"and",
	"break",
	"do",
	"else",
	"elseif",
	"end",
	"false",
	"for",
	"function",
	"if",
	"in",
	"local",
	"nil",
	"not",
	"or",
	"repeat",
	"return",
	"then",
	"true",
	"until",
	"while",
};

struct name_collector final : Luau::AstVisitor
{
	bool global = false;
	std::string name;
	std::vector<Luau::Location> plain;
	std::vector<std::pair<std::string, Luau::Location>> prefixed;

	bool visit(Luau::AstType* node) override;
	bool visit(Luau::AstTypePack* node) override;
	bool visit(Luau::AstTypeReference* node) override;
	bool visit(Luau::AstStatTypeAlias* node) override;
	bool visit(Luau::AstExprGlobal* node) override;
};

bool is_identifier(const std::string& name);
bool is_renamable(const symbolinfo& symbol);
std::optional<renametargetinfo> find_target(
	const Luau::SourceModule& source,
	const Luau::Module* module,
	const Luau::ModuleName& name,
	Luau::Position position,
	const Luau::GlobalTypes& globals);
std::optional<Luau::ModuleName> find_imported_module(const Luau::Module& module, Luau::Position position, const std::string& prefix);
bool declares_type(const Luau::SourceModule& source, const std::string& name);

}
}

void workspace::index_workspace()
{
	if (root.empty())
	{
		return;
	}

	std::vector<Luau::ModuleName> names;
	std::vector<std::string_view> patterns = {"**/*.luau"};

	if (settings.analyze_lua_files)
	{
		patterns.push_back("**/*.lua");
	}

	std::set<std::string> seen;

	auto add_file = [&](const std::filesystem::path& file)
	{
		std::string path = uri::normalize_path(file.generic_string());

		if (is_definition_file(path) || stdx::ends_with(path, ".d.luau") || !seen.insert(path).second)
		{
			return;
		}

		names.push_back(resolver.get_module_name(uri::from_path(path)));
	};

	for (std::string_view pattern : patterns)
	{
		for (const std::filesystem::path& file : stdx::find_files(root, pattern, settings.ignore_globs))
		{
			add_file(file);
		}
	}

	if (roblox && settings.rojo.enabled)
	{
		for (const std::filesystem::path& folder : rojo::get_project_paths(root, settings.rojo))
		{
			std::error_code error;

			if (std::filesystem::is_regular_file(folder, error))
			{
				add_file(folder);
				continue;
			}

			for (std::string_view pattern : patterns)
			{
				for (const std::filesystem::path& file : stdx::find_files(folder, pattern, settings.features.import_ignore_globs))
				{
					add_file(file);
				}
			}
		}
	}

	frontend.parseModules(names);
}

std::vector<std::pair<Luau::ModuleName, Luau::Location>> workspace::find_references(
	const Luau::ModuleName& name,
	Luau::Position position,
	std::optional<Luau::Location>& origin,
	std::optional<Luau::Location>& declaration)
{
	std::vector<std::pair<Luau::ModuleName, Luau::Location>> references;
	check_module(name, false);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (source == nullptr || !module)
	{
		return references;
	}

	if (std::optional<renametargetinfo> target = priv::find_target(*source, module.get(), name, position, frontend.globals))
	{
		origin = target->origin;
		return find_named_references(*target);
	}

	std::optional<symbolinfo> found = symbolfinder::find(*module, *source, position);

	if (!found)
	{
		return references;
	}

	origin = found->origin;

	if (found->local != nullptr)
	{
		declaration = found->local->location;
		references.emplace_back(name, found->local->location);

		for (const Luau::Location& location : symbolfinder::find_local_uses(*source, found->local))
		{
			references.emplace_back(name, location);
		}

		return references;
	}

	if (!priv::is_renamable(*found))
	{
		return references;
	}

	index_workspace();

	std::set<Luau::ModuleName> candidates = {name};
	Luau::ModuleName definition = found->owner_module.empty() ? name : found->owner_module;
	candidates.insert(definition);

	if (found->by_property)
	{
		for (const auto& [module_name, node] : frontend.sourceNodes)
		{
			candidates.insert(module_name);
		}
	}
	else
	{
		frontend.traverseDependents(
			definition,
			[&candidates, &definition](Luau::SourceNode& node) { return node.name == definition || candidates.insert(node.name).second; });
	}

	std::set<std::tuple<Luau::ModuleName, unsigned int, unsigned int>> seen;

	for (const Luau::ModuleName& candidate : candidates)
	{
		check_module(candidate, false);

		const Luau::SourceModule* candidate_source = frontend.getSourceModule(candidate);
		Luau::ModulePtr candidate_module = get_module(candidate, false);

		if (candidate_source == nullptr || !candidate_module)
		{
			continue;
		}

		for (const Luau::Location& location : symbolfinder::find_member_uses(*candidate_module, *candidate_source, *found))
		{
			if (seen.insert({candidate, location.begin.line, location.begin.column}).second)
			{
				references.emplace_back(candidate, location);
			}
		}
	}

	return references;
}

std::vector<std::pair<Luau::ModuleName, Luau::Location>> workspace::find_named_references(const renametargetinfo& target)
{
	std::vector<std::pair<Luau::ModuleName, Luau::Location>> references;
	index_workspace();

	std::set<Luau::ModuleName> candidates;

	if (target.global)
	{
		for (const auto& [module_name, node] : frontend.sourceNodes)
		{
			candidates.insert(module_name);
		}
	}
	else
	{
		candidates.insert(target.module);

		if (target.exported)
		{
			frontend.traverseDependents(
				target.module,
				[&candidates, &target](Luau::SourceNode& node)
				{ return node.name == target.module || candidates.insert(node.name).second; });
		}
	}

	for (const Luau::ModuleName& candidate : candidates)
	{
		if (is_definition_file(candidate))
		{
			continue;
		}

		const Luau::SourceModule* source = frontend.getSourceModule(candidate);

		if (source == nullptr || source->root == nullptr)
		{
			continue;
		}

		priv::name_collector collected;
		collected.global = target.global;
		collected.name = target.name;
		source->root->visit(&collected);

		if (target.global || candidate == target.module)
		{
			for (const Luau::Location& location : collected.plain)
			{
				references.emplace_back(candidate, location);
			}
		}

		if (target.global || collected.prefixed.empty())
		{
			continue;
		}

		check_module(candidate, false);
		Luau::ModulePtr module = get_module(candidate, false);

		if (!module)
		{
			continue;
		}

		for (const auto& [prefix, location] : collected.prefixed)
		{
			if (priv::find_imported_module(*module, location.begin, prefix) == target.module)
			{
				references.emplace_back(candidate, location);
			}
		}
	}

	return references;
}

json workspace::get_references(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json locations = json::array();

	if (document == nullptr)
	{
		return locations;
	}

	bool include_declaration = params.value("/context/includeDeclaration"_json_pointer, true);
	Luau::ModuleName name = resolver.get_module_name(address);
	std::optional<Luau::Location> origin;
	std::optional<Luau::Location> declaration;
	std::vector<std::pair<Luau::ModuleName, Luau::Location>> references =
		find_references(name, document->to_luau_position(params["position"]), origin, declaration);

	for (const auto& [module_name, reference] : references)
	{
		if (!include_declaration && module_name == name && declaration && reference == *declaration)
		{
			continue;
		}

		json location = get_location(module_name, reference);

		if (!location.is_null())
		{
			locations.push_back(location);
		}
	}

	return locations;
}

json workspace::get_prepare_rename(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr)
	{
		return nullptr;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, false);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (source == nullptr || !module)
	{
		return nullptr;
	}

	Luau::Position position = document->to_luau_position(params["position"]);

	if (std::optional<renametargetinfo> target = priv::find_target(*source, module.get(), name, position, frontend.globals))
	{
		return {{"range", document->to_lsp_range(target->origin)}, {"placeholder", target->name}};
	}

	std::optional<symbolinfo> found = symbolfinder::find(*module, *source, position);

	if (!found || (found->local == nullptr && !priv::is_renamable(*found)))
	{
		return nullptr;
	}

	return {{"range", document->to_lsp_range(found->origin)}, {"placeholder", found->name}};
}

json workspace::get_rename(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	std::string next_name = params.value("newName", "");
	const textdocument* document = resolver.get_document(address);

	if (!priv::is_identifier(next_name))
	{
		throw std::runtime_error("'" + next_name + "' is not a valid Luau name");
	}

	if (document == nullptr)
	{
		return nullptr;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	std::optional<Luau::Location> origin;
	std::optional<Luau::Location> declaration;
	std::vector<std::pair<Luau::ModuleName, Luau::Location>> references =
		find_references(name, document->to_luau_position(params["position"]), origin, declaration);

	if (references.empty())
	{
		throw std::runtime_error("nothing to rename here");
	}

	json changes = json::object();
	std::set<std::tuple<std::string, unsigned int, unsigned int>> seen;

	for (const auto& [module_name, location] : references)
	{
		json found = get_location(module_name, location);

		if (found.is_null() || !seen.insert({found["uri"].get<std::string>(), location.begin.line, location.begin.column}).second)
		{
			continue;
		}

		changes[found["uri"].get<std::string>()].push_back({{"range", found["range"]}, {"newText", next_name}});
	}

	return {{"changes", changes}};
}

namespace
{
namespace priv
{

bool is_identifier(const std::string& name)
{
	if (name.empty() || reserved_words.find(name) != reserved_words.end())
	{
		return false;
	}

	if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
	{
		return false;
	}

	return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

bool is_renamable(const symbolinfo& symbol)
{
	return symbol.member && !stdx::starts_with(symbol.owner_module, "@");
}

bool name_collector::visit(Luau::AstType* node)
{
	return !global;
}

bool name_collector::visit(Luau::AstTypePack* node)
{
	return !global;
}

bool name_collector::visit(Luau::AstTypeReference* node)
{
	if (global || name != node->name.value)
	{
		return true;
	}

	if (node->prefix)
	{
		prefixed.emplace_back(node->prefix->value, node->nameLocation);
	}
	else
	{
		plain.push_back(node->nameLocation);
	}

	return true;
}

bool name_collector::visit(Luau::AstStatTypeAlias* node)
{
	if (!global && name == node->name.value)
	{
		plain.push_back(node->nameLocation);
	}

	return true;
}

bool name_collector::visit(Luau::AstExprGlobal* node)
{
	if (global && name == node->name.value)
	{
		plain.push_back(node->location);
	}

	return true;
}

std::optional<renametargetinfo> find_target(
	const Luau::SourceModule& source,
	const Luau::Module* module,
	const Luau::ModuleName& name,
	Luau::Position position,
	const Luau::GlobalTypes& globals)
{
	for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(source, position, true))
	{
		if (Luau::AstStatTypeAlias* alias = node->as<Luau::AstStatTypeAlias>();
			alias != nullptr && alias->nameLocation.containsClosed(position))
		{
			return renametargetinfo{alias->name.value, alias->nameLocation, name, false, alias->exported};
		}

		if (Luau::AstTypeReference* reference = node->as<Luau::AstTypeReference>();
			reference != nullptr && reference->nameLocation.containsClosed(position))
		{
			std::string type_name = reference->name.value;

			if (!reference->prefix)
			{
				if (!declares_type(source, type_name))
				{
					return std::nullopt;
				}

				return renametargetinfo{type_name, reference->nameLocation, name, false, true};
			}

			std::optional<Luau::ModuleName> imported =
				module != nullptr ? find_imported_module(*module, position, reference->prefix->value) : std::nullopt;

			if (!imported)
			{
				return std::nullopt;
			}

			return renametargetinfo{type_name, reference->nameLocation, *imported, false, true};
		}

		if (Luau::AstExprGlobal* global = node->as<Luau::AstExprGlobal>(); global != nullptr && global->location.containsClosed(position))
		{
			Luau::AstName known = globals.globalNames.names->get(global->name.value);

			if (known.value != nullptr && globals.globalScope->bindings.count(Luau::Symbol(known)) != 0)
			{
				return std::nullopt;
			}

			return renametargetinfo{global->name.value, global->location, name, true, true};
		}
	}

	return std::nullopt;
}

std::optional<Luau::ModuleName> find_imported_module(const Luau::Module& module, Luau::Position position, const std::string& prefix)
{
	for (Luau::ScopePtr scope = Luau::findScopeAtPosition(module, position); scope != nullptr; scope = scope->parent)
	{
		const auto found = scope->importedModules.find(prefix);

		if (found != scope->importedModules.end())
		{
			return found->second;
		}
	}

	return std::nullopt;
}

bool declares_type(const Luau::SourceModule& source, const std::string& name)
{
	name_collector collected;
	collected.global = false;
	collected.name = name;
	source.root->visit(&collected);

	for (Luau::AstStat* statement : source.root->body)
	{
		if (Luau::AstStatTypeAlias* alias = statement->as<Luau::AstStatTypeAlias>(); alias != nullptr && name == alias->name.value)
		{
			return true;
		}
	}

	return false;
}

}
}

}
