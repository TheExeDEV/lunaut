#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"

#include <algorithm>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr size_t limit = 1000;

struct declaration
{
	std::string name;
	enumitem_symbol_kind kind;
	Luau::Location location;
};

struct declaration_collector final : Luau::AstVisitor
{
	explicit declaration_collector(const Luau::AstStatBlock* root);

	bool visit(Luau::AstStatLocalFunction* node) override;
	bool visit(Luau::AstStatFunction* node) override;
	bool visit(Luau::AstStatLocal* node) override;
	bool visit(Luau::AstStatTypeAlias* node) override;

	const Luau::AstStatBlock* root;
	std::vector<declaration> found;
};

std::string get_function_name(Luau::AstExpr* expr);
bool matches(const std::string& query, const std::string& name);

}
}

json workspace::get_workspace_symbols(const json& params)
{
	std::string query = stdx::to_lower(params.value("query", ""));
	json symbols = json::array();

	index_workspace();

	std::vector<Luau::ModuleName> modules;

	for (const auto& [module_name, node] : frontend.sourceNodes)
	{
		modules.push_back(module_name);
	}

	std::sort(modules.begin(), modules.end());

	for (const Luau::ModuleName& module_name : modules)
	{
		std::optional<std::string> real = resolver.get_real_path(module_name);

		if (!real || is_definition_file(*real) || is_ignored(*real))
		{
			continue;
		}

		const Luau::SourceModule* source = frontend.getSourceModule(module_name);

		if (source == nullptr || source->root == nullptr)
		{
			continue;
		}

		priv::declaration_collector collected(source->root);
		source->root->visit(&collected);
		std::optional<textdocument> text;

		for (const priv::declaration& found : collected.found)
		{
			if (!priv::matches(query, found.name))
			{
				continue;
			}

			if (!text)
			{
				text = get_text_for(module_name);
			}

			if (!text)
			{
				break;
			}

			symbols.push_back({
				{"name", found.name},
				{"kind", static_cast<int>(found.kind)},
				{"location", {{"uri", text->get_uri()}, {"range", text->to_lsp_range(found.location)}}},
				{"containerName", resolver.getHumanReadableModuleName(module_name)},
			});

			if (symbols.size() >= priv::limit)
			{
				return symbols;
			}
		}
	}

	return symbols;
}

namespace
{
namespace priv
{

declaration_collector::declaration_collector(const Luau::AstStatBlock* root)
	: root(root)
{
}

bool declaration_collector::visit(Luau::AstStatLocalFunction* node)
{
	found.push_back({node->name->name.value, enumitem_symbol_kind::Function, node->name->location});
	return true;
}

bool declaration_collector::visit(Luau::AstStatFunction* node)
{
	std::string name = get_function_name(node->name);
	Luau::AstExprIndexName* index = node->name->as<Luau::AstExprIndexName>();

	if (!name.empty())
	{
		found.push_back(
			{name,
			 index != nullptr && index->op == ':' ? enumitem_symbol_kind::Method : enumitem_symbol_kind::Function,
			 index != nullptr ? index->indexLocation : node->name->location});
	}

	return true;
}

bool declaration_collector::visit(Luau::AstStatLocal* node)
{
	if (std::find(root->body.begin(), root->body.end(), node) == root->body.end())
	{
		return true;
	}

	for (Luau::AstLocal* local : node->vars)
	{
		found.push_back(
			{local->name.value, node->isConst ? enumitem_symbol_kind::Constant : enumitem_symbol_kind::Variable, local->location});
	}

	return true;
}

bool declaration_collector::visit(Luau::AstStatTypeAlias* node)
{
	found.push_back({node->name.value, enumitem_symbol_kind::Struct, node->nameLocation});
	return true;
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

bool matches(const std::string& query, const std::string& name)
{
	std::string lowered = stdx::to_lower(name);
	size_t position = 0;

	for (char c : query)
	{
		position = lowered.find(c, position);

		if (position == std::string::npos)
		{
			return false;
		}

		++position;
	}

	return true;
}

}
}

}
