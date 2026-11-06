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

constexpr int kind_read = 2;
constexpr int kind_write = 3;

}
}

json workspace::get_document_highlights(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json highlights = json::array();

	if (document == nullptr)
	{
		return highlights;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, false);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (source == nullptr || !module)
	{
		return highlights;
	}

	std::optional<symbolinfo> found = symbolfinder::find(*module, *source, document->to_luau_position(params["position"]));

	if (!found)
	{
		return highlights;
	}

	if (found->local != nullptr)
	{
		highlights.push_back({{"range", document->to_lsp_range(found->local->location)}, {"kind", priv::kind_write}});

		for (const Luau::Location& location : symbolfinder::find_local_uses(*source, found->local))
		{
			highlights.push_back({{"range", document->to_lsp_range(location)}, {"kind", priv::kind_read}});
		}

		return highlights;
	}

	if (!found->member)
	{
		return highlights;
	}

	for (const Luau::Location& location : symbolfinder::find_member_uses(*module, *source, *found))
	{
		highlights.push_back({{"range", document->to_lsp_range(location)}, {"kind", priv::kind_read}});
	}

	return highlights;
}

}
