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

struct ownerkey
{
	Luau::ModuleName module;
	Luau::Location location;
	bool by_property = false;

	bool operator==(const ownerkey& other) const;
};

struct target
{
	Luau::AstLocal* local = nullptr;
	std::string name;
	std::optional<ownerkey> owner;
	Luau::Location origin;
};

struct local_collector final : Luau::AstVisitor
{
	explicit local_collector(Luau::AstLocal* local);

	bool visit(Luau::AstExprLocal* node) override;

	Luau::AstLocal* local;
	std::vector<Luau::Location> found;
};

struct property_collector final : Luau::AstVisitor
{
	property_collector(const Luau::Module& module, std::string name, ownerkey owner);

	bool visit(Luau::AstExprIndexName* node) override;
	bool visit(Luau::AstExprTable* node) override;

	const Luau::Module& module;
	std::string name;
	ownerkey owner;
	std::vector<Luau::Location> found;
};

std::optional<ownerkey> get_owner(const Luau::Module& module, const Luau::AstExpr* object, const std::string& name);
std::optional<target> find_target(const Luau::Module& module, const Luau::SourceModule& source, Luau::Position position);
bool is_identifier(const std::string& name);

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

	std::optional<priv::target> found = priv::find_target(*module, *source, position);

	if (!found)
	{
		return references;
	}

	origin = found->origin;

	if (found->local != nullptr)
	{
		priv::local_collector collected(found->local);
		source->root->visit(&collected);
		declaration = found->local->location;
		references.emplace_back(name, found->local->location);

		for (const Luau::Location& location : collected.found)
		{
			references.emplace_back(name, location);
		}

		return references;
	}

	if (!found->owner || stdx::starts_with(found->owner->module, "@"))
	{
		return references;
	}

	index_workspace();

	std::set<Luau::ModuleName> candidates = {name};
	Luau::ModuleName definition = found->owner->module.empty() ? name : found->owner->module;
	candidates.insert(definition);

	if (found->owner->by_property)
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

		if (candidate_source == nullptr || !candidate_module || candidate_source->root == nullptr)
		{
			continue;
		}

		priv::property_collector collected(*candidate_module, found->name, *found->owner);
		candidate_source->root->visit(&collected);

		for (const Luau::Location& location : collected.found)
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

	std::optional<priv::target> found = priv::find_target(*module, *source, document->to_luau_position(params["position"]));

	if (!found || (found->local == nullptr && (!found->owner || stdx::starts_with(found->owner->module, "@"))))
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

bool ownerkey::operator==(const ownerkey& other) const
{
	return by_property == other.by_property && module == other.module && location == other.location;
}

local_collector::local_collector(Luau::AstLocal* local)
	: local(local)
{
}

bool local_collector::visit(Luau::AstExprLocal* node)
{
	if (node->local == local)
	{
		found.push_back(node->location);
	}

	return true;
}

property_collector::property_collector(const Luau::Module& module, std::string name, ownerkey owner)
	: module(module)
	, name(std::move(name))
	, owner(std::move(owner))
{
}

bool property_collector::visit(Luau::AstExprIndexName* node)
{
	if (name == node->index.value)
	{
		std::optional<ownerkey> found = get_owner(module, node->expr, name);

		if (found && *found == owner)
		{
			this->found.push_back(node->indexLocation);
		}
	}

	return true;
}

bool property_collector::visit(Luau::AstExprTable* node)
{
	for (const Luau::AstExprTable::Item& item : node->items)
	{
		if (item.kind != Luau::AstExprTable::Item::Kind::Record || item.key == nullptr)
		{
			continue;
		}

		Luau::AstExprConstantString* key = item.key->as<Luau::AstExprConstantString>();

		if (key == nullptr || std::string(key->value.data, key->value.size) != name)
		{
			continue;
		}

		std::optional<ownerkey> found = get_owner(module, node, name);

		if (found && *found == owner)
		{
			this->found.push_back(key->location);
		}
	}

	return true;
}

std::optional<ownerkey> get_owner(const Luau::Module& module, const Luau::AstExpr* object, const std::string& name)
{
	const Luau::TypeId* type = module.astTypes.find(object);

	if (type == nullptr)
	{
		return std::nullopt;
	}

	Luau::TypeId followed = Luau::follow(*type);
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

	if (!table->definitionModuleName.empty())
	{
		return ownerkey{table->definitionModuleName, table->definitionLocation, false};
	}

	const auto property = table->props.find(name);

	if (property != table->props.end() && property->second.location)
	{
		return ownerkey{"", *property->second.location, true};
	}

	return std::nullopt;
}

std::optional<target> find_target(const Luau::Module& module, const Luau::SourceModule& source, Luau::Position position)
{
	Luau::ExprOrLocal hit = Luau::findExprOrLocalAtPosition(source, position);

	if (Luau::AstLocal* local = hit.getLocal())
	{
		return target{local, local->name.value, std::nullopt, local->location};
	}

	Luau::AstExpr* expr = hit.getExpr();

	if (expr == nullptr)
	{
		return std::nullopt;
	}

	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return target{local->local, local->local->name.value, std::nullopt, local->location};
	}

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		if (!index->indexLocation.containsClosed(position))
		{
			return std::nullopt;
		}

		std::string name = index->index.value;
		return target{nullptr, name, get_owner(module, index->expr, name), index->indexLocation};
	}

	Luau::AstExprConstantString* key = expr->as<Luau::AstExprConstantString>();

	if (key == nullptr)
	{
		return std::nullopt;
	}

	for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(source, position))
	{
		Luau::AstExprTable* table = node->as<Luau::AstExprTable>();

		if (table == nullptr)
		{
			continue;
		}

		for (const Luau::AstExprTable::Item& item : table->items)
		{
			if (item.kind == Luau::AstExprTable::Item::Kind::Record && item.key == key)
			{
				std::string name(key->value.data, key->value.size);
				return target{nullptr, name, get_owner(module, table, name), key->location};
			}
		}
	}

	return std::nullopt;
}

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

}
}

}
