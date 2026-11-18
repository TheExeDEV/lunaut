#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

namespace lunaut
{

json workspace::get_implementations(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json locations = json::array();

	if (document == nullptr)
	{
		return locations;
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	Luau::ModuleName name = resolver.get_module_name(address);
	std::optional<Luau::Location> origin;
	std::optional<Luau::Location> declaration;
	std::vector<std::pair<Luau::ModuleName, Luau::Location>> references = find_references(name, position, origin, declaration);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (references.empty() || source == nullptr || !module)
	{
		return locations;
	}

	std::optional<symbolinfo> symbol = symbolfinder::find(*module, *source, position);

	if (!symbol)
	{
		return locations;
	}

	std::set<Luau::ModuleName> modules;

	for (const auto& [module_name, location] : references)
	{
		modules.insert(module_name);
	}

	for (const Luau::ModuleName& module_name : modules)
	{
		const Luau::SourceModule* candidate_source = frontend.getSourceModule(module_name);
		Luau::ModulePtr candidate_module = get_module(module_name, false);

		if (candidate_source == nullptr || !candidate_module)
		{
			continue;
		}

		for (const Luau::Location& write : symbolfinder::find_writes(*candidate_module, *candidate_source, *symbol))
		{
			json location = get_location(module_name, write);

			if (!location.is_null())
			{
				locations.push_back(location);
			}
		}
	}

	return locations;
}

}
