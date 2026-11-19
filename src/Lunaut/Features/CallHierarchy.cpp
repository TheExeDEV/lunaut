#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"
#include "Luau/AstQuery.h"

#include <algorithm>

namespace lunaut
{
namespace
{
namespace priv
{

struct function_site
{
	Luau::AstExprFunction* function = nullptr;
	std::string name;
	Luau::Location selection;
};

struct function_finder final : Luau::AstVisitor
{
	bool visit(Luau::AstStatLocalFunction* node) override;
	bool visit(Luau::AstStatFunction* node) override;
	bool visit(Luau::AstStatLocal* node) override;
	bool visit(Luau::AstExprTable* node) override;

	std::vector<function_site> sites;
};

struct call_collector final : Luau::AstVisitor
{
	explicit call_collector(const Luau::AstExprFunction* root);

	bool visit(Luau::AstExprFunction* node) override;
	bool visit(Luau::AstExprCall* node) override;

	const Luau::AstExprFunction* root;
	std::vector<Luau::AstExprCall*> calls;
};

using definition = std::pair<Luau::ModuleName, Luau::Location>;

std::vector<function_site> get_sites(const Luau::SourceModule& source);
std::optional<function_site> find_site(const Luau::SourceModule& source, const Luau::Location& location);
function_site find_enclosing(const Luau::SourceModule& source, const Luau::Position& position);
Luau::Location get_callee_location(const Luau::AstExprCall* call);
std::optional<definition> find_member_definition(
	const Luau::Module& module, const Luau::AstExpr* object, const std::string& name, const Luau::ModuleName& current);
std::optional<definition> find_callee_definition(
	const Luau::Module& module, const Luau::AstExprCall* call, const Luau::ModuleName& current);
std::string get_function_name(Luau::AstExpr* expr);

}
}

json workspace::get_call_hierarchy_items(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr)
	{
		return nullptr;
	}

	Luau::Position position = document->to_luau_position(params["position"]);
	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, false);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (source == nullptr || !module)
	{
		return nullptr;
	}

	std::optional<symbolinfo> found = symbolfinder::find(*module, *source, position);

	if (!found)
	{
		return nullptr;
	}

	std::optional<priv::definition> target;

	if (found->local != nullptr)
	{
		target = priv::definition{name, found->local->location};
	}
	else
	{
		Luau::AstExpr* expr = Luau::findExprAtPosition(*source, position);
		Luau::AstExprIndexName* index = expr != nullptr ? expr->as<Luau::AstExprIndexName>() : nullptr;

		if (index != nullptr)
		{
			target = priv::find_member_definition(*module, index->expr, found->name, name);
		}

		if (!target)
		{
			target = priv::definition{name, found->origin};
		}
	}

	const Luau::SourceModule* target_source = target->first == name ? source : frontend.getSourceModule(target->first);
	std::optional<priv::function_site> site = target_source != nullptr ? priv::find_site(*target_source, target->second) : std::nullopt;
	std::optional<textdocument> text = get_text_for(target->first);

	if (!site || !text)
	{
		return nullptr;
	}

	return json::array({{
		{"name", site->name},
		{"kind", static_cast<int>(enumitem_symbol_kind::Function)},
		{"uri", text->get_uri()},
		{"detail", resolver.getHumanReadableModuleName(target->first)},
		{"range", text->to_lsp_range(site->function->location)},
		{"selectionRange", text->to_lsp_range(site->selection)},
		{"data", {{"module", target->first}, {"line", site->selection.begin.line}, {"column", site->selection.begin.column}}},
	}});
}

json workspace::get_incoming_calls(const json& params)
{
	const json& item = params["item"];
	Luau::ModuleName name = item["data"].value("module", "");
	Luau::Position position{item["data"].value("line", 0u), item["data"].value("column", 0u)};
	std::optional<Luau::Location> origin;
	std::optional<Luau::Location> declaration;
	std::vector<std::pair<Luau::ModuleName, Luau::Location>> references = find_references(name, position, origin, declaration);

	std::map<std::tuple<Luau::ModuleName, unsigned int, unsigned int>, json> grouped;

	for (const auto& [caller, location] : references)
	{
		const Luau::SourceModule* source = frontend.getSourceModule(caller);
		std::optional<textdocument> text = get_text_for(caller);

		if (source == nullptr || source->root == nullptr || !text)
		{
			continue;
		}

		priv::call_collector calls(nullptr);
		source->root->visit(&calls);

		bool called = std::any_of(
			calls.calls.begin(),
			calls.calls.end(),
			[&location](const Luau::AstExprCall* call) { return priv::get_callee_location(call) == location; });

		if (!called)
		{
			continue;
		}

		priv::function_site site = priv::find_enclosing(*source, location.begin);
		std::tuple<Luau::ModuleName, unsigned int, unsigned int> key{caller, site.selection.begin.line, site.selection.begin.column};
		auto existing = grouped.find(key);

		if (existing == grouped.end())
		{
			Luau::Location range = site.function != nullptr ? site.function->location : site.selection;
			json from = {
				{"name", site.name.empty() ? resolver.getHumanReadableModuleName(caller) : site.name},
				{"kind", static_cast<int>(site.function != nullptr ? enumitem_symbol_kind::Function : enumitem_symbol_kind::Module)},
				{"uri", text->get_uri()},
				{"detail", resolver.getHumanReadableModuleName(caller)},
				{"range", text->to_lsp_range(range)},
				{"selectionRange", text->to_lsp_range(site.selection)},
				{"data", {{"module", caller}, {"line", site.selection.begin.line}, {"column", site.selection.begin.column}}},
			};
			existing = grouped.emplace(key, json{{"from", from}, {"fromRanges", json::array()}}).first;
		}

		existing->second["fromRanges"].push_back(text->to_lsp_range(location));
	}

	json result = json::array();

	for (auto& [key, call] : grouped)
	{
		result.push_back(std::move(call));
	}

	return result;
}

json workspace::get_outgoing_calls(const json& params)
{
	const json& item = params["item"];
	Luau::ModuleName name = item["data"].value("module", "");
	Luau::Position position{item["data"].value("line", 0u), item["data"].value("column", 0u)};
	check_module(name, false);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);
	std::optional<textdocument> caller_text = get_text_for(name);

	if (source == nullptr || !module || !caller_text)
	{
		return json::array();
	}

	std::optional<priv::function_site> site = priv::find_site(*source, Luau::Location{position, position});

	if (!site)
	{
		return json::array();
	}

	priv::call_collector calls(site->function);
	site->function->visit(&calls);

	std::map<std::tuple<Luau::ModuleName, unsigned int, unsigned int>, json> grouped;

	for (const Luau::AstExprCall* call : calls.calls)
	{
		std::optional<priv::definition> target = priv::find_callee_definition(*module, call, name);

		if (!target)
		{
			continue;
		}

		const Luau::SourceModule* target_source = target->first == name ? source : frontend.getSourceModule(target->first);
		std::optional<priv::function_site> callee =
			target_source != nullptr ? priv::find_site(*target_source, target->second) : std::nullopt;
		std::optional<textdocument> text = callee ? get_text_for(target->first) : std::nullopt;

		if (!callee || !text)
		{
			continue;
		}

		std::tuple<Luau::ModuleName, unsigned int, unsigned int> key{
			target->first, callee->selection.begin.line, callee->selection.begin.column};
		auto existing = grouped.find(key);

		if (existing == grouped.end())
		{
			json to = {
				{"name", callee->name},
				{"kind", static_cast<int>(enumitem_symbol_kind::Function)},
				{"uri", text->get_uri()},
				{"detail", resolver.getHumanReadableModuleName(target->first)},
				{"range", text->to_lsp_range(callee->function->location)},
				{"selectionRange", text->to_lsp_range(callee->selection)},
				{"data", {{"module", target->first}, {"line", callee->selection.begin.line}, {"column", callee->selection.begin.column}}},
			};
			existing = grouped.emplace(key, json{{"to", to}, {"fromRanges", json::array()}}).first;
		}

		existing->second["fromRanges"].push_back(caller_text->to_lsp_range(priv::get_callee_location(call)));
	}

	json result = json::array();

	for (auto& [key, call] : grouped)
	{
		result.push_back(std::move(call));
	}

	return result;
}

namespace
{
namespace priv
{

bool function_finder::visit(Luau::AstStatLocalFunction* node)
{
	sites.push_back({node->func, node->name->name.value, node->name->location});
	return true;
}

bool function_finder::visit(Luau::AstStatFunction* node)
{
	Luau::AstExprIndexName* index = node->name->as<Luau::AstExprIndexName>();
	sites.push_back({node->func, get_function_name(node->name), index != nullptr ? index->indexLocation : node->name->location});
	return true;
}

bool function_finder::visit(Luau::AstStatLocal* node)
{
	for (size_t i = 0; i < node->vars.size && i < node->values.size; ++i)
	{
		if (Luau::AstExprFunction* function = node->values.data[i]->as<Luau::AstExprFunction>())
		{
			sites.push_back({function, node->vars.data[i]->name.value, node->vars.data[i]->location});
		}
	}

	return true;
}

bool function_finder::visit(Luau::AstExprTable* node)
{
	for (const Luau::AstExprTable::Item& item : node->items)
	{
		Luau::AstExprConstantString* key = item.key != nullptr ? item.key->as<Luau::AstExprConstantString>() : nullptr;
		Luau::AstExprFunction* function = item.value->as<Luau::AstExprFunction>();

		if (item.kind == Luau::AstExprTable::Item::Kind::Record && key != nullptr && function != nullptr)
		{
			sites.push_back({function, std::string(key->value.data, key->value.size), key->location});
		}
	}

	return true;
}

call_collector::call_collector(const Luau::AstExprFunction* root)
	: root(root)
{
}

bool call_collector::visit(Luau::AstExprFunction* node)
{
	return root == nullptr || node == root;
}

bool call_collector::visit(Luau::AstExprCall* node)
{
	calls.push_back(node);
	return true;
}

std::vector<function_site> get_sites(const Luau::SourceModule& source)
{
	function_finder finder;

	if (source.root != nullptr)
	{
		source.root->visit(&finder);
	}

	return finder.sites;
}

std::optional<function_site> find_site(const Luau::SourceModule& source, const Luau::Location& location)
{
	for (const function_site& site : get_sites(source))
	{
		if (site.selection.containsClosed(location.begin) || site.selection.begin == location.begin)
		{
			return site;
		}
	}

	for (const function_site& site : get_sites(source))
	{
		if (site.function->location.begin.line == location.begin.line && site.function->location.containsClosed(location.begin))
		{
			return site;
		}
	}

	return std::nullopt;
}

function_site find_enclosing(const Luau::SourceModule& source, const Luau::Position& position)
{
	std::optional<function_site> best;

	for (const function_site& site : get_sites(source))
	{
		if (!site.function->location.containsClosed(position))
		{
			continue;
		}

		if (!best || best->function->location.encloses(site.function->location))
		{
			best = site;
		}
	}

	if (best)
	{
		return *best;
	}

	return function_site{nullptr, "", Luau::Location{Luau::Position{0, 0}, Luau::Position{0, 0}}};
}

Luau::Location get_callee_location(const Luau::AstExprCall* call)
{
	if (const Luau::AstExprIndexName* index = call->func->as<Luau::AstExprIndexName>())
	{
		return index->indexLocation;
	}

	return call->func->location;
}

std::optional<definition> find_member_definition(
	const Luau::Module& module, const Luau::AstExpr* object, const std::string& name, const Luau::ModuleName& current)
{
	const Luau::TypeId* owner = module.astTypes.find(object);
	const Luau::TableType* table = owner != nullptr ? Luau::get<Luau::TableType>(Luau::follow(*owner)) : nullptr;

	if (table == nullptr)
	{
		return std::nullopt;
	}

	const auto property = table->props.find(name);

	if (property == table->props.end() || !property->second.location)
	{
		return std::nullopt;
	}

	return definition{table->definitionModuleName.empty() ? current : table->definitionModuleName, *property->second.location};
}

std::optional<definition> find_callee_definition(const Luau::Module& module, const Luau::AstExprCall* call, const Luau::ModuleName& current)
{
	if (const Luau::AstExprLocal* local = call->func->as<Luau::AstExprLocal>())
	{
		return definition{current, local->local->location};
	}

	if (const Luau::AstExprIndexName* index = call->func->as<Luau::AstExprIndexName>())
	{
		return find_member_definition(module, index->expr, index->index.value, current);
	}

	return std::nullopt;
}

std::string get_function_name(Luau::AstExpr* expr)
{
	if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
	{
		return global->name.value;
	}

	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return local->local->name.value;
	}

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		std::string base = get_function_name(index->expr);
		return base.empty() ? std::string(index->index.value) : base + index->op + index->index.value;
	}

	return {};
}

}
}

}
