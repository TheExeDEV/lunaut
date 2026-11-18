#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"

namespace lunaut
{

json workspace::get_selection_ranges(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json ranges = json::array();

	if (document == nullptr || !params.contains("positions"))
	{
		return ranges;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	frontend.parse(name);

	const Luau::SourceModule* source = frontend.getSourceModule(name);

	for (const json& lsp_position : params["positions"])
	{
		Luau::Position position = document->to_luau_position(lsp_position);
		json selection = {{"range", {{"start", lsp_position}, {"end", lsp_position}}}};

		if (source == nullptr || source->root == nullptr)
		{
			ranges.push_back(selection);
			continue;
		}

		std::optional<Luau::Location> previous;
		json chain = nullptr;

		for (Luau::AstNode* node : Luau::findAstAncestryOfPosition(*source, position, true))
		{
			if (previous && *previous == node->location)
			{
				continue;
			}

			previous = node->location;
			json next = {{"range", document->to_lsp_range(node->location)}};

			if (!chain.is_null())
			{
				next["parent"] = std::move(chain);
			}

			chain = std::move(next);
		}

		ranges.push_back(chain.is_null() ? selection : chain);
	}

	return ranges;
}

}
