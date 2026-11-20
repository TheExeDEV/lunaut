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

struct collector final : Luau::AstVisitor
{
	explicit collector(const textdocument& document, const Luau::AstStatBlock* root);

	bool visit(Luau::AstStatLocalFunction* node) override;
	bool visit(Luau::AstStatFunction* node) override;
	bool visit(Luau::AstStatLocal* node) override;
	bool visit(Luau::AstStatTypeAlias* node) override;

	void add(const std::string& name, enumitem_symbol_kind kind, const Luau::Location& range, const Luau::Location& selection);

	const textdocument& document;
	const Luau::AstStatBlock* root;
	json symbols = json::array();
};

std::string get_function_name(Luau::AstExpr* expr);

}
}

json workspace::get_document_symbols(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr)
	{
		return json::array();
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	frontend.parse(name);
	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source == nullptr || source->root == nullptr)
	{
		return json::array();
	}

	priv::collector collected(*document, source->root);
	source->root->visit(&collected);
	return collected.symbols;
}

namespace
{
namespace priv
{

collector::collector(const textdocument& document, const Luau::AstStatBlock* root)
	: document(document)
	, root(root)
{
}

bool collector::visit(Luau::AstStatLocalFunction* node)
{
	add(node->name->name.value, enumitem_symbol_kind::Function, node->location, node->name->location);
	return true;
}

bool collector::visit(Luau::AstStatFunction* node)
{
	Luau::AstExprIndexName* index = node->name->as<Luau::AstExprIndexName>();
	enumitem_symbol_kind kind = index != nullptr && index->op == ':' ? enumitem_symbol_kind::Method : enumitem_symbol_kind::Function;
	add(get_function_name(node->name), kind, node->location, node->name->location);
	return true;
}

bool collector::visit(Luau::AstStatLocal* node)
{
	bool top_level = false;

	for (Luau::AstStat* statement : root->body)
	{
		top_level = top_level || statement == node;
	}

	if (!top_level)
	{
		return true;
	}

	for (Luau::AstLocal* local : node->vars)
	{
		add(local->name.value,
			node->isConst ? enumitem_symbol_kind::Constant : enumitem_symbol_kind::Variable,
			node->location,
			local->location);
	}

	return true;
}

bool collector::visit(Luau::AstStatTypeAlias* node)
{
	add(node->name.value, enumitem_symbol_kind::Struct, node->location, node->nameLocation);
	return true;
}

void collector::add(const std::string& name, enumitem_symbol_kind kind, const Luau::Location& range, const Luau::Location& selection)
{
	if (name.empty() || name.front() == '%')
	{
		return;
	}

	bool contained = range.begin <= selection.begin && selection.end <= range.end;

	symbols.push_back({
		{"name", name},
		{"kind", static_cast<int>(kind)},
		{"range", document.to_lsp_range(range)},
		{"selectionRange", document.to_lsp_range(contained ? selection : range)},
	});
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
