#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"
#include "Luau/Scope.h"

#include <algorithm>

namespace lunaut
{
namespace
{
namespace priv
{

struct token
{
	unsigned int line;
	unsigned int column;
	unsigned int length;
	enumitem_token_type type;
	int modifiers;
};

struct token_collector final : Luau::AstVisitor
{
	token_collector(const Luau::Module& module, const textdocument& document);

	bool visit(Luau::AstStatLocal* node) override;
	bool visit(Luau::AstStatLocalFunction* node) override;
	bool visit(Luau::AstStatFor* node) override;
	bool visit(Luau::AstStatForIn* node) override;
	bool visit(Luau::AstStatTypeAlias* node) override;
	bool visit(Luau::AstExprFunction* node) override;
	bool visit(Luau::AstExprLocal* node) override;
	bool visit(Luau::AstExprGlobal* node) override;
	bool visit(Luau::AstExprIndexName* node) override;
	bool visit(Luau::AstTypeReference* node) override;

	void add(const Luau::Location& location, enumitem_token_type type, int modifiers);
	void add_local(Luau::AstLocal* local, enumitem_token_type type, int modifiers);
	bool is_function(const Luau::AstExpr* expr) const;
	bool is_builtin(const Luau::AstName& name) const;

	const Luau::Module& module;
	const textdocument& document;
	std::set<const Luau::AstLocal*> parameters;
	std::set<const Luau::AstLocal*> functions;
	std::vector<token> tokens;
};

int get_modifier(enumitem_token_modifier modifier);

}
}

json workspace::get_semantic_tokens(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr || !settings.features.semantic_tokens)
	{
		return {{"data", json::array()}};
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, false);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, false);

	if (source == nullptr || !module || source->root == nullptr)
	{
		return {{"data", json::array()}};
	}

	priv::token_collector collected(*module, *document);
	source->root->visit(&collected);

	std::sort(
		collected.tokens.begin(),
		collected.tokens.end(),
		[](const priv::token& a, const priv::token& b) { return a.line != b.line ? a.line < b.line : a.column < b.column; });

	json data = json::array();
	unsigned int line = 0;
	unsigned int column = 0;
	std::optional<std::pair<unsigned int, unsigned int>> previous;

	for (const priv::token& current : collected.tokens)
	{
		if (previous && previous->first == current.line && previous->second == current.column)
		{
			continue;
		}

		unsigned int delta_line = current.line - line;
		unsigned int delta_column = delta_line == 0 ? current.column - column : current.column;

		data.push_back(delta_line);
		data.push_back(delta_column);
		data.push_back(current.length);
		data.push_back(static_cast<int>(current.type));
		data.push_back(current.modifiers);

		line = current.line;
		column = current.column;
		previous = std::pair{current.line, current.column};
	}

	return {{"data", data}};
}

namespace
{
namespace priv
{

token_collector::token_collector(const Luau::Module& module, const textdocument& document)
	: module(module)
	, document(document)
{
}

bool token_collector::visit(Luau::AstStatLocal* node)
{
	for (size_t i = 0; i < node->vars.size; ++i)
	{
		Luau::AstLocal* local = node->vars.data[i];
		bool function = i < node->values.size && node->values.data[i]->is<Luau::AstExprFunction>();

		if (function)
		{
			functions.insert(local);
		}

		add_local(
			local,
			function ? enumitem_token_type::Function : enumitem_token_type::Variable,
			get_modifier(enumitem_token_modifier::Declaration) | (local->isConst ? get_modifier(enumitem_token_modifier::Readonly) : 0));
	}

	return true;
}

bool token_collector::visit(Luau::AstStatLocalFunction* node)
{
	functions.insert(node->name);
	add_local(node->name, enumitem_token_type::Function, get_modifier(enumitem_token_modifier::Declaration));
	return true;
}

bool token_collector::visit(Luau::AstStatFor* node)
{
	add_local(node->var, enumitem_token_type::Variable, get_modifier(enumitem_token_modifier::Declaration));
	return true;
}

bool token_collector::visit(Luau::AstStatForIn* node)
{
	for (Luau::AstLocal* local : node->vars)
	{
		add_local(local, enumitem_token_type::Variable, get_modifier(enumitem_token_modifier::Declaration));
	}

	return true;
}

bool token_collector::visit(Luau::AstStatTypeAlias* node)
{
	add(node->nameLocation, enumitem_token_type::Type, get_modifier(enumitem_token_modifier::Declaration));
	return true;
}

bool token_collector::visit(Luau::AstExprFunction* node)
{
	for (Luau::AstLocal* argument : node->args)
	{
		parameters.insert(argument);
		add_local(argument, enumitem_token_type::Parameter, get_modifier(enumitem_token_modifier::Declaration));
	}

	return true;
}

bool token_collector::visit(Luau::AstExprLocal* node)
{
	int modifiers = node->local->isConst ? get_modifier(enumitem_token_modifier::Readonly) : 0;

	if (parameters.count(node->local) != 0)
	{
		add(node->location, enumitem_token_type::Parameter, modifiers);
	}
	else if (functions.count(node->local) != 0 || is_function(node))
	{
		add(node->location, enumitem_token_type::Function, modifiers);
	}
	else
	{
		add(node->location, enumitem_token_type::Variable, modifiers);
	}

	return true;
}

bool token_collector::visit(Luau::AstExprGlobal* node)
{
	int modifiers = is_builtin(node->name) ? get_modifier(enumitem_token_modifier::DefaultLibrary) : 0;

	if (node->name == "Enum")
	{
		add(node->location, enumitem_token_type::Enum, modifiers);
	}
	else
	{
		add(node->location, is_function(node) ? enumitem_token_type::Function : enumitem_token_type::Variable, modifiers);
	}

	return true;
}

bool token_collector::visit(Luau::AstExprIndexName* node)
{
	enumitem_token_type type = enumitem_token_type::Property;
	Luau::AstExprIndexName* parent = node->expr->as<Luau::AstExprIndexName>();
	Luau::AstExprGlobal* root = parent != nullptr ? parent->expr->as<Luau::AstExprGlobal>() : node->expr->as<Luau::AstExprGlobal>();

	if (root != nullptr && root->name == "Enum")
	{
		type = parent != nullptr ? enumitem_token_type::EnumMember : enumitem_token_type::Enum;
	}
	else if (is_function(node))
	{
		type = node->op == ':' ? enumitem_token_type::Method : enumitem_token_type::Function;
	}

	add(node->indexLocation, type, 0);
	return true;
}

bool token_collector::visit(Luau::AstTypeReference* node)
{
	if (node->prefix && node->prefixLocation)
	{
		add(*node->prefixLocation, enumitem_token_type::Namespace, 0);
	}

	add(node->nameLocation, enumitem_token_type::Type, 0);
	return true;
}

void token_collector::add(const Luau::Location& location, enumitem_token_type type, int modifiers)
{
	if (location.begin.line != location.end.line || location.end.column <= location.begin.column)
	{
		return;
	}

	json begin = document.to_lsp_position(location.begin);
	json end = document.to_lsp_position(location.end);
	unsigned int start = begin["character"].get<unsigned int>();
	unsigned int finish = end["character"].get<unsigned int>();

	if (finish <= start)
	{
		return;
	}

	tokens.push_back(token{location.begin.line, start, finish - start, type, modifiers});
}

void token_collector::add_local(Luau::AstLocal* local, enumitem_token_type type, int modifiers)
{
	if (local->name == "self")
	{
		return;
	}

	add(local->location, type, modifiers);
}

bool token_collector::is_function(const Luau::AstExpr* expr) const
{
	const Luau::TypeId* type = module.astTypes.find(expr);

	if (type == nullptr)
	{
		return false;
	}

	Luau::TypeId followed = Luau::follow(*type);
	return Luau::get<Luau::FunctionType>(followed) != nullptr || Luau::get<Luau::IntersectionType>(followed) != nullptr;
}

bool token_collector::is_builtin(const Luau::AstName& name) const
{
	Luau::ScopePtr scope = module.getModuleScope();

	if (!scope)
	{
		return false;
	}

	for (const Luau::Scope* current = scope.get(); current != nullptr; current = current->parent.get())
	{
		if (current->bindings.find(Luau::Symbol(name)) != current->bindings.end())
		{
			return current != scope.get();
		}
	}

	return false;
}

int get_modifier(enumitem_token_modifier modifier)
{
	return static_cast<int>(modifier);
}

}
}

}
