#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"

namespace lunaut
{
namespace
{
namespace priv
{

struct require_collector final : Luau::AstVisitor
{
	bool visit(Luau::AstExprCall* node) override;

	std::vector<std::pair<Luau::Location, Luau::Location>> calls;
};

bool exists(const std::string& path);

}
}

json workspace::get_document_links(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json links = json::array();

	if (document == nullptr)
	{
		return links;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	frontend.parse(name);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	const auto node = frontend.sourceNodes.find(name);

	if (source == nullptr || source->root == nullptr || node == frontend.sourceNodes.end())
	{
		return links;
	}

	priv::require_collector collected;
	source->root->visit(&collected);

	for (const auto& [call, argument] : collected.calls)
	{
		for (const auto& [required, location] : node->second->requireLocations)
		{
			if (location != call)
			{
				continue;
			}

			std::optional<std::string> path = resolver.get_real_path(required);

			if (path && priv::exists(*path))
			{
				links.push_back({
					{"range", document->to_lsp_range(argument)},
					{"target", uri::from_path(*path)},
					{"tooltip", resolver.getHumanReadableModuleName(required)},
				});
			}

			break;
		}
	}

	return links;
}

namespace
{
namespace priv
{

bool require_collector::visit(Luau::AstExprCall* node)
{
	Luau::AstExprGlobal* global = node->func->as<Luau::AstExprGlobal>();

	if (global != nullptr && global->name == "require" && node->args.size >= 1)
	{
		calls.emplace_back(node->location, node->args.data[0]->location);
	}

	return true;
}

bool exists(const std::string& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

}
}

}
