#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"
#include "Luau/Lexer.h"
#include "Luau/PrettyPrinter.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"
#include "Luau/VisitType.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace lunaut
{
namespace
{
namespace priv
{

constexpr std::string_view documentation_breaker = "\n----------\n";

std::string make_code_block(const std::string& code);
std::optional<std::string> get_type_name(Luau::TypeId type);
std::string to_string_type_fun(const std::string& name, const Luau::TypeFun& definition);
std::string to_string_named_function(
	const Luau::Module& module, const std::string& name, Luau::AstExpr* expr, const std::string& signature);
bool is_reserved_word_at(std::string_view text, const Luau::Position& position);
std::optional<size_t> get_utf8_length(std::string_view text);

}
}

json workspace::get_hover(const json& params)
{
	std::string address = params["textDocument"].value("uri", "");
	const textdocument* document = resolver.get_document(address);

	if (document == nullptr)
	{
		return nullptr;
	}

	Luau::Position position = document->to_luau_position(params["position"]);

	if (priv::is_reserved_word_at(document->get_text(), position))
	{
		return nullptr;
	}

	Luau::ModuleName name = resolver.get_module_name(address);
	check_module(name, true);

	const Luau::SourceModule* source = frontend.getSourceModule(name);
	Luau::ModulePtr module = get_module(name, true);

	if (source == nullptr || !module || Luau::isWithinComment(*source, position))
	{
		return nullptr;
	}

	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, position, true);
	Luau::AstNode* node = ancestry.empty() ? nullptr : ancestry.back();
	Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, position);
	Luau::ExprOrLocal target = Luau::findExprOrLocalAtPosition(*source, position);

	if (node == nullptr || !scope)
	{
		return nullptr;
	}

	std::optional<Luau::TypeId> type;
	std::optional<namedtypeinfo> alias;
	std::optional<namedtypeinfo> value_alias;
	std::optional<Luau::ModuleName> value_alias_module;
	std::optional<namedtypeinfo> return_alias;
	std::optional<Luau::ModuleName> return_alias_module;
	std::optional<declarationinfo> origin;
	std::optional<declarationinfo> function_definition;
	std::optional<std::string> index_name;
	bool require_reference = false;
	std::optional<std::string> symbol = Luau::getDocumentationSymbolAtPosition(*source, *module, position);

	if (Luau::AstTypeReference* reference = node->as<Luau::AstTypeReference>())
	{
		std::optional<Luau::TypeFun> definition = reference->prefix
			? scope->lookupImportedType(reference->prefix->value, reference->name.value)
			: scope->lookupType(reference->name.value);

		if (!definition)
		{
			return nullptr;
		}

		std::string type_name =
			reference->prefix ? std::string(reference->prefix->value) + "." + reference->name.value : reference->name.value;
		alias = namedtypeinfo{type_name, *definition};
		type = definition->type;

		if (reference->prefix || reference->prefixLocal)
		{
			std::string imported =
				reference->prefix ? std::string(reference->prefix->value) : std::string(reference->prefixLocal->name.value);

			if (std::optional<Luau::ModuleName> required = typelookup::find_imported_module(scope, imported))
			{
				if (definition->definitionLocation)
				{
					origin = declarationinfo{*required, *definition->definitionLocation};
				}
			}
		}
		else if (definition->definitionLocation)
		{
			std::optional<Luau::ModuleName> definition_module = find_definition_module(*definition->definitionLocation);

			if (!definition_module)
			{
				definition_module = typelookup::get_definition_module(definition->type);
			}

			if (!definition_module)
			{
				definition_module = find_definition_module(definition->type);
			}

			origin = declarationinfo{definition_module.value_or(name), *definition->definitionLocation};
		}
	}
	else if (Luau::AstStatTypeAlias* statement = node->as<Luau::AstStatTypeAlias>())
	{
		std::optional<Luau::TypeFun> definition = scope->lookupType(statement->name.value);

		if (!definition)
		{
			return nullptr;
		}

		alias = namedtypeinfo{statement->name.value, *definition};
		type = definition->type;
		origin = declarationinfo{name, statement->location};
	}
	else if (Luau::AstType* annotation = node->asType())
	{
		if (const Luau::TypeId* resolved = module->astResolvedTypes.find(annotation))
		{
			type = *resolved;
		}
	}
	else if (Luau::AstLocal* local = target.getLocal())
	{
		type = scope->lookup(Luau::Symbol(local));

		if (type && typenormalizer::is_refined(*type))
		{
			if (std::optional<Luau::TypeId> declared = get_initializer_type(*module, typelookup::find_initializer(*source, local)))
			{
				type = declared;
			}
		}

		value_alias = typelookup::get_reference_alias(scope, local->annotation);
		origin = declarationinfo{name, local->location};
	}
	else if (Luau::AstExpr* expr = target.getExpr())
	{
		if (ancestry.size() >= 2)
		{
			if (Luau::AstExprTable* table = ancestry[ancestry.size() - 2]->as<Luau::AstExprTable>())
			{
				for (const Luau::AstExprTable::Item& item : table->items)
				{
					if (item.key != nullptr && item.key->location.contains(position))
					{
						if (const Luau::TypeId* value = module->astTypes.find(item.value))
						{
							type = *value;
						}

						std::string key;

						if (Luau::AstExprConstantString* constant = item.key->as<Luau::AstExprConstantString>())
						{
							key.assign(constant->value.data, constant->value.size);
						}
						else if (Luau::AstExprGlobal* global = item.key->as<Luau::AstExprGlobal>())
						{
							key = global->name.value;
						}

						if (const Luau::TypeId* expected = module->astExpectedTypes.find(table); expected != nullptr && !key.empty())
						{
							Luau::TypeId base = Luau::follow(*expected);

							if (std::optional<Luau::Property> property = typelookup::find_property(base, key, base))
							{
								if (property->readTy)
								{
									type = *property->readTy;
								}

								if (std::optional<Luau::ModuleName> definition = typelookup::get_definition_module(base))
								{
									if (property->location)
									{
										origin = declarationinfo{*definition, *property->location};
									}
									else if (property->typeLocation)
									{
										origin = declarationinfo{*definition, *property->typeLocation};
									}
								}
							}
						}

						break;
					}
				}
			}
		}

		if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
		{
			if (const Luau::TypeId* owner = module->astTypes.find(index->expr))
			{
				Luau::TypeId base = Luau::follow(*owner);

				if (std::optional<Luau::Property> property = typelookup::find_property(base, index->index.value, base))
				{
					if (property->readTy)
					{
						type = *property->readTy;
					}

					index_name = index->index.value;

					if (std::optional<Luau::ModuleName> definition = typelookup::get_definition_module(base))
					{
						if (property->location)
						{
							origin = declarationinfo{*definition, *property->location};
						}
						else if (property->typeLocation)
						{
							origin = declarationinfo{*definition, *property->typeLocation};
						}
					}
				}
			}
		}

		if (Luau::AstExprLocal* reference = expr->as<Luau::AstExprLocal>(); !origin && reference != nullptr)
		{
			origin = declarationinfo{name, reference->local->location};
		}

		if (!type)
		{
			if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>(); global != nullptr && global->name == "require")
			{
				for (Luau::AstNode* ancestor : ancestry)
				{
					Luau::AstExprCall* call = ancestor->as<Luau::AstExprCall>();

					if (call == nullptr || call->func != expr)
					{
						continue;
					}

					for (Luau::AstNode* statement_node : ancestry)
					{
						Luau::AstStatLocal* statement = statement_node->as<Luau::AstStatLocal>();

						if (statement == nullptr)
						{
							continue;
						}

						for (size_t index = 0; index < statement->values.size && index < statement->vars.size; ++index)
						{
							if (statement->values.data[index] == call)
							{
								type = scope->lookup(Luau::Symbol(statement->vars.data[index]));
							}
						}

						break;
					}

					require_reference = type.has_value();
					break;
				}
			}
		}

		if (!type)
		{
			if (const Luau::TypeId* found = module->astTypes.find(expr))
			{
				type = *found;
			}
			else if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
			{
				type = scope->lookup(global->name);
			}
			else if (Luau::AstExprLocal* reference = expr->as<Luau::AstExprLocal>())
			{
				type = scope->lookup(Luau::Symbol(reference->local));
				value_alias = typelookup::get_reference_alias(scope, reference->local->annotation);
			}
		}
	}

	if (!type)
	{
		return nullptr;
	}

	type = typenormalizer::strip_refinements(*type);

	if (!alias && !value_alias)
	{
		const Luau::AstExpr* expression = target.getExpr();

		if (expression == nullptr)
		{
			const Luau::AstLocal* local = target.getLocal();

			if (local == nullptr && node->is<Luau::AstExprLocal>())
			{
				local = node->as<Luau::AstExprLocal>()->local;
			}

			if (local != nullptr)
			{
				expression = typelookup::find_initializer(*source, local);
			}
		}

		std::string alias_name;
		std::string alias_display;

		if (expression != nullptr)
		{
			if (std::optional<Luau::TypeFun> definition = find_expression_alias(*module, scope, expression, alias_name, alias_display))
			{
				value_alias = namedtypeinfo{alias_name, *definition, alias_display};
			}
		}

		if (!value_alias)
		{
			std::string type_name;

			if (std::optional<Luau::TypeFun> definition = find_return_alias(*type, type_name))
			{
				value_alias = namedtypeinfo{type_name, *definition, type_name};
			}
		}

		if (!value_alias)
		{
			if (std::optional<Luau::TypeFun> definition = find_display_alias(*type, name, alias_name, alias_display))
			{
				value_alias = namedtypeinfo{alias_name, *definition, alias_display};
			}
		}
	}

	if (value_alias && !value_alias_module && value_alias->definition.definitionLocation)
	{
		value_alias_module = find_definition_module(*value_alias->definition.definitionLocation);

		if (!value_alias_module)
		{
			value_alias_module = find_definition_module(value_alias->definition.type);
		}
	}

	if (require_reference)
	{
		symbol.reset();
	}

	if (!symbol)
	{
		symbol = (*type)->documentationSymbol;
	}

	Luau::ToStringOptions options;
	options.exhaustive = true;
	options.useLineBreaks = true;
	options.functionTypeArguments = true;
	options.hideNamedFunctionTypeParameters = false;
	options.hideTableKind = true;
	options.hideTableAliasExpansions = true;

	std::string text = Luau::toString(*type, options);

	const Luau::AstType* display_annotation = nullptr;

	if (const Luau::AstLocal* local = target.getLocal())
	{
		display_annotation = local->annotation;
	}
	else if (node->is<Luau::AstExprLocal>())
	{
		display_annotation = node->as<Luau::AstExprLocal>()->local->annotation;
	}

	const Luau::AstExpr* display_expression = target.getExpr();

	if (display_expression == nullptr)
	{
		const Luau::AstLocal* local = target.getLocal();

		if (local == nullptr && node->is<Luau::AstExprLocal>())
		{
			local = node->as<Luau::AstExprLocal>()->local;
		}

		if (local != nullptr)
		{
			display_expression = typelookup::find_initializer(*source, local);
		}
	}

	std::vector<typelinkinfo> type_links;
	typedisplayinfo display_info = get_type_display(*type, *module, scope, display_annotation, display_expression, 0, &type_links);
	const std::string display = display_info.text;

	if (alias)
	{
		Luau::ToStringOptions body = options;
		body.hideTableAliasExpansions = false;
		text =
			priv::make_code_block("type " + priv::to_string_type_fun(alias->name, alias->definition) + " = " + Luau::toString(*type, body));
	}
	else if (const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(*type))
	{
		std::optional<Luau::AstName> local_name = target.getName();
		std::string function_name = target.getLocal() != nullptr && local_name ? std::string(local_name->value) : "";

		if (function->definition && function->definition->definitionModuleName)
		{
			function_definition = declarationinfo{*function->definition->definitionModuleName, function->definition->definitionLocation};
		}

		if (function->generics.empty() && function->genericPacks.empty())
		{
			std::string alias_name;
			std::string alias_display;
			Luau::ModuleName alias_module;

			if (std::optional<Luau::TypeFun> definition = find_function_return_alias(*function, alias_module, alias_name, &alias_display))
			{
				return_alias = namedtypeinfo{alias_name, *definition, alias_display};

				if (definition->definitionLocation)
				{
					return_alias_module = find_definition_module(*definition->definitionLocation).value_or(alias_module);
				}
				else
				{
					return_alias_module = alias_module;
				}
			}
		}

		if (!return_alias && origin && index_name)
		{
			std::string alias_name;
			std::string alias_display;

			if (std::optional<Luau::TypeFun> definition =
					find_annotation_return_alias(origin->module, origin->location, *index_name, alias_name, &alias_display))
			{
				return_alias = namedtypeinfo{alias_name, *definition, alias_display};

				if (definition->definitionLocation)
				{
					return_alias_module = find_definition_module(*definition->definitionLocation).value_or(origin->module);
				}
			}
		}

		Luau::AstExpr* function_expr = target.getExpr();
		bool hide_self = false;

		if (function_expr != nullptr)
		{
			if (Luau::AstExprIndexName* index = function_expr->as<Luau::AstExprIndexName>())
			{
				hide_self = index->op == ':';
			}
		}

		const std::string* return_display = nullptr;

		if (return_alias)
		{
			return_display = return_alias->display.empty() ? &return_alias->name : &return_alias->display;
		}

		std::vector<typelinkinfo> signature_links;
		std::string signature = render_type_display(*type, module->name, scope, 0, true, hide_self, return_display, &signature_links);
		text = priv::make_code_block(priv::to_string_named_function(*module, function_name, function_expr, signature));

		for (const typelinkinfo& link : signature_links)
		{
			type_links.push_back(link);
		}
	}
	else if (index_name)
	{
		text = priv::make_code_block(*index_name + ": " + display);
	}
	else if (target.getLocal() != nullptr || node->is<Luau::AstExprLocal>())
	{
		const Luau::AstLocal* local = target.getLocal() != nullptr ? target.getLocal() : node->as<Luau::AstExprLocal>()->local;
		text = priv::make_code_block(std::string(local->isConst ? "const " : "local ") + local->name.value + ": " + display);
	}
	else if (Luau::AstExprGlobal* global = node->as<Luau::AstExprGlobal>(); global != nullptr && global->name != "require")
	{
		text = priv::make_code_block("type " + std::string(global->name.value) + " = " + display);
	}
	else if (Luau::AstExprConstantString* value = node->as<Luau::AstExprConstantString>())
	{
		size_t bytes = value->value.size;
		std::optional<size_t> characters = priv::get_utf8_length(std::string_view(value->value.data, value->value.size));
		text = characters && *characters != bytes
			? priv::make_code_block("string (" + std::to_string(bytes) + " bytes, " + std::to_string(*characters) + " characters)")
			: priv::make_code_block("string (" + std::to_string(bytes) + " bytes)");
	}
	else
	{
		text = priv::make_code_block(display);
	}

	std::optional<std::string> notes;

	if (settings.features.documentation)
	{
		if (symbol)
		{
			notes = docs.get_markdown(*symbol);
		}

		if (!notes)
		{
			notes = get_type_documentation(*type);
		}

		if (!notes && value_alias && value_alias->definition.definitionLocation && value_alias_module)
		{
			notes = get_comment_documentation(*value_alias_module, *value_alias->definition.definitionLocation);
		}

		if (!notes && origin)
		{
			notes = get_comment_documentation(origin->module, origin->location);
		}
	}

	if (notes && !notes->empty())
	{
		text += std::string(priv::documentation_breaker) + *notes;
	}

	std::vector<std::string> definitions;
	std::vector<std::string> seen;

	auto add_definition = [&](const std::string& label, const Luau::ModuleName& definition_module, const Luau::Location& location)
	{
		std::optional<textdocument> document = get_text_for(definition_module);

		if (!document)
		{
			return;
		}

		std::string uri = document->get_uri();

		std::string key = uri + "#" + std::to_string(location.begin.line) + ":" + label;

		if (std::find(seen.begin(), seen.end(), key) != seen.end())
		{
			return;
		}

		seen.push_back(key);
		definitions.push_back("[" + label + "](" + uri + "#L" + std::to_string(location.begin.line + 1) + ")");
	};

	if (alias && alias->definition.definitionLocation)
	{
		add_definition(alias->name, origin ? origin->module : name, *alias->definition.definitionLocation);
	}

	if (value_alias && value_alias->definition.definitionLocation)
	{
		bool linked = false;
		Luau::TypeId followed = Luau::follow(*type);

		if (const Luau::TableType* table = Luau::get<Luau::TableType>(followed); table != nullptr && !table->definitionModuleName.empty())
		{
			add_definition(value_alias->name, table->definitionModuleName, table->definitionLocation);
			linked = true;
		}

		if (!linked)
		{
			std::optional<Luau::ModuleName> definition_module = value_alias_module;

			if (!definition_module)
			{
				definition_module = find_definition_module(*value_alias->definition.definitionLocation);
			}

			add_definition(value_alias->name, definition_module.value_or(name), *value_alias->definition.definitionLocation);
		}
	}

	if (return_alias && return_alias->definition.definitionLocation && return_alias_module)
	{
		add_definition(return_alias->name, *return_alias_module, *return_alias->definition.definitionLocation);
	}

	for (const typelinkinfo& link : type_links)
	{
		add_definition(link.name, link.module, link.location);
	}

	if (function_definition)
	{
		std::string label = index_name.value_or("");

		if (label.empty())
		{
			if (const Luau::AstExprGlobal* global = node->as<Luau::AstExprGlobal>())
			{
				label = global->name.value;
			}
			else if (target.getLocal() != nullptr)
			{
				label = target.getLocal()->name.value;
			}
		}

		add_definition(label.empty() ? "function" : label, function_definition->module, function_definition->location);
	}

	if (!definitions.empty())
	{
		text += "\n\n**Definitions**\n";

		for (size_t index = 0; index < definitions.size(); ++index)
		{
			text += (index == 0 ? "- " : "\n- ") + definitions[index];
		}
	}

	return {{"contents", protocol::make_markdown(text)}};
}

std::optional<std::string> workspace::get_comment_documentation(const Luau::ModuleName& name, const Luau::Location& location)
{
	const Luau::SourceModule* source = frontend.getSourceModule(name);

	if (source == nullptr)
	{
		const auto known = definition_sources.find(name);

		if (known != definition_sources.end())
		{
			source = &known->second;
		}
	}

	std::optional<textdocument> text = get_text_for(name);

	if (source == nullptr || !text)
	{
		return std::nullopt;
	}

	return documentation::get_comment_markdown(*source, text->get_text(), location.begin.line);
}

std::optional<std::string> workspace::get_type_documentation(Luau::TypeId type)
{
	Luau::TypeId followed = Luau::follow(type);

	if (const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(followed))
	{
		if (function->definition && function->definition->definitionModuleName)
		{
			return get_comment_documentation(*function->definition->definitionModuleName, function->definition->definitionLocation);
		}
	}
	else if (const Luau::TableType* table = Luau::get<Luau::TableType>(followed))
	{
		if (!table->definitionModuleName.empty())
		{
			return get_comment_documentation(table->definitionModuleName, table->definitionLocation);
		}
	}
	else if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed))
	{
		if (!extern_type->definitionModuleName.empty() && extern_type->definitionLocation)
		{
			return get_comment_documentation(extern_type->definitionModuleName, *extern_type->definitionLocation);
		}
	}

	return std::nullopt;
}

namespace
{
namespace priv
{

std::string make_code_block(const std::string& code)
{
	return "```luau\n" + code + "\n```";
}

std::optional<std::string> get_type_name(Luau::TypeId type)
{
	std::optional<std::string> name;
	Luau::TypeId followed = Luau::follow(type);

	if (const std::string* found = Luau::getName(followed))
	{
		name = *found;
	}
	else if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
	{
		if (const std::string* found_metatable = Luau::getName(metatable->metatable))
		{
			name = *found_metatable;
		}
	}
	else if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed))
	{
		name = extern_type->name;
	}

	if (name && stdx::starts_with(*name, "typeof(") && stdx::ends_with(*name, ")"))
	{
		return name->substr(7, name->size() - 8);
	}

	return name;
}

std::string to_string_type_fun(const std::string& name, const Luau::TypeFun& definition)
{
	if (definition.typeParams.empty() && definition.typePackParams.empty())
	{
		return name;
	}

	std::string output = name + "<";
	bool comma = false;

	for (const Luau::GenericTypeDefinition& parameter : definition.typeParams)
	{
		output += (comma ? ", " : "") + Luau::toString(Luau::follow(parameter.ty));

		if (parameter.defaultValue)
		{
			output += " = " + Luau::toString(Luau::follow(*parameter.defaultValue));
		}

		comma = true;
	}

	for (const Luau::GenericTypePackDefinition& pack : definition.typePackParams)
	{
		output += (comma ? ", " : "") + Luau::toString(Luau::follow(pack.tp));

		if (pack.defaultValue)
		{
			output += " = " + Luau::toString(Luau::follow(*pack.defaultValue));
		}

		comma = true;
	}

	return output + ">";
}

std::string to_string_named_function(const Luau::Module& module, const std::string& name, Luau::AstExpr* expr, const std::string& signature)
{
	if (!name.empty())
	{
		return "function " + name + signature;
	}

	if (expr == nullptr || expr->is<Luau::AstExprGroup>() || expr->is<Luau::AstExprFunction>())
	{
		return "function" + signature;
	}

	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return "function " + std::string(local->local->name.value) + signature;
	}

	if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
	{
		return "function " + std::string(global->name.value) + signature;
	}

	const Luau::TypeId* parent = nullptr;
	std::string method;
	std::string base_name;

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		parent = module.astTypes.find(index->expr);
		method = std::string(1, index->op) + index->index.value;
		base_name = stdx::trim(Luau::toString(index->expr));
	}
	else if (Luau::AstExprIndexExpr* index = expr->as<Luau::AstExprIndexExpr>())
	{
		parent = module.astTypes.find(index->expr);
		method = "[" + Luau::toString(index->index) + "]";
		base_name = stdx::trim(Luau::toString(index->expr));
	}

	if (parent == nullptr)
	{
		return "function" + method + signature;
	}

	if (std::optional<std::string> type_name = get_type_name(*parent))
	{
		base_name = *type_name;
	}

	return "function " + base_name + method + signature;
}

bool is_reserved_word_at(std::string_view text, const Luau::Position& position)
{
	Luau::Allocator allocator;
	Luau::AstNameTable names(allocator);
	Luau::Lexer lexer(text.data(), text.size(), names);
	lexer.setSkipComments(true);

	while (true)
	{
		const Luau::Lexeme& token = lexer.next();

		if (token.type == Luau::Lexeme::Eof || token.location.begin > position)
		{
			return false;
		}

		if (!token.location.contains(position))
		{
			continue;
		}

		if (token.type >= Luau::Lexeme::Reserved_BEGIN && token.type < Luau::Lexeme::Reserved_END)
		{
			return token.type != Luau::Lexeme::ReservedFunction;
		}

		if (token.type == Luau::Lexeme::Name)
		{
			std::string_view name = token.name != nullptr ? std::string_view(token.name) : std::string_view();
			return name == "const" || name == "continue";
		}

		return false;
	}
}

std::optional<size_t> get_utf8_length(std::string_view text)
{
	size_t count = 0;

	for (size_t i = 0; i < text.size();)
	{
		unsigned char lead = static_cast<unsigned char>(text[i]);
		size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;

		if (length == 0 || i + length > text.size())
		{
			return std::nullopt;
		}

		i += length;
		++count;
	}

	return count;
}

}
}

}
