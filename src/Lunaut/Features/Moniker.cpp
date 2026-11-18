#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view scheme = "luau";

json make_moniker(const std::string& identifier, std::string_view unique, std::string_view kind);
std::string make_place(const Luau::Location& location);
bool is_exported(const Luau::Module& module, const symbolinfo& symbol);
std::string get_identifier(const fileresolver& resolver, const std::filesystem::path& root, const Luau::ModuleName& name);

}
}

json workspace::get_monikers(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);
	json monikers = json::array();

	if (document == nullptr)
	{
		return monikers;
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, false);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (source == nullptr || !module)
	{
		return monikers;
	}

	Luau::AstExpr* expr = Luau::findExprOrLocalAtPosition(*source, position).getExpr();

	if (Luau::AstExprGlobal* global = expr ? expr->as<Luau::AstExprGlobal>() : nullptr)
	{
		monikers.push_back(priv::make_moniker(global->name.value, "global", "import"));
		return monikers;
	}

	std::optional<symbolinfo> symbol = symbolfinder::find(*module, *source, position);

	if (!symbol)
	{
		return monikers;
	}

	if (symbol->local != nullptr)
	{
		std::string identifier =
			priv::get_identifier(resolver, root, name) + "@" + priv::make_place(symbol->local->location) + ":" + symbol->name;
		monikers.push_back(priv::make_moniker(identifier, "document", "local"));
		return monikers;
	}

	if (!symbol->member || symbol->owner_module.empty())
	{
		monikers.push_back(priv::make_moniker(priv::get_identifier(resolver, root, name) + ":" + symbol->name, "document", "local"));
		return monikers;
	}

	Luau::ModulePtr owner = get_module(symbol->owner_module, false);
	std::string owner_identifier = priv::get_identifier(resolver, root, symbol->owner_module);

	if (owner && priv::is_exported(*owner, *symbol))
	{
		std::string_view kind = symbol->owner_module == name ? "export" : "import";
		monikers.push_back(priv::make_moniker(owner_identifier + "." + symbol->name, "project", kind));
		return monikers;
	}

	std::string identifier = owner_identifier + "@" + priv::make_place(symbol->owner_location) + "." + symbol->name;
	monikers.push_back(priv::make_moniker(identifier, symbol->owner_module == name ? "document" : "project", "local"));
	return monikers;
}

namespace
{
namespace priv
{

json make_moniker(const std::string& identifier, std::string_view unique, std::string_view kind)
{
	return {{"scheme", scheme}, {"identifier", identifier}, {"unique", unique}, {"kind", kind}};
}

std::string get_identifier(const fileresolver& resolver, const std::filesystem::path& root, const Luau::ModuleName& name)
{
	std::optional<std::string> path = resolver.get_real_path(name);

	if (!path || root.empty())
	{
		return name;
	}

	std::filesystem::path relative = std::filesystem::path(*path).lexically_relative(root);
	return relative.empty() ? name : relative.generic_string();
}

std::string make_place(const Luau::Location& location)
{
	return std::to_string(location.begin.line + 1) + ":" + std::to_string(location.begin.column + 1);
}

bool is_exported(const Luau::Module& module, const symbolinfo& symbol)
{
	std::optional<Luau::TypeId> returned = Luau::first(module.returnType);

	if (!returned)
	{
		return false;
	}

	Luau::TypeId followed = Luau::follow(*returned);

	if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
	{
		followed = Luau::follow(metatable->table);
	}

	const Luau::TableType* table = Luau::get<Luau::TableType>(followed);
	return table != nullptr && table->definitionModuleName == symbol.owner_module && table->definitionLocation == symbol.owner_location;
}

}
}

}
