#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view show_references = "lunaut.showReferences";

struct function_collector final : Luau::AstVisitor
{
	bool visit(Luau::AstStatLocalFunction* node) override;
	bool visit(Luau::AstStatFunction* node) override;

	std::vector<Luau::Location> found;
};

std::string make_title(size_t count);

}
}

json workspace::get_code_lenses(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json lenses = json::array();

	if (document == nullptr || !settings.features.code_lens)
	{
		return lenses;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	frontend.parse(name);

	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source == nullptr || source->root == nullptr)
	{
		return lenses;
	}

	priv::function_collector collected;
	source->root->visit(&collected);

	for (const Luau::Location& location : collected.found)
	{
		json range = document->to_lsp_range(location);
		lenses.push_back({{"range", range}, {"data", {{"uri", address}, {"position", range["start"]}}}});
	}

	return lenses;
}

json workspace::resolve_code_lens(const json& lens)
{
	json resolved = lens;
	std::string address = lens.value("/data/uri"_json_pointer, "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr)
	{
		resolved["command"] = {{"title", priv::make_title(0)}, {"command", ""}};
		return resolved;
	}

	json start = lens["data"]["position"];
	Luau::Position position = document->to_luau_position(start);
	Luau::ModuleName name = resolver.get_module_name(address);
	std::optional<Luau::Location> origin;
	std::optional<Luau::Location> declaration;
	json locations = json::array();

	for (const auto& [module_name, reference] : find_references(name, position, origin, declaration))
	{
		if (module_name == name && reference.begin == position)
		{
			continue;
		}

		json location = get_location(module_name, reference);

		if (!location.is_null())
		{
			locations.push_back(location);
		}
	}

	resolved["command"] = {
		{"title", priv::make_title(locations.size())},
		{"command", priv::show_references},
		{"arguments", {address, start, locations}},
	};
	return resolved;
}

namespace
{
namespace priv
{

bool function_collector::visit(Luau::AstStatLocalFunction* node)
{
	found.push_back(node->name->location);
	return true;
}

bool function_collector::visit(Luau::AstStatFunction* node)
{
	if (Luau::AstExprIndexName* index = node->name->as<Luau::AstExprIndexName>())
	{
		found.push_back(index->indexLocation);
	}
	else if (Luau::AstExprLocal* local = node->name->as<Luau::AstExprLocal>())
	{
		found.push_back(local->location);
	}

	return true;
}

std::string make_title(size_t count)
{
	return std::to_string(count) + (count == 1 ? " reference" : " references");
}

}
}

}
