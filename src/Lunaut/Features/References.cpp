#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

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

bool is_identifier(const std::string& name);
bool is_renamable(const symbolinfo& symbol);

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

	for (std::string_view pattern : patterns)
	{
		for (const std::filesystem::path& file : stdx::find_files(root, pattern, settings.ignore_globs))
		{
			std::string path = uri::normalize_path(file.generic_string());

			if (is_definition_file(path) || stdx::ends_with(path, ".d.luau"))
			{
				continue;
			}

			names.push_back(resolver.get_module_name(uri::from_path(path)));
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

	std::optional<symbolinfo> found = symbolfinder::find(*module, *source, document->to_luau_position(params["position"]));

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

}
}

}
