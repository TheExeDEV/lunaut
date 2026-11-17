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

struct location_info
{
	Luau::ModuleName module;
	Luau::Location location;
};

struct named_type
{
	std::string name;
	Luau::TypeFun definition;
	std::string display;
};

std::string make_code_block(const std::string& code);
std::optional<std::string> get_type_name(Luau::TypeId type);
std::string to_string_type_fun(const std::string& name, const Luau::TypeFun& definition);
std::string
to_string_named_function(const Luau::Module& module, const std::string& name, Luau::AstExpr* expr, const std::string& signature);
std::optional<Luau::Property> find_property(Luau::TypeId owner, const std::string& name, Luau::TypeId& base);
bool is_primitive_like(Luau::TypeId type);
bool is_same_type(Luau::TypeId left, Luau::TypeId right);
bool is_same_type(Luau::TypeId left, Luau::TypeId right, const Luau::TypeFun& definition);
bool may_be_same_type(Luau::TypeId left, Luau::TypeId right);
void flatten_intersection(Luau::TypeId type, std::vector<Luau::TypeId>& parts);
void collect_origin_modules(Luau::TypeId type, std::set<Luau::ModuleName>& modules);
void collect_definition_locations(Luau::TypeId type, std::vector<location_info>& locations);
bool location_contains(const Luau::Location& outer, const Luau::Location& inner);
int definition_location_score(
	const Luau::ModuleName& module, const std::optional<Luau::Location>& location, const std::vector<location_info>& locations);
const Luau::AstExpr* find_initializer(const Luau::SourceModule& source, const Luau::AstLocal* local);
std::optional<named_type> get_reference_alias(const Luau::ScopePtr& scope, const Luau::AstType* annotation);
bool contains_generic_reference(const Luau::ScopePtr& scope, const Luau::AstType* annotation);
bool contains_generic_reference(const Luau::ScopePtr& scope, const Luau::AstTypePack* pack);
std::optional<Luau::ModuleName> get_definition_module(Luau::TypeId type);
std::optional<Luau::ModuleName> find_imported_module(const Luau::ScopePtr& scope, const std::string& alias);
std::optional<size_t> get_utf8_length(std::string_view text);
bool is_reserved_word_at(std::string_view text, const Luau::Position& position);

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
	std::optional<priv::named_type> alias;
	std::optional<priv::named_type> value_alias;
	std::optional<Luau::ModuleName> value_alias_module;
	std::optional<priv::named_type> return_alias;
	std::optional<Luau::ModuleName> return_alias_module;
	std::optional<priv::location_info> origin;
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
		alias = priv::named_type{type_name, *definition};
		type = definition->type;

		if (reference->prefix || reference->prefixLocal)
		{
			std::string imported =
				reference->prefix ? std::string(reference->prefix->value) : std::string(reference->prefixLocal->name.value);

			if (std::optional<Luau::ModuleName> required = priv::find_imported_module(scope, imported))
			{
				if (definition->definitionLocation)
				{
					origin = priv::location_info{*required, *definition->definitionLocation};
				}
			}
		}
		else if (definition->definitionLocation)
		{
			std::optional<Luau::ModuleName> definition_module = find_definition_module(*definition->definitionLocation);

			if (!definition_module)
			{
				definition_module = priv::get_definition_module(definition->type);
			}

			if (!definition_module)
			{
				definition_module = find_definition_module(definition->type);
			}

			origin = priv::location_info{definition_module.value_or(name), *definition->definitionLocation};
		}
	}
	else if (Luau::AstStatTypeAlias* statement = node->as<Luau::AstStatTypeAlias>())
	{
		std::optional<Luau::TypeFun> definition = scope->lookupType(statement->name.value);

		if (!definition)
		{
			return nullptr;
		}

		alias = priv::named_type{statement->name.value, *definition};
		type = definition->type;
		origin = priv::location_info{name, statement->location};
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
		value_alias = priv::get_reference_alias(scope, local->annotation);
		origin = priv::location_info{name, local->location};
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

							if (std::optional<Luau::Property> property = priv::find_property(base, key, base))
							{
								if (property->readTy)
								{
									type = *property->readTy;
								}

								if (std::optional<Luau::ModuleName> definition = priv::get_definition_module(base))
								{
									if (property->location)
									{
										origin = priv::location_info{*definition, *property->location};
									}
									else if (property->typeLocation)
									{
										origin = priv::location_info{*definition, *property->typeLocation};
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

				if (std::optional<Luau::Property> property = priv::find_property(base, index->index.value, base))
				{
					if (property->readTy)
					{
						type = *property->readTy;
					}

					index_name = index->index.value;

					if (std::optional<Luau::ModuleName> definition = priv::get_definition_module(base))
					{
						if (property->location)
						{
							origin = priv::location_info{*definition, *property->location};
						}
						else if (property->typeLocation)
						{
							origin = priv::location_info{*definition, *property->typeLocation};
						}
					}
				}
			}
		}

		if (Luau::AstExprLocal* reference = expr->as<Luau::AstExprLocal>(); !origin && reference != nullptr)
		{
			origin = priv::location_info{name, reference->local->location};
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
				value_alias = priv::get_reference_alias(scope, reference->local->annotation);
			}
		}
	}

	if (!type)
	{
		return nullptr;
	}

	type = Luau::follow(*type);

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
				expression = priv::find_initializer(*source, local);
			}
		}

		std::string alias_name;
		std::string alias_display;

		if (expression != nullptr)
		{
			if (std::optional<Luau::TypeFun> definition = find_expression_alias(*module, scope, expression, alias_name, alias_display))
			{
				value_alias = priv::named_type{alias_name, *definition, alias_display};
			}
		}

		if (!value_alias)
		{
			std::string type_name;

			if (std::optional<Luau::TypeFun> definition = find_return_alias(*type, type_name))
			{
				value_alias = priv::named_type{type_name, *definition, type_name};
			}
		}

		if (!value_alias)
		{
			if (std::optional<Luau::TypeFun> definition = find_display_alias(*type, name, alias_name, alias_display))
			{
				value_alias = priv::named_type{alias_name, *definition, alias_display};
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
			display_expression = priv::find_initializer(*source, local);
		}
	}

	typedisplayinfo display_info = get_type_display(*type, *module, scope, display_annotation, display_expression);
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

		if (function->generics.empty() && function->genericPacks.empty())
		{
			std::string alias_name;
			Luau::ModuleName alias_module;

			if (std::optional<Luau::TypeFun> definition = find_function_return_alias(*function, alias_module, alias_name))
			{
				return_alias = priv::named_type{alias_name, *definition};
				return_alias_module = alias_module;
			}
		}

		if (!return_alias && origin && index_name)
		{
			std::string alias_name;

			if (std::optional<Luau::TypeFun> definition =
					find_annotation_return_alias(origin->module, origin->location, *index_name, alias_name))
			{
				return_alias = priv::named_type{alias_name, *definition};

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

		std::string signature =
			render_type_display(*type, module->name, scope, 0, true, hide_self, return_alias ? &return_alias->name : nullptr);
		text = priv::make_code_block(priv::to_string_named_function(*module, function_name, function_expr, signature));
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

json workspace::get_type_definition(const json& params)
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

	if (source == nullptr || module == nullptr)
	{
		return nullptr;
	}

	Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, position);
	Luau::ExprOrLocal target = Luau::findExprOrLocalAtPosition(*source, position);
	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, position, true);
	Luau::AstNode* node = ancestry.empty() ? nullptr : ancestry.back();
	std::optional<priv::location_info> found;

	auto use_alias = [&](const Luau::TypeFun& definition, std::optional<Luau::ModuleName> origin)
	{
		if (!definition.definitionLocation)
		{
			return false;
		}

		Luau::ModuleName definition_module = origin.value_or(find_definition_module(*definition.definitionLocation).value_or(name));
		found = priv::location_info{definition_module, *definition.definitionLocation};
		return true;
	};

	auto use_type = [&](Luau::TypeId type)
	{
		Luau::TypeId followed = Luau::follow(type);

		if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
		{
			followed = Luau::follow(metatable->table);
		}

		if (const Luau::TableType* table = Luau::get<Luau::TableType>(followed); table != nullptr && !table->definitionModuleName.empty())
		{
			found = priv::location_info{table->definitionModuleName, table->definitionLocation};
			return true;
		}

		if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed);
			extern_type != nullptr && !extern_type->definitionModuleName.empty() && extern_type->definitionLocation)
		{
			found = priv::location_info{extern_type->definitionModuleName, *extern_type->definitionLocation};
			return true;
		}

		std::string alias_name;
		Luau::ModuleName alias_module;

		if (std::optional<Luau::TypeFun> definition = find_return_alias(type, alias_name))
		{
			if (definition->definitionLocation)
			{
				Luau::ModuleName definition_module = find_definition_module(*definition->definitionLocation).value_or(name);
				found = priv::location_info{definition_module, *definition->definitionLocation};
				return true;
			}
		}

		if (std::optional<Luau::TypeFun> definition = find_type_alias(followed, alias_module, alias_name))
		{
			if (definition->definitionLocation)
			{
				found = priv::location_info{alias_module, *definition->definitionLocation};
				return true;
			}
		}

		return false;
	};

	if (Luau::AstTypeReference* reference = node != nullptr ? node->as<Luau::AstTypeReference>() : nullptr)
	{
		if (scope != nullptr)
		{
			std::optional<Luau::TypeFun> definition = reference->prefix
				? scope->lookupImportedType(reference->prefix->value, reference->name.value)
				: scope->lookupType(reference->name.value);

			if (definition)
			{
				std::optional<Luau::ModuleName> origin;

				if (reference->prefix)
				{
					origin = priv::find_imported_module(scope, reference->prefix->value);
				}
				else if (reference->prefixLocal)
				{
					origin = priv::find_imported_module(scope, reference->prefixLocal->name.value);
				}

				use_alias(*definition, origin);
			}
		}
	}
	else if (Luau::AstStatTypeAlias* statement = node != nullptr ? node->as<Luau::AstStatTypeAlias>() : nullptr)
	{
		if (scope != nullptr)
		{
			if (std::optional<Luau::TypeFun> definition = scope->lookupType(statement->name.value))
			{
				use_alias(*definition, name);
			}
		}
	}
	else if (Luau::AstLocal* local = target.getLocal())
	{
		if (std::optional<priv::named_type> alias = priv::get_reference_alias(scope, local->annotation))
		{
			use_alias(alias->definition, name);
		}
		else if (std::optional<Luau::TypeId> type = scope->lookup(Luau::Symbol(local)))
		{
			use_type(*type);
		}
	}
	else if (Luau::AstExpr* expr = target.getExpr())
	{
		if (Luau::AstExprLocal* reference = expr->as<Luau::AstExprLocal>())
		{
			if (std::optional<priv::named_type> alias = priv::get_reference_alias(scope, reference->local->annotation))
			{
				use_alias(alias->definition, name);
			}
		}

		if (!found)
		{
			if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
			{
				if (const Luau::TypeId* owner = module->astTypes.find(index->expr))
				{
					Luau::TypeId base = Luau::follow(*owner);

					if (std::optional<Luau::Property> property = priv::find_property(base, index->index.value, base))
					{
						if (property->typeLocation)
						{
							found = priv::location_info{priv::get_definition_module(base).value_or(name), *property->typeLocation};
						}
						else if (property->readTy)
						{
							use_type(*property->readTy);
						}
					}
				}
			}
		}

		if (!found)
		{
			if (const Luau::TypeId* type = module->astTypes.find(expr))
			{
				use_type(*type);
			}
		}
	}
	else if (Luau::AstType* annotation = node != nullptr ? node->asType() : nullptr)
	{
		if (const Luau::TypeId* resolved = module->astResolvedTypes.find(annotation))
		{
			use_type(*resolved);
		}
	}

	if (!found)
	{
		return nullptr;
	}

	return get_location(found->module, found->location);
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

std::optional<std::string> workspace::get_annotation_text(const Luau::ModuleName& name, const Luau::AstType* annotation) const
{
	if (annotation == nullptr)
	{
		return std::nullopt;
	}

	std::optional<textdocument> text = get_text_for(name);

	if (!text)
	{
		return std::nullopt;
	}

	const std::string& content = text->get_text();

	auto to_offset = [&](const Luau::Position& position)
	{
		size_t offset = 0;

		for (unsigned int line = 0; line < position.line; ++line)
		{
			size_t next = content.find('\n', offset);

			if (next == std::string::npos)
			{
				return content.size();
			}

			offset = next + 1;
		}

		return std::min(content.size(), offset + static_cast<size_t>(position.column));
	};

	size_t begin = to_offset(annotation->location.begin);
	size_t end = to_offset(annotation->location.end);

	if (begin >= end || end > content.size())
	{
		return std::nullopt;
	}

	return content.substr(begin, end - begin);
}

std::optional<Luau::TypeFun> workspace::get_annotation_alias(
	const Luau::ModuleName& name,
	const Luau::Module* module,
	const Luau::ScopePtr& scope,
	const Luau::AstType* annotation,
	std::string& alias,
	std::string* display) const
{
	if (annotation == nullptr)
	{
		return std::nullopt;
	}

	if (std::optional<priv::named_type> reference = priv::get_reference_alias(scope, annotation))
	{
		alias = reference->name;

		if (display != nullptr)
		{
			*display = reference->display.empty() ? reference->name : reference->display;
		}

		return reference->definition;
	}

	if (annotation->as<Luau::AstTypeReference>() != nullptr || annotation->as<Luau::AstTypeTable>() == nullptr)
	{
		return std::nullopt;
	}

	if (priv::contains_generic_reference(scope, annotation))
	{
		return std::nullopt;
	}

	std::optional<std::string> text = get_annotation_text(name, annotation);

	if (!text)
	{
		return std::nullopt;
	}

	alias = *text;

	if (display != nullptr)
	{
		*display = *text;
	}

	if (module == nullptr)
	{
		return std::nullopt;
	}

	const Luau::TypeId* resolved = module->astResolvedTypes.find(annotation);

	if (resolved == nullptr)
	{
		return std::nullopt;
	}

	return Luau::TypeFun(*resolved);
}

std::optional<Luau::ModuleName> workspace::find_definition_module(const Luau::Location& location, const std::string& name) const
{
	auto match = [&](const Luau::ModuleName& module_name, const Luau::ModulePtr& module) -> std::optional<Luau::ModuleName>
	{
		if (!module)
		{
			return std::nullopt;
		}

		for (const auto& [alias, definition] : module->exportedTypeBindings)
		{
			if (!name.empty() && alias != name)
			{
				continue;
			}

			if (definition.definitionLocation && *definition.definitionLocation == location)
			{
				return module_name;
			}
		}

		return std::nullopt;
	};

	for (const auto& [module_name, source_node] : frontend.sourceNodes)
	{
		if (std::optional<Luau::ModuleName> found = match(module_name, frontend.moduleResolver.getModule(module_name)))
		{
			return found;
		}
	}

	for (const auto& [module_name, module] : definition_modules)
	{
		if (std::optional<Luau::ModuleName> found = match(module_name, module))
		{
			return found;
		}
	}

	return std::nullopt;
}

std::optional<Luau::TypeFun> workspace::find_type_alias(
	Luau::TypeId type, Luau::ModuleName& module, std::string& name, const std::optional<Luau::ModuleName>& preferred) const
{
	Luau::TypeId followed = Luau::follow(type);

	// A primitive already has a canonical name. Matching it against an alias such as
	// the Roblox globals' `type AdReward = any` or `type ContentId = string` would
	// replace that canonical name with an unrelated one, so structural alias recovery
	// never applies to primitives.
	if (priv::is_primitive_like(followed))
	{
		return std::nullopt;
	}

	std::set<Luau::ModuleName> origins;
	priv::collect_origin_modules(followed, origins);
	std::vector<priv::location_info> locations;
	priv::collect_definition_locations(followed, locations);

	// A union alias describes all of its options at once, so matching it by a nested
	// definition location would pick an alias for one member and silently drop the rest.
	const bool match_locations = Luau::get<Luau::UnionType>(followed) == nullptr;

	const Luau::TableType* root_table = Luau::get<Luau::TableType>(followed);

	struct candidate
	{
		Luau::ModuleName module;
		std::string name;
		Luau::TypeFun definition;
		bool local;
		bool same;
		int score;
	};

	std::vector<candidate> candidates;
	std::set<std::string> seen;

	auto score = [&](const Luau::ModuleName& candidate_module,
					 const std::string& candidate_name,
					 const Luau::TypeFun& definition,
					 bool local,
					 bool same)
	{
		int value = 0;

		if (same)
		{
			value += 240;
		}

		if (origins.count(candidate_module))
		{
			value += 100;
		}

		if (preferred && candidate_module == *preferred)
		{
			value += 80;
		}

		if (root_table != nullptr && root_table->name && candidate_name == *root_table->name)
		{
			value += 60;
		}

		if (root_table != nullptr && !root_table->definitionModuleName.empty() && candidate_module == root_table->definitionModuleName)
		{
			value += 50;
		}

		if (root_table != nullptr && definition.definitionLocation && *definition.definitionLocation == root_table->definitionLocation)
		{
			value += 200;
		}

		value += priv::definition_location_score(candidate_module, definition.definitionLocation, locations);

		if (local)
		{
			value += 10;
		}

		return value;
	};

	auto add = [&](const Luau::ModuleName& candidate_module,
				   const std::string& candidate_name,
				   const Luau::TypeFun& definition,
				   bool local,
				   bool same)
	{
		std::string key = candidate_module + "\n" + candidate_name;

		if (definition.definitionLocation)
		{
			key += "\n" + std::to_string(definition.definitionLocation->begin.line) + ":" +
				std::to_string(definition.definitionLocation->begin.column);
		}

		if (!seen.insert(key).second)
		{
			return;
		}

		candidates.push_back(
			{candidate_module, candidate_name, definition, local, same, score(candidate_module, candidate_name, definition, local, same)});
	};

	auto scan = [&](const Luau::ModuleName& candidate_module, const Luau::ModulePtr& candidate_module_ptr)
	{
		if (!candidate_module_ptr)
		{
			return;
		}

		auto matches = [&](const Luau::TypeFun& definition) -> bool
		{
			Luau::TypeId candidate_type = Luau::follow(definition.type);

			if (!priv::may_be_same_type(followed, candidate_type))
			{
				return false;
			}

			return priv::is_same_type(followed, candidate_type, definition);
		};

		for (const auto& [alias, definition] : candidate_module_ptr->exportedTypeBindings)
		{
			bool same = matches(definition);
			bool located =
				match_locations && priv::definition_location_score(candidate_module, definition.definitionLocation, locations) > 0;

			if (same || located)
			{
				add(candidate_module, alias, definition, false, same);
			}
		}

		const Luau::SourceModule* candidate_source = frontend.getSourceModule(candidate_module);

		if (candidate_source == nullptr)
		{
			auto known = definition_sources.find(candidate_module);

			if (known != definition_sources.end())
			{
				candidate_source = &known->second;
			}
		}

		if (candidate_source == nullptr || candidate_source->root == nullptr)
		{
			return;
		}

		for (Luau::AstStat* statement : candidate_source->root->body)
		{
			Luau::AstStatTypeAlias* type_alias = statement->as<Luau::AstStatTypeAlias>();

			if (type_alias == nullptr || candidate_module_ptr->exportedTypeBindings.count(type_alias->name.value) != 0)
			{
				continue;
			}

			const Luau::TypeId* resolved = candidate_module_ptr->astResolvedTypes.find(type_alias->type);

			if (resolved == nullptr)
			{
				continue;
			}

			Luau::TypeFun definition;
			definition.type = *resolved;
			definition.definitionLocation = type_alias->location;

			if (Luau::ScopePtr alias_scope = Luau::findScopeAtPosition(*candidate_module_ptr, type_alias->location.begin))
			{
				if (std::optional<Luau::TypeFun> bound = alias_scope->lookupType(type_alias->name.value))
				{
					definition.typeParams = bound->typeParams;
					definition.typePackParams = bound->typePackParams;
				}
			}

			bool same = matches(definition);
			bool located =
				match_locations && priv::definition_location_score(candidate_module, definition.definitionLocation, locations) > 0;

			if (!same && !located)
			{
				continue;
			}

			add(candidate_module, type_alias->name.value, definition, true, same);
		}
	};

	std::set<Luau::ModuleName> scanned;

	auto scan_module = [&](const Luau::ModuleName& candidate_module)
	{
		if (!scanned.insert(candidate_module).second)
		{
			return;
		}

		auto source = frontend.sourceNodes.find(candidate_module);

		if (source != frontend.sourceNodes.end())
		{
			// Dirty modules can still expose type IDs from a module graph that is about to
			// be replaced. Following those IDs is what used to crash the server.
			if (!source->second->hasDirtyModule(false))
			{
				scan(candidate_module, frontend.moduleResolver.getModule(candidate_module));
			}

			return;
		}

		auto definition = definition_modules.find(candidate_module);

		if (definition != definition_modules.end())
		{
			scan(candidate_module, definition->second);
		}
	};

	for (const Luau::ModuleName& origin : origins)
	{
		scan_module(origin);
	}

	if (preferred)
	{
		scan_module(*preferred);
	}

	for (const auto& [definition_name, definition_module] : definition_modules)
	{
		scan_module(definition_name);
	}

	if (candidates.empty())
	{
		return std::nullopt;
	}

	const candidate* best = &candidates.front();

	for (const candidate& entry : candidates)
	{
		if (entry.score > best->score)
		{
			best = &entry;
		}
	}

	module = best->module;
	name = best->name;
	return best->definition;
}

std::optional<Luau::TypeFun> workspace::find_display_alias(
	Luau::TypeId type, const std::optional<Luau::ModuleName>& preferred, std::string& name, std::string& display) const
{
	Luau::TypeId followed = Luau::follow(type);

	if (priv::is_primitive_like(followed))
	{
		return std::nullopt;
	}

	Luau::ModuleName alias_module;

	if (const Luau::UnionType* union_type = Luau::get<Luau::UnionType>(followed))
	{
		if (std::optional<Luau::TypeFun> definition = find_type_alias(followed, alias_module, name, preferred))
		{
			display = name;
			return definition;
		}

		std::vector<Luau::TypeId> options;
		bool optional = false;

		for (Luau::TypeId part : union_type->options)
		{
			if (Luau::isNil(part))
			{
				optional = true;
			}
			else
			{
				options.push_back(Luau::follow(part));
			}
		}

		if (options.empty())
		{
			return std::nullopt;
		}

		// Every union member resolving to the same alias means the whole union is that alias.
		// Luau can represent one alias as several structurally expanded variants, and naming
		// that union after the shared alias is correct even when it has several members.
		if (options.size() > 8)
		{
			return std::nullopt;
		}

		Luau::TypeFun shared;
		Luau::ModuleName shared_module;
		std::string shared_name;

		for (size_t index = 0; index < options.size(); ++index)
		{
			Luau::ModuleName option_module;
			std::string option_name;

			std::optional<Luau::TypeFun> definition = find_type_alias(options[index], option_module, option_name, preferred);

			if (!definition)
			{
				return std::nullopt;
			}

			if (index == 0)
			{
				shared = *definition;
				shared_module = std::move(option_module);
				shared_name = std::move(option_name);
			}
			else if (option_module != shared_module || option_name != shared_name)
			{
				return std::nullopt;
			}
		}

		alias_module = std::move(shared_module);
		name = std::move(shared_name);
		display = optional ? name + "?" : name;
		return shared;
	}

	if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(followed))
	{
		if (std::optional<Luau::TypeFun> definition = find_type_alias(followed, alias_module, name, preferred))
		{
			display = name;
			return definition;
		}

		Luau::TypeId best_part = nullptr;
		Luau::TypeFun best_definition;
		std::string best_name;
		size_t best_size = 0;

		for (Luau::TypeId part : intersection->parts)
		{
			std::vector<Luau::TypeId> parts;
			priv::flatten_intersection(part, parts);

			if (parts.size() <= best_size)
			{
				continue;
			}

			std::string part_name;

			if (std::optional<Luau::TypeFun> definition = find_type_alias(part, alias_module, part_name, preferred))
			{
				best_part = part;
				best_definition = *definition;
				best_name = part_name;
				best_size = parts.size();
			}
		}

		if (best_part != nullptr)
		{
			name = best_name;
			display = best_name;
			return best_definition;
		}

		return std::nullopt;
	}

	if (std::optional<Luau::TypeFun> definition = find_type_alias(followed, alias_module, name, preferred))
	{
		display = name;
		return definition;
	}

	return std::nullopt;
}

std::optional<Luau::TypeFun> workspace::find_expression_alias(
	const Luau::Module& module, const Luau::ScopePtr& scope, const Luau::AstExpr* expression, std::string& name, std::string& display) const
{
	if (expression == nullptr)
	{
		return std::nullopt;
	}

	if (const Luau::AstExprTypeAssertion* assertion = expression->as<Luau::AstExprTypeAssertion>())
	{
		if (std::optional<Luau::TypeFun> definition =
				get_annotation_alias(module.name, &module, scope, assertion->annotation, name, &display))
		{
			return definition;
		}
	}

	const Luau::AstExprCall* call = expression->as<Luau::AstExprCall>();

	if (call == nullptr)
	{
		return std::nullopt;
	}

	if (const Luau::TypeId* found = module.astTypes.find(call->func))
	{
		const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(Luau::follow(*found));

		if (function != nullptr)
		{
			Luau::ModuleName alias_module;

			if (std::optional<Luau::TypeFun> definition = find_function_return_alias(*function, alias_module, name, &display))
			{
				return definition;
			}

			if (const Luau::AstExprIndexName* index = call->func->as<Luau::AstExprIndexName>())
			{
				if (const Luau::TypeId* owner = module.astTypes.find(index->expr))
				{
					Luau::TypeId base = Luau::follow(*owner);

					if (std::optional<Luau::Property> property = priv::find_property(base, index->index.value, base))
					{
						if (property->readTy)
						{
							std::optional<Luau::ModuleName> owner_module = priv::get_definition_module(base);

							if (owner_module)
							{
								std::optional<Luau::Location> property_location = property->typeLocation;

								if (!property_location)
								{
									property_location = property->location;
								}

								if (property_location)
								{
									if (std::optional<Luau::TypeFun> definition = find_annotation_return_alias(
											*owner_module, *property_location, index->index.value, name, &display))
									{
										return definition;
									}
								}
							}

							const Luau::FunctionType* property_function = Luau::get<Luau::FunctionType>(Luau::follow(*property->readTy));

							if (property_function != nullptr && property_function->definition &&
								property_function->definition->definitionModuleName)
							{
								if (std::optional<Luau::TypeFun> definition = find_annotation_return_alias(
										*property_function->definition->definitionModuleName,
										property_function->definition->definitionLocation,
										index->index.value,
										name,
										&display))
								{
									return definition;
								}
							}
						}
					}
				}
			}
		}
	}

	for (const Luau::AstTypeOrPack& argument : call->typeArguments)
	{
		if (argument.type == nullptr)
		{
			continue;
		}

		if (std::optional<Luau::TypeFun> definition = get_annotation_alias(module.name, &module, scope, argument.type, name, &display))
		{
			return definition;
		}
	}

	return std::nullopt;
}

std::optional<Luau::TypeFun> workspace::find_return_alias(Luau::TypeId type, std::string& name) const
{
	Luau::TypeId followed = Luau::follow(type);

	if (priv::is_primitive_like(followed))
	{
		return std::nullopt;
	}

	std::optional<Luau::ModuleName> preferred = priv::get_definition_module(followed);
	std::set<Luau::ModuleName> origins;
	priv::collect_origin_modules(followed, origins);
	std::optional<priv::named_type> exact;
	std::optional<Luau::ModuleName> exact_module;

	struct candidate
	{
		Luau::ModuleName name;
		Luau::ModulePtr module;
		const Luau::AstType* annotation;
		Luau::TypeId type;
	};

	std::vector<candidate> candidates;
	std::set<Luau::ModuleName> module_names = origins;

	if (preferred)
	{
		module_names.insert(*preferred);
	}

	for (const Luau::ModuleName& module_name : module_names)
	{
		auto node = frontend.sourceNodes.find(module_name);

		if (node == frontend.sourceNodes.end() || node->second->hasDirtyModule(false))
		{
			continue;
		}

		const Luau::SourceModule* source = frontend.getSourceModule(module_name);
		Luau::ModulePtr module = frontend.moduleResolver.getModule(module_name);

		if (source == nullptr || module == nullptr || source->root == nullptr)
		{
			continue;
		}

		for (Luau::AstStat* statement : source->root->body)
		{
			Luau::AstStatReturn* statement_return = statement->as<Luau::AstStatReturn>();

			if (statement_return == nullptr || statement_return->list.size != 1)
			{
				continue;
			}

			Luau::AstExprLocal* expression = statement_return->list.data[0]->as<Luau::AstExprLocal>();

			if (expression == nullptr || expression->local->annotation == nullptr)
			{
				continue;
			}

			const Luau::TypeId* resolved = module->astResolvedTypes.find(expression->local->annotation);

			if (resolved == nullptr)
			{
				continue;
			}

			Luau::TypeId annotation_type = Luau::follow(*resolved);

			if (annotation_type == followed)
			{
				Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, expression->local->annotation->location.begin);
				std::string annotation_name;

				if (std::optional<Luau::TypeFun> definition =
						get_annotation_alias(module_name, module.get(), scope, expression->local->annotation, annotation_name))
				{
					if (preferred && module_name == *preferred)
					{
						name = annotation_name;
						return definition;
					}

					if (!exact)
					{
						exact = priv::named_type{annotation_name, *definition, annotation_name};
						exact_module = module_name;
					}
				}
			}
			else
			{
				candidates.push_back({module_name, module, expression->local->annotation, annotation_type});
			}
		}
	}

	if (exact && (!preferred || exact_module == preferred))
	{
		name = exact->name;
		return exact->definition;
	}

	auto try_candidate = [&](const candidate& entry) -> std::optional<Luau::TypeFun>
	{
		if (!priv::is_same_type(followed, entry.type))
		{
			return std::nullopt;
		}

		if (!origins.empty())
		{
			std::set<Luau::ModuleName> candidate_origins;
			priv::collect_origin_modules(entry.type, candidate_origins);

			bool related = candidate_origins.empty();

			for (const Luau::ModuleName& candidate_origin : candidate_origins)
			{
				if (origins.count(candidate_origin) != 0)
				{
					related = true;
					break;
				}
			}

			if (!related)
			{
				return std::nullopt;
			}
		}

		Luau::ScopePtr scope = Luau::findScopeAtPosition(*entry.module, entry.annotation->location.begin);

		return get_annotation_alias(entry.name, entry.module.get(), scope, entry.annotation, name);
	};

	if (preferred)
	{
		for (const candidate& entry : candidates)
		{
			if (entry.name != *preferred)
			{
				continue;
			}

			if (std::optional<Luau::TypeFun> definition = try_candidate(entry))
			{
				return definition;
			}
		}
	}

	if (exact)
	{
		name = exact->name;
		return exact->definition;
	}

	for (const candidate& entry : candidates)
	{
		if (std::optional<Luau::TypeFun> definition = try_candidate(entry))
		{
			return definition;
		}
	}

	return std::nullopt;
}

std::optional<Luau::TypeFun> workspace::find_function_return_alias(
	const Luau::FunctionType& function, Luau::ModuleName& module_name, std::string& name, std::string* display) const
{
	if (!function.definition || !function.definition->definitionModuleName)
	{
		return std::nullopt;
	}

	module_name = *function.definition->definitionModuleName;

	const Luau::SourceModule* source = frontend.getSourceModule(module_name);
	Luau::ModulePtr module = frontend.moduleResolver.getModule(module_name);

	if (source == nullptr)
	{
		auto known = definition_sources.find(module_name);

		if (known != definition_sources.end())
		{
			source = &known->second;
		}
	}

	if (module == nullptr)
	{
		auto known = definition_modules.find(module_name);

		if (known != definition_modules.end())
		{
			module = known->second;
		}
	}

	if (source == nullptr || module == nullptr || source->root == nullptr)
	{
		return std::nullopt;
	}

	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, function.definition->definitionLocation.begin, true);

	for (Luau::AstNode* node : ancestry)
	{
		Luau::AstExprFunction* expression = node->as<Luau::AstExprFunction>();

		if (expression == nullptr || expression->returnAnnotation == nullptr)
		{
			continue;
		}

		Luau::AstTypePackExplicit* pack = expression->returnAnnotation->as<Luau::AstTypePackExplicit>();

		if (pack == nullptr || pack->typeList.types.size != 1 || pack->typeList.tailType != nullptr)
		{
			break;
		}

		Luau::AstType* annotation = pack->typeList.types.data[0];
		Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, annotation->location.begin);

		if (std::optional<Luau::TypeFun> definition = get_annotation_alias(module_name, module.get(), scope, annotation, name, display))
		{
			return definition;
		}

		break;
	}

	return std::nullopt;
}

std::optional<Luau::TypeFun> workspace::find_annotation_return_alias(
	const Luau::ModuleName& module_name,
	const Luau::Location& location,
	const std::string& property,
	std::string& name,
	std::string* display) const
{
	const Luau::SourceModule* source = frontend.getSourceModule(module_name);
	Luau::ModulePtr module = frontend.moduleResolver.getModule(module_name);

	if (source == nullptr)
	{
		auto known = definition_sources.find(module_name);

		if (known != definition_sources.end())
		{
			source = &known->second;
		}
	}

	if (module == nullptr)
	{
		auto known = definition_modules.find(module_name);

		if (known != definition_modules.end())
		{
			module = known->second;
		}
	}

	if (source == nullptr || module == nullptr || source->root == nullptr)
	{
		return std::nullopt;
	}

	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(*source, location.begin, true);
	Luau::AstTypeFunction* function = nullptr;

	for (Luau::AstNode* node : ancestry)
	{
		if (Luau::AstTypeFunction* candidate = node->as<Luau::AstTypeFunction>())
		{
			function = candidate;
			break;
		}

		Luau::AstTypeTable* table = node->as<Luau::AstTypeTable>();

		if (table == nullptr)
		{
			continue;
		}

		for (const Luau::AstTableProp& field : table->props)
		{
			if (field.name.value != property)
			{
				continue;
			}

			function = field.type->as<Luau::AstTypeFunction>();
			break;
		}
	}

	if (function == nullptr || function->returnTypes == nullptr)
	{
		return std::nullopt;
	}

	Luau::AstTypePackExplicit* pack = function->returnTypes->as<Luau::AstTypePackExplicit>();

	if (pack == nullptr || pack->typeList.types.size != 1 || pack->typeList.tailType != nullptr)
	{
		return std::nullopt;
	}

	Luau::AstType* annotation = pack->typeList.types.data[0];
	Luau::ScopePtr scope = Luau::findScopeAtPosition(*module, annotation->location.begin);

	return get_annotation_alias(module_name, module.get(), scope, annotation, name, display);
}

std::optional<Luau::ModuleName> workspace::find_definition_module(Luau::TypeId type) const
{
	Luau::ModuleName module;
	std::string name;

	if (!find_type_alias(type, module, name))
	{
		return std::nullopt;
	}

	return module;
}

std::string workspace::format_alias_display(const Luau::ModuleName& module, const std::string& name) const
{
	if (definition_show_as_module.count(module) == 0)
	{
		return name;
	}

	std::string qualifier = module;
	size_t at = qualifier.find('@');

	if (at != std::string::npos)
	{
		qualifier.erase(at, 1);
	}

	if (!name.empty() && name.back() == '?')
	{
		std::string base = name.substr(0, name.size() - 1);

		if (base.find('.') != std::string::npos)
		{
			return name;
		}

		return qualifier + "." + base + "?";
	}

	if (name.find('.') != std::string::npos)
	{
		return name;
	}

	return qualifier + "." + name;
}

std::string workspace::render_type_display(
	Luau::TypeId type,
	const Luau::ModuleName& module_name,
	const Luau::ScopePtr& scope,
	size_t max_length,
	bool named_function,
	bool hide_self,
	const std::string* return_alias) const
{
	Luau::TypeId followed = Luau::follow(type);

	if (!settings.features.alias_types && !settings.features.refine_types)
	{
		Luau::ToStringOptions plain;
		plain.exhaustive = false;
		plain.hideTableKind = true;

		if (max_length > 0)
		{
			plain.maxTypeLength = max_length;
			plain.maxTableLength = max_length;
		}

		if (named_function)
		{
			if (const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(followed))
			{
				plain.useLineBreaks = false;
				plain.functionTypeArguments = true;
				plain.hideFunctionSelfArgument = hide_self && function->hasSelf;

				std::string text = Luau::toStringNamedFunction("", *function, plain);
				size_t found = 0;

				while ((found = text.find("_: ", found)) != std::string::npos)
				{
					text.erase(found, 3);
				}

				if (return_alias != nullptr)
				{
					size_t arrow = text.rfind("): ");

					if (arrow != std::string::npos)
					{
						text.erase(arrow + 3);
						text += *return_alias;
					}
				}

				return text;
			}
		}

		return Luau::toString(followed, plain);
	}

	Luau::ToStringOptions options;
	options.exhaustive = false;
	options.useLineBreaks = false;
	options.functionTypeArguments = true;
	options.hideTableKind = true;
	options.hideTableAliasExpansions = true;
	if (max_length > 0)
	{
		options.maxTypeLength = max_length;
		options.maxTableLength = max_length;
	}

	std::set<Luau::TypeId> active;
	std::map<Luau::TypeId, std::optional<std::string>> alias_cache;
	size_t alias_budget = 160;
	size_t render_budget = 768;
	int depth = 0;

	auto fallback = [&](Luau::TypeId value)
	{
		return Luau::toString(value, options);
	};
	auto fallback_pack = [&](Luau::TypePackId value)
	{
		return Luau::toString(value, options);
	};

	// `Luau::toString` hides a variadic tail marked hidden and skips empty tails.
	// Function display must do the same or inferred packs leak `...any` into signatures.
	auto visible_tail = [](const std::optional<Luau::TypePackId>& pack) -> std::optional<Luau::TypePackId>
	{
		if (!pack)
		{
			return std::nullopt;
		}

		Luau::TypePackId followed_tail = Luau::follow(*pack);

		if (const Luau::VariadicTypePack* variadic = Luau::get<Luau::VariadicTypePack>(followed_tail))
		{
			return variadic->hidden ? std::nullopt : pack;
		}

		return Luau::isEmpty(followed_tail) ? std::nullopt : pack;
	};

	std::function<std::optional<std::string>(Luau::TypeId)> resolve_alias = [&](Luau::TypeId value) -> std::optional<std::string>
	{
		value = Luau::follow(value);

		if (auto found = alias_cache.find(value); found != alias_cache.end())
		{
			return found->second;
		}

		std::optional<std::string> result;

		if (settings.features.alias_types && alias_budget > 0)
		{
			--alias_budget;
			Luau::ModuleName module;
			std::string name;

			if (find_type_alias(value, module, name, module_name))
			{
				result = format_alias_display(module, name);
			}
			else if (const Luau::UnionType* union_type = Luau::get<Luau::UnionType>(value))
			{
				Luau::TypeId only = nullptr;
				bool optional = false;

				for (Luau::TypeId option : union_type->options)
				{
					if (Luau::isNil(Luau::follow(option)))
					{
						optional = true;
					}
					else if (only == nullptr)
					{
						only = Luau::follow(option);
					}
					else
					{
						only = nullptr;
						break;
					}
				}

				if (only != nullptr)
				{
					std::string only_name;

					if (find_type_alias(only, module, only_name, module_name))
					{
						result = format_alias_display(module, only_name) + (optional ? "?" : "");
					}
				}
			}
		}

		alias_cache[value] = result;
		return result;
	};

	std::function<std::string(Luau::TypeId)> render = [&](Luau::TypeId value) -> std::string
	{
		value = Luau::follow(value);

		// A named function keeps its signature even when the whole function type has
		// an alias; only the nested types are normalized.
		if (!(named_function && depth == 0))
		{
			if (std::optional<std::string> alias = resolve_alias(value))
			{
				return *alias;
			}
		}

		if (render_budget == 0)
		{
			return fallback(value);
		}

		--render_budget;

		if (depth > 48 || !active.insert(value).second)
		{
			return fallback(value);
		}

		++depth;
		std::string result;

		if (const Luau::UnionType* union_type = Luau::get<Luau::UnionType>(value))
		{
			bool optional = false;
			std::vector<Luau::TypeId> options;

			for (Luau::TypeId option : union_type->options)
			{
				option = Luau::follow(option);

				if (Luau::isNil(option))
				{
					optional = true;
				}
				else
				{
					options.push_back(option);
				}
			}

			if (options.empty())
			{
				result = "nil";
			}
			else if (options.size() == 1)
			{
				result = render(options.front()) + (optional ? "?" : "");
			}
			else
			{
				struct part
				{
					std::string text;
					bool wrap = false;
				};

				std::vector<part> parts;

				for (Luau::TypeId option : options)
				{
					std::string text = render(option);

					if (text.empty())
					{
						continue;
					}

					bool wrap = !resolve_alias(option) &&
						(Luau::get<Luau::IntersectionType>(option) != nullptr || Luau::get<Luau::FunctionType>(option) != nullptr);
					parts.push_back({text, wrap});
				}

				std::sort(parts.begin(), parts.end(), [](const part& left, const part& right) { return left.text < right.text; });
				parts.erase(
					std::unique(parts.begin(), parts.end(), [](const part& left, const part& right) { return left.text == right.text; }),
					parts.end());

				if (settings.features.refine_types)
				{
					std::vector<part> filtered;

					for (const part& entry : parts)
					{
						if (entry.text == "{}" && parts.size() > 1)
						{
							continue;
						}

						filtered.push_back(entry);
					}

					if (!filtered.empty())
					{
						parts = std::move(filtered);
					}
				}

				if (parts.empty())
				{
					result = optional ? "nil" : "unknown";
				}
				else if (parts.size() == 1)
				{
					result = parts.front().text + (optional ? "?" : "");
				}
				else
				{
					result = optional ? "(" : "";

					for (size_t index = 0; index < parts.size(); ++index)
					{
						if (index != 0)
						{
							result += " | ";
						}

						result += parts[index].wrap ? "(" + parts[index].text + ")" : parts[index].text;
					}

					if (optional)
					{
						result += ")?";
					}
				}
			}
		}
		else if (Luau::get<Luau::IntersectionType>(value) != nullptr)
		{
			std::vector<Luau::TypeId> parts;
			priv::flatten_intersection(value, parts);

			std::vector<std::string> rendered;

			for (Luau::TypeId part : parts)
			{
				std::string text = render(part);

				if (text.empty())
				{
					continue;
				}

				if (!resolve_alias(part) && (Luau::get<Luau::UnionType>(part) != nullptr || Luau::get<Luau::FunctionType>(part) != nullptr))
				{
					text = "(" + text + ")";
				}

				rendered.push_back(std::move(text));
			}

			std::sort(rendered.begin(), rendered.end());
			rendered.erase(std::unique(rendered.begin(), rendered.end()), rendered.end());

			for (size_t index = 0; index < rendered.size(); ++index)
			{
				if (index != 0)
				{
					result += " & ";
				}

				result += rendered[index];
			}

			if (result.empty())
			{
				result = fallback(value);
			}
		}
		else if (const Luau::TableType* table = Luau::get<Luau::TableType>(value))
		{
			if (table->boundTo)
			{
				result = render(*table->boundTo);
			}
			else if (table->name)
			{
				result =
					format_alias_display(table->definitionModuleName.empty() ? module_name : table->definitionModuleName, *table->name);

				if (!table->instantiatedTypeParams.empty() || !table->instantiatedTypePackParams.empty())
				{
					result += "<";

					bool first = true;

					for (Luau::TypeId argument : table->instantiatedTypeParams)
					{
						if (!first)
						{
							result += ", ";
						}

						first = false;
						result += render(argument);
					}

					for (Luau::TypePackId argument : table->instantiatedTypePackParams)
					{
						if (!first)
						{
							result += ", ";
						}

						first = false;
						result += fallback_pack(argument);
					}

					result += ">";
				}
			}
			else if (table->indexer.has_value() && table->props.empty() && Luau::isNumber(table->indexer->indexType))
			{
				result = "{";

				if (table->indexer->isReadOnly)
				{
					result += "read ";
				}

				result += render(table->indexer->indexResultType);
				result += "}";
			}
			else
			{
				std::vector<std::string> fields;

				if (table->indexer.has_value())
				{
					std::string field;

					if (table->indexer->isReadOnly)
					{
						field += "read ";
					}

					field += "[" + render(table->indexer->indexType) + "]: " + render(table->indexer->indexResultType);
					fields.push_back(std::move(field));
				}

				for (const auto& [name, property] : table->props)
				{
					if (property.isShared())
					{
						const Luau::TypeId* type = property.readTy ? &*property.readTy : (property.writeTy ? &*property.writeTy : nullptr);

						if (type != nullptr)
						{
							fields.push_back(name + ": " + render(*type));
						}

						continue;
					}

					if (property.readTy)
					{
						fields.push_back("read " + name + ": " + render(*property.readTy));
					}

					if (property.writeTy)
					{
						fields.push_back("write " + name + ": " + render(*property.writeTy));
					}
				}

				result = "{";

				if (!fields.empty())
				{
					result += " ";

					for (size_t index = 0; index < fields.size(); ++index)
					{
						if (index != 0)
						{
							result += ", ";
						}

						result += fields[index];
					}

					result += " ";
				}

				result += "}";
			}
		}
		else if (const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(value))
		{
			if (!function->generics.empty() || !function->genericPacks.empty())
			{
				result = "<";
				bool first = true;

				for (Luau::TypeId generic_id : function->generics)
				{
					if (!first)
					{
						result += ", ";
					}

					first = false;
					const Luau::GenericType* generic = Luau::get<Luau::GenericType>(Luau::follow(generic_id));
					result += generic != nullptr ? generic->name : fallback(generic_id);
				}

				for (Luau::TypePackId generic_id : function->genericPacks)
				{
					if (!first)
					{
						result += ", ";
					}

					first = false;
					const Luau::GenericTypePack* generic = Luau::get<Luau::GenericTypePack>(Luau::follow(generic_id));
					result += "..." + (generic != nullptr ? generic->name : std::string());
				}

				result += ">";
			}

			auto [arguments, tail] = Luau::flatten(function->argTypes);
			std::optional<Luau::TypePackId> arguments_tail = visible_tail(tail);
			result += "(";

			bool first_argument = true;

			for (size_t index = 0; index < arguments.size(); ++index)
			{
				if (hide_self && depth == 1 && index == 0 && function->hasSelf)
				{
					continue;
				}

				if (!first_argument)
				{
					result += ", ";
				}

				first_argument = false;

				if (index < function->argNames.size() && function->argNames[index] && !function->argNames[index]->name.empty())
				{
					result += function->argNames[index]->name + ": ";
				}

				result += render(arguments[index]);
			}

			if (arguments_tail)
			{
				if (!first_argument)
				{
					result += ", ";
				}

				result += fallback_pack(*arguments_tail);
			}

			result += named_function && depth == 1 ? "): " : ") -> ";

			if (named_function && depth == 1 && return_alias != nullptr)
			{
				result += *return_alias;
			}
			else
			{
				auto [returns, return_tail] = Luau::flatten(function->retTypes);
				std::optional<Luau::TypePackId> returns_tail = visible_tail(return_tail);

				if (returns.empty() && !returns_tail)
				{
					result += "()";
				}
				else if (returns.size() == 1 && !returns_tail)
				{
					result += render(returns.front());
				}
				else
				{
					result += "(";

					for (size_t index = 0; index < returns.size(); ++index)
					{
						if (index != 0)
						{
							result += ", ";
						}

						result += render(returns[index]);
					}

					if (returns_tail)
					{
						if (!returns.empty())
						{
							result += ", ";
						}

						result += fallback_pack(*returns_tail);
					}

					result += ")";
				}
			}
		}
		else if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(value))
		{
			result = "setmetatable<" + render(metatable->table) + ", " + render(metatable->metatable) + ">";
		}
		else if (const Luau::GenericType* generic = Luau::get<Luau::GenericType>(value))
		{
			result = generic->name;
		}
		else
		{
			result = fallback(value);
		}

		active.erase(value);
		--depth;
		return result;
	};

	std::string result = render(followed);

	if (result.empty())
	{
		result = fallback(followed);
	}

	return result;
}

typedisplayinfo workspace::get_type_display(
	Luau::TypeId type,
	const Luau::Module& module,
	const Luau::ScopePtr& scope,
	const Luau::AstType* annotation,
	const Luau::AstExpr* initializer,
	size_t max_length) const
{
	typedisplayinfo info;
	Luau::TypeId followed = Luau::follow(type);

	if (settings.features.refine_types && initializer != nullptr)
	{
		const Luau::AstExprBinary* binary = initializer->as<Luau::AstExprBinary>();

		if (binary != nullptr && binary->op == Luau::AstExprBinary::Or)
		{
			const Luau::AstExpr* right = binary->right;
			const Luau::TypeId* right_type = module.astTypes.find(right);
			const Luau::TableType* right_table = right_type != nullptr ? Luau::get<Luau::TableType>(Luau::follow(*right_type)) : nullptr;
			bool empty_right = right->is<Luau::AstExprTable>() && right->as<Luau::AstExprTable>()->items.size == 0;
			empty_right = empty_right || (right_table != nullptr && right_table->props.empty() && !right_table->indexer.has_value());

			const Luau::TypeId* left_type = module.astTypes.find(binary->left);

			if (empty_right && left_type != nullptr)
			{
				Luau::TypeId left = Luau::follow(*left_type);

				bool table_like = Luau::get<Luau::TableType>(left) != nullptr || Luau::get<Luau::IntersectionType>(left) != nullptr ||
					Luau::get<Luau::MetatableType>(left) != nullptr;

				if (table_like)
				{
					info.text = render_type_display(left, module.name, scope, max_length);
				}
				else if (const Luau::UnionType* left_union = Luau::get<Luau::UnionType>(left))
				{
					std::vector<Luau::TypeId> values;
					bool optional = false;

					for (Luau::TypeId option : left_union->options)
					{
						if (Luau::isNil(Luau::follow(option)))
						{
							optional = true;
						}
						else
						{
							values.push_back(Luau::follow(option));
						}
					}

					if (optional)
					{
						if (values.size() == 1)
						{
							info.text = render_type_display(values.front(), module.name, scope, max_length);
						}
						else if (!values.empty())
						{
							std::string joined;

							for (size_t index = 0; index < values.size(); ++index)
							{
								if (index != 0)
								{
									joined += " | ";
								}

								joined += render_type_display(values[index], module.name, scope, max_length);
							}

							info.text = joined;
						}
					}
				}

				if (!info.text.empty())
				{
					if (max_length > 0 && info.text.size() > max_length)
					{
						info.truncated = true;
					}

					return info;
				}
			}
		}
	}

	std::string name;
	std::string display;
	Luau::TypeFun definition;
	bool found = false;

	if (settings.features.alias_types)
	{
		if (const std::optional<priv::named_type> reference = priv::get_reference_alias(scope, annotation))
		{
			name = reference->name;
			display = reference->display.empty() ? reference->name : reference->display;
			definition = reference->definition;
			found = true;
		}

		if (!found && initializer != nullptr)
		{
			if (std::optional<Luau::TypeFun> alias = find_expression_alias(module, scope, initializer, name, display))
			{
				definition = *alias;
				found = true;
			}
		}

		if (!found)
		{
			if (std::optional<Luau::TypeFun> alias = find_return_alias(followed, name))
			{
				definition = *alias;
				display = name;
				found = true;
			}
		}

		if (!found)
		{
			std::string alias_name;
			std::string alias_display;

			if (std::optional<Luau::TypeFun> alias = find_display_alias(followed, module.name, alias_name, alias_display))
			{
				definition = *alias;
				name = alias_name;
				display = alias_display.empty() ? alias_name : alias_display;
				found = true;
			}
		}
	}

	if (found)
	{
		info.alias = true;
		info.name = name;
		info.text = display;

		if (definition.definitionLocation)
		{
			size_t dot = name.rfind('.');
			std::string alias_name = dot == std::string::npos ? name : name.substr(dot + 1);
			info.module = find_definition_module(*definition.definitionLocation, alias_name);

			if (!info.module)
			{
				info.module = find_definition_module(*definition.definitionLocation);
			}
		}

		if (!info.module && definition.type != nullptr)
		{
			info.module = find_definition_module(definition.type);
		}

		if (info.module)
		{
			info.show_as_module = definition_show_as_module.count(*info.module) != 0;
			info.text = format_alias_display(*info.module, info.text);

			size_t dot = name.rfind('.');
			std::string base = dot == std::string::npos ? name : name.substr(dot + 1);
			Luau::ModulePtr alias_module = frontend.moduleResolver.getModule(*info.module);

			if (!alias_module)
			{
				const auto known_module = definition_modules.find(*info.module);

				if (known_module != definition_modules.end())
				{
					alias_module = known_module->second;
				}
			}

			// Aliases that are not exported cannot be referenced from another module,
			// so inlay hints must not offer to insert `Module.Name` for them.
			info.exported = alias_module == nullptr || alias_module->exportedTypeBindings.count(base) != 0;

			std::optional<std::string> path;
			const auto known = definition_module_paths.find(*info.module);

			if (known != definition_module_paths.end())
			{
				path = known->second;
			}
			else
			{
				path = resolver.get_real_path(*info.module);
			}

			info.definition_file = path && is_definition_file(*path);
		}

		return info;
	}

	info.text = render_type_display(followed, module.name, scope, max_length);

	if (max_length > 0 && info.text.size() > max_length)
	{
		info.truncated = true;
	}

	return info;
}

std::optional<std::string> workspace::get_type_alias_name(
	Luau::TypeId type,
	const Luau::Module& module,
	const Luau::ScopePtr& scope,
	const Luau::AstType* annotation,
	const Luau::AstExpr* initializer) const
{
	typedisplayinfo info = get_type_display(type, module, scope, annotation, initializer);

	if (info.alias)
	{
		return info.text;
	}

	return std::nullopt;
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

void flatten_intersection(Luau::TypeId type, std::vector<Luau::TypeId>& parts)
{
	type = Luau::follow(type);

	if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(type))
	{
		for (Luau::TypeId part : intersection->parts)
		{
			flatten_intersection(part, parts);
		}

		return;
	}

	parts.push_back(type);
}

struct type_equality
{
	std::set<std::pair<Luau::TypeId, Luau::TypeId>> active_types;
	std::set<std::pair<Luau::TypePackId, Luau::TypePackId>> active_packs;
	std::map<std::pair<Luau::TypeId, Luau::TypeId>, bool> type_results;
	std::map<std::pair<Luau::TypePackId, Luau::TypePackId>, bool> pack_results;
	std::map<Luau::TypeId, Luau::TypeId> type_substitutions;
	std::map<Luau::TypePackId, Luau::TypePackId> pack_substitutions;
	std::map<std::string, Luau::TypeId> named_type_substitutions;
	std::map<std::string, Luau::TypePackId> named_pack_substitutions;
	bool any_unknown_equivalent = false;

	bool equals(Luau::TypeId left, Luau::TypeId right);
	bool equals_pack(Luau::TypePackId left, Luau::TypePackId right);
	bool equals_table(const Luau::TableType& left, const Luau::TableType& right);
	bool equals_function(const Luau::FunctionType& left, const Luau::FunctionType& right);
	bool equals_property(const Luau::Property& left, const Luau::Property& right);
	bool equals_unordered(const std::vector<Luau::TypeId>& left, const std::vector<Luau::TypeId>& right);
	bool merge_table(Luau::TypeId type, Luau::TableType& out, std::set<Luau::TypeId>& active);
	bool compare(Luau::TypeId left, Luau::TypeId right);
};

bool type_equality::equals(Luau::TypeId left, Luau::TypeId right)
{
	left = Luau::follow(left);
	right = Luau::follow(right);

	// Type aliases with defaulted parameters are compared through their default
	// instantiation. `observer_var` is stored as `observer_var<T>` but appears in
	// checked code as `observer_var<unknown>`, so substituting the defaults lets the
	// display recover the alias instead of expanding the union.
	if (!type_substitutions.empty())
	{
		if (auto found = type_substitutions.find(left); found != type_substitutions.end())
		{
			left = Luau::follow(found->second);
		}

		if (auto found = type_substitutions.find(right); found != type_substitutions.end())
		{
			right = Luau::follow(found->second);
		}
	}

	if (!named_type_substitutions.empty())
	{
		auto substitute = [&](Luau::TypeId value) -> Luau::TypeId
		{
			if (const Luau::GenericType* generic = Luau::get<Luau::GenericType>(value))
			{
				if (auto found = named_type_substitutions.find(generic->name); found != named_type_substitutions.end())
				{
					return Luau::follow(found->second);
				}
			}

			return value;
		};

		left = substitute(left);
		right = substitute(right);
	}

	if (left == right)
	{
		return true;
	}

	const std::pair<Luau::TypeId, Luau::TypeId> key{left, right};

	auto cached = type_results.find(key);

	if (cached != type_results.end())
	{
		return cached->second;
	}

	if (!active_types.insert(key).second)
	{
		return true;
	}

	bool result = compare(left, right);
	active_types.erase(key);
	type_results.emplace(key, result);
	return result;
}

bool type_equality::equals_pack(Luau::TypePackId left, Luau::TypePackId right)
{
	left = Luau::follow(left);
	right = Luau::follow(right);

	if (!pack_substitutions.empty())
	{
		if (auto found = pack_substitutions.find(left); found != pack_substitutions.end())
		{
			left = Luau::follow(found->second);
		}

		if (auto found = pack_substitutions.find(right); found != pack_substitutions.end())
		{
			right = Luau::follow(found->second);
		}
	}

	if (!named_pack_substitutions.empty())
	{
		auto substitute = [&](Luau::TypePackId value) -> Luau::TypePackId
		{
			if (const Luau::GenericTypePack* generic = Luau::get<Luau::GenericTypePack>(value))
			{
				if (auto found = named_pack_substitutions.find(generic->name); found != named_pack_substitutions.end())
				{
					return Luau::follow(found->second);
				}
			}

			return value;
		};

		left = substitute(left);
		right = substitute(right);
	}

	if (left == right)
	{
		return true;
	}

	const std::pair<Luau::TypePackId, Luau::TypePackId> key{left, right};

	auto cached = pack_results.find(key);

	if (cached != pack_results.end())
	{
		return cached->second;
	}

	if (!active_packs.insert(key).second)
	{
		return true;
	}

	bool result = false;

	if (const Luau::TypePack* left_pack = Luau::get<Luau::TypePack>(left))
	{
		const Luau::TypePack* right_pack = Luau::get<Luau::TypePack>(right);

		if (right_pack != nullptr && left_pack->head.size() == right_pack->head.size() && bool(left_pack->tail) == bool(right_pack->tail))
		{
			result = true;

			for (size_t index = 0; index < left_pack->head.size() && result; ++index)
			{
				result = equals(left_pack->head[index], right_pack->head[index]);
			}

			if (result && left_pack->tail)
			{
				result = equals_pack(*left_pack->tail, *right_pack->tail);
			}
		}
	}
	else if (const Luau::VariadicTypePack* left_variadic = Luau::get<Luau::VariadicTypePack>(left))
	{
		const Luau::VariadicTypePack* right_variadic = Luau::get<Luau::VariadicTypePack>(right);
		result = right_variadic != nullptr && equals(left_variadic->ty, right_variadic->ty);
	}
	else if (const Luau::GenericTypePack* left_generic = Luau::get<Luau::GenericTypePack>(left))
	{
		const Luau::GenericTypePack* right_generic = Luau::get<Luau::GenericTypePack>(right);
		result = right_generic != nullptr && !left_generic->name.empty() && left_generic->name == right_generic->name;
	}
	else if (const Luau::FreeTypePack* left_free = Luau::get<Luau::FreeTypePack>(left))
	{
		const Luau::FreeTypePack* right_free = Luau::get<Luau::FreeTypePack>(right);
		result = right_free != nullptr && left_free->index == right_free->index;
	}

	active_packs.erase(key);
	pack_results.emplace(key, result);
	return result;
}

bool type_equality::equals_property(const Luau::Property& left, const Luau::Property& right)
{
	if (bool(left.readTy) != bool(right.readTy) || bool(left.writeTy) != bool(right.writeTy))
	{
		return false;
	}

	if (left.readTy && !equals(*left.readTy, *right.readTy))
	{
		return false;
	}

	if (left.writeTy && !equals(*left.writeTy, *right.writeTy))
	{
		return false;
	}

	return true;
}

bool type_equality::merge_table(Luau::TypeId type, Luau::TableType& out, std::set<Luau::TypeId>& active)
{
	type = Luau::follow(type);

	if (const Luau::TableType* table = Luau::get<Luau::TableType>(type))
	{
		for (const auto& [name, property] : table->props)
		{
			auto [entry, inserted] = out.props.emplace(name, property);

			if (!inserted)
			{
				Luau::Property& target = entry->second;

				if (bool(target.readTy) != bool(property.readTy) || bool(target.writeTy) != bool(property.writeTy))
				{
					return false;
				}

				if (target.readTy && property.readTy && !equals(*target.readTy, *property.readTy))
				{
					return false;
				}

				if (target.writeTy && property.writeTy && !equals(*target.writeTy, *property.writeTy))
				{
					return false;
				}
			}
		}

		if (table->indexer)
		{
			if (!out.indexer)
			{
				out.indexer = table->indexer;
			}
			else if (
				out.indexer->isReadOnly != table->indexer->isReadOnly || !equals(out.indexer->indexType, table->indexer->indexType) ||
				!equals(out.indexer->indexResultType, table->indexer->indexResultType))
			{
				return false;
			}
		}

		return true;
	}

	if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(type))
	{
		if (!active.insert(type).second)
		{
			return true;
		}

		for (Luau::TypeId part : intersection->parts)
		{
			if (!merge_table(part, out, active))
			{
				active.erase(type);
				return false;
			}
		}

		active.erase(type);
		return true;
	}

	if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(type))
	{
		return merge_table(metatable->table, out, active);
	}

	return false;
}

bool type_equality::equals_table(const Luau::TableType& left, const Luau::TableType& right)
{
	if (left.name && right.name && *left.name != *right.name)
	{
		return false;
	}

	if (left.props.size() != right.props.size() || bool(left.indexer) != bool(right.indexer))
	{
		return false;
	}

	if (left.indexer && right.indexer)
	{
		if (left.indexer->isReadOnly != right.indexer->isReadOnly)
		{
			return false;
		}

		if (!equals(left.indexer->indexType, right.indexer->indexType) ||
			!equals(left.indexer->indexResultType, right.indexer->indexResultType))
		{
			return false;
		}
	}

	Luau::TableType::Props::const_iterator left_property = left.props.begin();
	Luau::TableType::Props::const_iterator right_property = right.props.begin();

	while (left_property != left.props.end())
	{
		if (left_property->first != right_property->first || !equals_property(left_property->second, right_property->second))
		{
			return false;
		}

		++left_property;
		++right_property;
	}

	return true;
}

bool type_equality::equals_function(const Luau::FunctionType& left, const Luau::FunctionType& right)
{
	if (left.generics.size() != right.generics.size() || left.genericPacks.size() != right.genericPacks.size())
	{
		return false;
	}

	return equals_pack(left.argTypes, right.argTypes) && equals_pack(left.retTypes, right.retTypes);
}

bool type_equality::equals_unordered(const std::vector<Luau::TypeId>& left, const std::vector<Luau::TypeId>& right)
{
	if (left.size() != right.size())
	{
		return false;
	}

	std::vector<bool> matched(right.size(), false);

	for (Luau::TypeId left_part : left)
	{
		bool found = false;

		for (size_t index = 0; index < right.size(); ++index)
		{
			if (matched[index])
			{
				continue;
			}

			if (equals(left_part, right[index]))
			{
				matched[index] = true;
				found = true;
				break;
			}
		}

		if (!found)
		{
			return false;
		}
	}

	return true;
}

bool type_equality::compare(Luau::TypeId left, Luau::TypeId right)
{
	// Luau stores an unbound generic's default as `unknown`, while the checked
	// type that the default produces is often `any`. Inside a default-parameter
	// comparison the two describe the same "no constraint" slot.
	if (any_unknown_equivalent)
	{
		bool left_blank = Luau::get<Luau::AnyType>(left) != nullptr || Luau::get<Luau::UnknownType>(left) != nullptr;
		bool right_blank = Luau::get<Luau::AnyType>(right) != nullptr || Luau::get<Luau::UnknownType>(right) != nullptr;

		if (left_blank && right_blank)
		{
			return true;
		}
	}

	if (Luau::get<Luau::IntersectionType>(left) != nullptr)
	{
		if (Luau::get<Luau::IntersectionType>(right) == nullptr)
		{
			// A union frequently carries one member as an alias and another as the
			// same type with its table intersections flattened into a single table.
			// Treat an intersection of tables as the table it describes.
			if (const Luau::TableType* right_table = Luau::get<Luau::TableType>(right))
			{
				Luau::TableType merged;
				std::set<Luau::TypeId> active;

				if (merge_table(left, merged, active) && equals_table(merged, *right_table))
				{
					return true;
				}
			}

			return false;
		}

		Luau::TableType left_merged;
		Luau::TableType right_merged;
		std::set<Luau::TypeId> left_active;
		std::set<Luau::TypeId> right_active;

		if (merge_table(left, left_merged, left_active) && merge_table(right, right_merged, right_active) &&
			equals_table(left_merged, right_merged))
		{
			return true;
		}

		std::vector<Luau::TypeId> left_parts;
		std::vector<Luau::TypeId> right_parts;
		flatten_intersection(left, left_parts);
		flatten_intersection(right, right_parts);
		return equals_unordered(left_parts, right_parts);
	}

	if (Luau::get<Luau::IntersectionType>(right) != nullptr)
	{
		if (const Luau::TableType* left_table = Luau::get<Luau::TableType>(left))
		{
			Luau::TableType merged;
			std::set<Luau::TypeId> active;

			if (merge_table(right, merged, active) && equals_table(merged, *left_table))
			{
				return true;
			}
		}

		return false;
	}

	if (const Luau::UnionType* left_union = Luau::get<Luau::UnionType>(left))
	{
		const Luau::UnionType* right_union = Luau::get<Luau::UnionType>(right);
		return right_union != nullptr && equals_unordered(left_union->options, right_union->options);
	}

	if (const Luau::TableType* left_table = Luau::get<Luau::TableType>(left))
	{
		const Luau::TableType* right_table = Luau::get<Luau::TableType>(right);
		return right_table != nullptr && equals_table(*left_table, *right_table);
	}

	if (const Luau::FunctionType* left_function = Luau::get<Luau::FunctionType>(left))
	{
		const Luau::FunctionType* right_function = Luau::get<Luau::FunctionType>(right);
		return right_function != nullptr && equals_function(*left_function, *right_function);
	}

	if (const Luau::MetatableType* left_metatable = Luau::get<Luau::MetatableType>(left))
	{
		const Luau::MetatableType* right_metatable = Luau::get<Luau::MetatableType>(right);

		return right_metatable != nullptr && equals(left_metatable->table, right_metatable->table) &&
			equals(left_metatable->metatable, right_metatable->metatable);
	}

	if (const Luau::PrimitiveType* left_primitive = Luau::get<Luau::PrimitiveType>(left))
	{
		const Luau::PrimitiveType* right_primitive = Luau::get<Luau::PrimitiveType>(right);
		return right_primitive != nullptr && left_primitive->type == right_primitive->type;
	}

	if (const Luau::SingletonType* left_singleton = Luau::get<Luau::SingletonType>(left))
	{
		const Luau::SingletonType* right_singleton = Luau::get<Luau::SingletonType>(right);

		if (right_singleton == nullptr)
		{
			return false;
		}

		if (const Luau::BooleanSingleton* left_boolean = Luau::get_if<Luau::BooleanSingleton>(&left_singleton->variant))
		{
			const Luau::BooleanSingleton* right_boolean = Luau::get_if<Luau::BooleanSingleton>(&right_singleton->variant);
			return right_boolean != nullptr && left_boolean->value == right_boolean->value;
		}

		const Luau::StringSingleton* left_string = Luau::get_if<Luau::StringSingleton>(&left_singleton->variant);
		const Luau::StringSingleton* right_string = Luau::get_if<Luau::StringSingleton>(&right_singleton->variant);
		return left_string != nullptr && right_string != nullptr && left_string->value == right_string->value;
	}

	if (const Luau::NegationType* left_negation = Luau::get<Luau::NegationType>(left))
	{
		const Luau::NegationType* right_negation = Luau::get<Luau::NegationType>(right);
		return right_negation != nullptr && equals(left_negation->ty, right_negation->ty);
	}

	if (const Luau::GenericType* left_generic = Luau::get<Luau::GenericType>(left))
	{
		const Luau::GenericType* right_generic = Luau::get<Luau::GenericType>(right);
		return right_generic != nullptr && !left_generic->name.empty() && left_generic->name == right_generic->name;
	}

	if (const Luau::ExternType* left_extern = Luau::get<Luau::ExternType>(left))
	{
		const Luau::ExternType* right_extern = Luau::get<Luau::ExternType>(right);
		return right_extern != nullptr && left_extern->name == right_extern->name;
	}

	if (const Luau::TypeFunctionInstanceType* left_instance = Luau::get<Luau::TypeFunctionInstanceType>(left))
	{
		const Luau::TypeFunctionInstanceType* right_instance = Luau::get<Luau::TypeFunctionInstanceType>(right);

		if (right_instance == nullptr || left_instance->function != right_instance->function ||
			left_instance->typeArguments.size() != right_instance->typeArguments.size() ||
			left_instance->packArguments.size() != right_instance->packArguments.size())
		{
			return false;
		}

		for (size_t index = 0; index < left_instance->typeArguments.size(); ++index)
		{
			if (!equals(left_instance->typeArguments[index], right_instance->typeArguments[index]))
			{
				return false;
			}
		}

		for (size_t index = 0; index < left_instance->packArguments.size(); ++index)
		{
			if (!equals_pack(left_instance->packArguments[index], right_instance->packArguments[index]))
			{
				return false;
			}
		}

		return true;
	}

	if (Luau::get<Luau::AnyType>(left) != nullptr)
	{
		return Luau::get<Luau::AnyType>(right) != nullptr;
	}

	if (Luau::get<Luau::UnknownType>(left) != nullptr)
	{
		return Luau::get<Luau::UnknownType>(right) != nullptr;
	}

	if (Luau::get<Luau::NeverType>(left) != nullptr)
	{
		return Luau::get<Luau::NeverType>(right) != nullptr;
	}

	if (Luau::get<Luau::NoRefineType>(left) != nullptr)
	{
		return Luau::get<Luau::NoRefineType>(right) != nullptr;
	}

	return Luau::get<Luau::ErrorType>(left) != nullptr && Luau::get<Luau::ErrorType>(right) != nullptr;
}

bool is_primitive_like(Luau::TypeId type)
{
	type = Luau::follow(type);

	return Luau::get<Luau::AnyType>(type) != nullptr || Luau::get<Luau::UnknownType>(type) != nullptr ||
		Luau::get<Luau::NeverType>(type) != nullptr || Luau::get<Luau::NoRefineType>(type) != nullptr ||
		Luau::get<Luau::ErrorType>(type) != nullptr || Luau::get<Luau::PrimitiveType>(type) != nullptr;
}

bool is_same_type(Luau::TypeId left, Luau::TypeId right)
{
	if (left == right)
	{
		return true;
	}

	left = Luau::follow(left);
	right = Luau::follow(right);

	if (left == right)
	{
		return true;
	}

	type_equality equality;
	return equality.equals(left, right);
}

bool is_same_type(Luau::TypeId left, Luau::TypeId right, const Luau::TypeFun& definition)
{
	type_equality equality;

	for (const Luau::GenericTypeDefinition& parameter : definition.typeParams)
	{
		if (!parameter.defaultValue)
		{
			continue;
		}

		Luau::TypeId generic = Luau::follow(parameter.ty);
		Luau::TypeId value = Luau::follow(*parameter.defaultValue);
		equality.type_substitutions[generic] = value;

		if (const Luau::GenericType* named = Luau::get<Luau::GenericType>(generic))
		{
			equality.named_type_substitutions[named->name] = value;
		}
	}

	for (const Luau::GenericTypePackDefinition& parameter : definition.typePackParams)
	{
		if (!parameter.defaultValue)
		{
			continue;
		}

		Luau::TypePackId generic = Luau::follow(parameter.tp);
		Luau::TypePackId value = Luau::follow(*parameter.defaultValue);
		equality.pack_substitutions[generic] = value;

		if (const Luau::GenericTypePack* named = Luau::get<Luau::GenericTypePack>(generic))
		{
			equality.named_pack_substitutions[named->name] = value;
		}
	}

	if (equality.type_substitutions.empty() && equality.pack_substitutions.empty())
	{
		return is_same_type(left, right);
	}

	equality.any_unknown_equivalent = true;
	return equality.equals(left, right);
}

enum class shallow_kind
{
	other,
	table,
	function,
	union_type,
	metatable,
	primitive,
	singleton,
	generic,
	extern_type,
	type_function,
	any_type,
	unknown_type,
	never_type,
	norefine,
	blocked,
	pending,
	free_type,
	lazy
};

shallow_kind get_shallow_kind(Luau::TypeId type)
{
	if (Luau::get<Luau::TableType>(type) != nullptr)
	{
		return shallow_kind::table;
	}

	if (Luau::get<Luau::FunctionType>(type) != nullptr)
	{
		return shallow_kind::function;
	}

	if (Luau::get<Luau::UnionType>(type) != nullptr)
	{
		return shallow_kind::union_type;
	}

	if (Luau::get<Luau::MetatableType>(type) != nullptr)
	{
		return shallow_kind::metatable;
	}

	if (Luau::get<Luau::PrimitiveType>(type) != nullptr)
	{
		return shallow_kind::primitive;
	}

	if (Luau::get<Luau::SingletonType>(type) != nullptr)
	{
		return shallow_kind::singleton;
	}

	if (Luau::get<Luau::GenericType>(type) != nullptr)
	{
		return shallow_kind::generic;
	}

	if (Luau::get<Luau::ExternType>(type) != nullptr)
	{
		return shallow_kind::extern_type;
	}

	if (Luau::get<Luau::TypeFunctionInstanceType>(type) != nullptr)
	{
		return shallow_kind::type_function;
	}

	if (Luau::get<Luau::AnyType>(type) != nullptr)
	{
		return shallow_kind::any_type;
	}

	if (Luau::get<Luau::UnknownType>(type) != nullptr)
	{
		return shallow_kind::unknown_type;
	}

	if (Luau::get<Luau::NeverType>(type) != nullptr)
	{
		return shallow_kind::never_type;
	}

	if (Luau::get<Luau::NoRefineType>(type) != nullptr)
	{
		return shallow_kind::norefine;
	}

	if (Luau::get<Luau::BlockedType>(type) != nullptr)
	{
		return shallow_kind::blocked;
	}

	if (Luau::get<Luau::PendingExpansionType>(type) != nullptr)
	{
		return shallow_kind::pending;
	}

	if (Luau::get<Luau::FreeType>(type) != nullptr)
	{
		return shallow_kind::free_type;
	}

	if (Luau::get<Luau::LazyType>(type) != nullptr)
	{
		return shallow_kind::lazy;
	}

	return shallow_kind::other;
}

bool may_be_same_type(Luau::TypeId left, Luau::TypeId right)
{
	left = Luau::follow(left);
	right = Luau::follow(right);

	if (left == right)
	{
		return true;
	}

	const Luau::IntersectionType* left_intersection = Luau::get<Luau::IntersectionType>(left);
	const Luau::IntersectionType* right_intersection = Luau::get<Luau::IntersectionType>(right);

	if (left_intersection != nullptr || right_intersection != nullptr)
	{
		if (left_intersection != nullptr && right_intersection != nullptr)
		{
			// `A & B` and `C & D` can name the same table when the intersections are
			// split differently (a nameable part + inline fields vs one inline table).
			// Let the structural comparison decide instead of requiring identical parts.
			return true;
		}

		// An intersection of tables has the same shape as one merged table; let the
		// structural comparison below decide instead of rejecting it by part count.
		return Luau::get<Luau::TableType>(left_intersection != nullptr ? right : left) != nullptr;
	}

	shallow_kind left_kind = get_shallow_kind(left);
	shallow_kind right_kind = get_shallow_kind(right);

	if (left_kind != right_kind)
	{
		return false;
	}

	switch (left_kind)
	{
	case shallow_kind::table:
	{
		const Luau::TableType* left_table = Luau::get<Luau::TableType>(left);
		const Luau::TableType* right_table = Luau::get<Luau::TableType>(right);

		if (left_table->name && right_table->name && *left_table->name != *right_table->name)
		{
			return false;
		}

		if (left_table->props.size() != right_table->props.size() || bool(left_table->indexer) != bool(right_table->indexer))
		{
			return false;
		}

		Luau::TableType::Props::const_iterator left_property = left_table->props.begin();
		Luau::TableType::Props::const_iterator right_property = right_table->props.begin();

		while (left_property != left_table->props.end())
		{
			if (left_property->first != right_property->first)
			{
				return false;
			}

			++left_property;
			++right_property;
		}

		return true;
	}
	case shallow_kind::union_type:
	{
		const Luau::UnionType* left_union = Luau::get<Luau::UnionType>(left);
		const Luau::UnionType* right_union = Luau::get<Luau::UnionType>(right);

		if (left_union->options.size() != right_union->options.size())
		{
			return false;
		}

		std::vector<shallow_kind> left_options;
		std::vector<shallow_kind> right_options;
		left_options.reserve(left_union->options.size());
		right_options.reserve(right_union->options.size());

		for (Luau::TypeId option : left_union->options)
		{
			left_options.push_back(get_shallow_kind(Luau::follow(option)));
		}

		for (Luau::TypeId option : right_union->options)
		{
			right_options.push_back(get_shallow_kind(Luau::follow(option)));
		}

		std::sort(left_options.begin(), left_options.end());
		std::sort(right_options.begin(), right_options.end());
		return left_options == right_options;
	}
	case shallow_kind::function:
	{
		const Luau::FunctionType* left_function = Luau::get<Luau::FunctionType>(left);
		const Luau::FunctionType* right_function = Luau::get<Luau::FunctionType>(right);
		return left_function->generics.size() == right_function->generics.size() &&
			left_function->genericPacks.size() == right_function->genericPacks.size();
	}
	case shallow_kind::type_function:
	{
		const Luau::TypeFunctionInstanceType* left_instance = Luau::get<Luau::TypeFunctionInstanceType>(left);
		const Luau::TypeFunctionInstanceType* right_instance = Luau::get<Luau::TypeFunctionInstanceType>(right);
		return left_instance->function == right_instance->function &&
			left_instance->typeArguments.size() == right_instance->typeArguments.size() &&
			left_instance->packArguments.size() == right_instance->packArguments.size();
	}
	default:
		return true;
	}
}

void collect_origin_modules(Luau::TypeId type, std::set<Luau::ModuleName>& modules)
{
	std::set<Luau::TypeId> visited;
	std::vector<Luau::TypeId> pending{type};

	while (!pending.empty())
	{
		Luau::TypeId current = Luau::follow(pending.back());
		pending.pop_back();

		if (!visited.insert(current).second)
		{
			continue;
		}

		if (const Luau::TableType* table = Luau::get<Luau::TableType>(current))
		{
			if (!table->definitionModuleName.empty())
			{
				modules.insert(table->definitionModuleName);
			}

			continue;
		}

		if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(current))
		{
			if (!extern_type->definitionModuleName.empty())
			{
				modules.insert(extern_type->definitionModuleName);
			}

			continue;
		}

		if (const Luau::UnionType* union_type = Luau::get<Luau::UnionType>(current))
		{
			pending.insert(pending.end(), union_type->options.begin(), union_type->options.end());
			continue;
		}

		if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(current))
		{
			pending.insert(pending.end(), intersection->parts.begin(), intersection->parts.end());
			continue;
		}

		if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(current))
		{
			pending.push_back(metatable->table);
			pending.push_back(metatable->metatable);
			continue;
		}

		if (const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(current))
		{
			if (function->definition && function->definition->definitionModuleName)
			{
				modules.insert(*function->definition->definitionModuleName);
			}

			continue;
		}

		if (const Luau::TypeFunctionInstanceType* instance = Luau::get<Luau::TypeFunctionInstanceType>(current))
		{
			pending.insert(pending.end(), instance->typeArguments.begin(), instance->typeArguments.end());
		}
	}
}

void collect_definition_locations(Luau::TypeId type, std::vector<location_info>& locations)
{
	std::set<Luau::TypeId> visited;
	std::function<void(Luau::TypeId)> visit = [&](Luau::TypeId current)
	{
		current = Luau::follow(current);

		if (locations.size() >= 64 || !visited.insert(current).second)
		{
			return;
		}

		if (const Luau::TableType* table = Luau::get<Luau::TableType>(current))
		{
			if (!table->definitionModuleName.empty() && table->definitionLocation.begin.line != 0)
			{
				locations.push_back({table->definitionModuleName, table->definitionLocation});
			}

			return;
		}

		if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(current))
		{
			if (!extern_type->definitionModuleName.empty() && extern_type->definitionLocation)
			{
				locations.push_back({extern_type->definitionModuleName, *extern_type->definitionLocation});
			}

			return;
		}

		if (const Luau::FunctionType* function = Luau::get<Luau::FunctionType>(current))
		{
			if (function->definition && function->definition->definitionModuleName)
			{
				locations.push_back({*function->definition->definitionModuleName, function->definition->definitionLocation});
			}

			return;
		}

		if (const Luau::UnionType* union_type = Luau::get<Luau::UnionType>(current))
		{
			for (Luau::TypeId option : union_type->options)
			{
				visit(option);
			}

			return;
		}

		if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(current))
		{
			for (Luau::TypeId part : intersection->parts)
			{
				visit(part);
			}

			return;
		}

		if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(current))
		{
			visit(metatable->table);
			visit(metatable->metatable);
			return;
		}
	};

	visit(type);
}

bool position_before(const Luau::Position& left, const Luau::Position& right)
{
	return left.line < right.line || (left.line == right.line && left.column <= right.column);
}

bool location_contains(const Luau::Location& outer, const Luau::Location& inner)
{
	return position_before(outer.begin, inner.begin) && position_before(inner.end, outer.end);
}

int definition_location_score(
	const Luau::ModuleName& module, const std::optional<Luau::Location>& location, const std::vector<location_info>& locations)
{
	if (!location)
	{
		return 0;
	}

	int best = 0;

	for (size_t index = 0; index < locations.size(); ++index)
	{
		const location_info& candidate = locations[index];

		if (candidate.module != module || !location_contains(*location, candidate.location))
		{
			continue;
		}

		int begin_overhead = static_cast<int>(candidate.location.begin.line) - static_cast<int>(location->begin.line);
		int end_overhead = static_cast<int>(location->end.line) - static_cast<int>(candidate.location.end.line);
		int overhead = std::max(0, begin_overhead) + std::max(0, end_overhead);
		int value = 150 - std::min(90, overhead * 6);

		// The first collected location belongs to the outermost part of the type,
		// so an alias that starts there is a better description than one wrapping a nested part.
		value += index == 0 ? 60 : std::max(0, 30 - static_cast<int>(index) * 4);

		best = std::max(best, value);
	}

	return best;
}

const Luau::AstExpr* find_initializer(const Luau::SourceModule& source, const Luau::AstLocal* local)
{
	if (local == nullptr)
	{
		return nullptr;
	}

	std::vector<Luau::AstNode*> ancestry = Luau::findAstAncestryOfPosition(source, local->location.begin, true);

	for (Luau::AstNode* node : ancestry)
	{
		Luau::AstStatLocal* statement = node->as<Luau::AstStatLocal>();

		if (statement == nullptr)
		{
			continue;
		}

		for (size_t index = 0; index < statement->vars.size && index < statement->values.size; ++index)
		{
			if (statement->vars.data[index] == local)
			{
				return statement->values.data[index];
			}
		}
	}

	return nullptr;
}

bool contains_generic_reference(const Luau::ScopePtr& scope, const Luau::AstTypePack* pack)
{
	if (pack == nullptr)
	{
		return false;
	}

	if (const Luau::AstTypePackExplicit* explicit_pack = pack->as<Luau::AstTypePackExplicit>())
	{
		for (Luau::AstType* type : explicit_pack->typeList.types)
		{
			if (contains_generic_reference(scope, type))
			{
				return true;
			}
		}

		return contains_generic_reference(scope, explicit_pack->typeList.tailType);
	}

	if (const Luau::AstTypePackVariadic* variadic = pack->as<Luau::AstTypePackVariadic>())
	{
		return contains_generic_reference(scope, variadic->variadicType);
	}

	if (pack->as<Luau::AstTypePackGeneric>() != nullptr)
	{
		return true;
	}

	return false;
}

bool contains_generic_reference(const Luau::ScopePtr& scope, const Luau::AstType* annotation)
{
	if (annotation == nullptr)
	{
		return false;
	}

	if (const Luau::AstTypeReference* reference = annotation->as<Luau::AstTypeReference>())
	{
		std::optional<Luau::TypeFun> definition = reference->prefix
			? scope->lookupImportedType(reference->prefix->value, reference->name.value)
			: scope->lookupType(reference->name.value);

		if (definition && Luau::get<Luau::GenericType>(Luau::follow(definition->type)) != nullptr)
		{
			return true;
		}

		for (const Luau::AstTypeOrPack& parameter : reference->parameters)
		{
			if (contains_generic_reference(scope, parameter.type) || contains_generic_reference(scope, parameter.typePack))
			{
				return true;
			}
		}

		return false;
	}

	if (const Luau::AstTypeTable* table = annotation->as<Luau::AstTypeTable>())
	{
		for (const Luau::AstTableProp& property : table->props)
		{
			if (contains_generic_reference(scope, property.type))
			{
				return true;
			}
		}

		if (table->indexer != nullptr)
		{
			return contains_generic_reference(scope, table->indexer->indexType) ||
				contains_generic_reference(scope, table->indexer->resultType);
		}

		return false;
	}

	if (const Luau::AstTypeFunction* function = annotation->as<Luau::AstTypeFunction>())
	{
		for (Luau::AstType* argument : function->argTypes.types)
		{
			if (contains_generic_reference(scope, argument))
			{
				return true;
			}
		}

		return contains_generic_reference(scope, function->argTypes.tailType) || contains_generic_reference(scope, function->returnTypes);
	}

	if (const Luau::AstTypeUnion* union_type = annotation->as<Luau::AstTypeUnion>())
	{
		for (Luau::AstType* option : union_type->types)
		{
			if (contains_generic_reference(scope, option))
			{
				return true;
			}
		}

		return false;
	}

	if (const Luau::AstTypeIntersection* intersection = annotation->as<Luau::AstTypeIntersection>())
	{
		for (Luau::AstType* part : intersection->types)
		{
			if (contains_generic_reference(scope, part))
			{
				return true;
			}
		}

		return false;
	}

	if (const Luau::AstTypeGroup* group = annotation->as<Luau::AstTypeGroup>())
	{
		return contains_generic_reference(scope, group->type);
	}

	return false;
}

std::optional<named_type> get_reference_alias(const Luau::ScopePtr& scope, const Luau::AstType* annotation)
{
	if (annotation == nullptr)
	{
		return std::nullopt;
	}

	if (const Luau::AstTypeUnion* union_type = annotation->as<Luau::AstTypeUnion>())
	{
		const Luau::AstType* option = nullptr;
		bool optional = false;

		for (Luau::AstType* part : union_type->types)
		{
			if (part->is<Luau::AstTypeOptional>() ||
				(part->as<Luau::AstTypeReference>() != nullptr && !part->as<Luau::AstTypeReference>()->prefix &&
				 part->as<Luau::AstTypeReference>()->name == "nil"))
			{
				optional = true;
				continue;
			}

			if (option != nullptr)
			{
				return std::nullopt;
			}

			option = part;
		}

		if (option == nullptr)
		{
			return std::nullopt;
		}

		std::optional<named_type> alias = get_reference_alias(scope, option);

		if (alias && optional)
		{
			alias->display = (alias->display.empty() ? alias->name : alias->display) + "?";
		}

		return alias;
	}

	const Luau::AstTypeReference* reference = annotation->as<Luau::AstTypeReference>();

	if (reference == nullptr || reference->hasParameterList)
	{
		return std::nullopt;
	}

	std::optional<Luau::TypeFun> definition = reference->prefix ? scope->lookupImportedType(reference->prefix->value, reference->name.value)
																: scope->lookupType(reference->name.value);

	if (!definition)
	{
		return std::nullopt;
	}

	if (Luau::get<Luau::GenericType>(Luau::follow(definition->type)) != nullptr)
	{
		return std::nullopt;
	}

	for (const Luau::GenericTypeDefinition& parameter : definition->typeParams)
	{
		if (!parameter.defaultValue)
		{
			return std::nullopt;
		}
	}

	for (const Luau::GenericTypePackDefinition& parameter : definition->typePackParams)
	{
		if (!parameter.defaultValue)
		{
			return std::nullopt;
		}
	}

	std::string name = reference->prefix ? std::string(reference->prefix->value) + "." + reference->name.value : reference->name.value;
	return named_type{name, *definition};
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

std::optional<Luau::Property> find_property(Luau::TypeId owner, const std::string& name, Luau::TypeId& base)
{
	Luau::TypeId current = Luau::follow(owner);

	for (int depth = 0; depth < 32; ++depth)
	{
		if (const Luau::TableType* table = Luau::get<Luau::TableType>(current))
		{
			const auto found = table->props.find(name);

			if (found != table->props.end())
			{
				base = current;
				return found->second;
			}

			return std::nullopt;
		}

		if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(current))
		{
			if (const Luau::TableType* table = Luau::get<Luau::TableType>(Luau::follow(metatable->table)))
			{
				const auto found = table->props.find(name);

				if (found != table->props.end())
				{
					base = current;
					return found->second;
				}
			}

			const Luau::TableType* meta = Luau::get<Luau::TableType>(Luau::follow(metatable->metatable));
			const auto index = meta != nullptr ? meta->props.find("__index") : decltype(meta->props.end()){};

			if (meta == nullptr || index == meta->props.end() || !index->second.readTy)
			{
				return std::nullopt;
			}

			current = Luau::follow(*index->second.readTy);
			continue;
		}

		if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(current))
		{
			const auto found = extern_type->props.find(name);

			if (found != extern_type->props.end())
			{
				base = current;
				return found->second;
			}

			if (!extern_type->parent)
			{
				return std::nullopt;
			}

			current = Luau::follow(*extern_type->parent);
			continue;
		}

		if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(current))
		{
			for (Luau::TypeId part : intersection->parts)
			{
				if (std::optional<Luau::Property> property = find_property(part, name, base))
				{
					return property;
				}
			}

			return std::nullopt;
		}

		return std::nullopt;
	}

	return std::nullopt;
}

std::optional<Luau::ModuleName> get_definition_module(Luau::TypeId type)
{
	Luau::TypeId followed = Luau::follow(type);

	if (const Luau::TableType* table = Luau::get<Luau::TableType>(followed); table != nullptr && !table->definitionModuleName.empty())
	{
		return table->definitionModuleName;
	}

	if (const Luau::MetatableType* metatable = Luau::get<Luau::MetatableType>(followed))
	{
		return get_definition_module(metatable->table);
	}

	if (const Luau::ExternType* extern_type = Luau::get<Luau::ExternType>(followed);
		extern_type != nullptr && !extern_type->definitionModuleName.empty())
	{
		return extern_type->definitionModuleName;
	}

	return std::nullopt;
}

std::optional<Luau::ModuleName> find_imported_module(const Luau::ScopePtr& scope, const std::string& alias)
{
	for (Luau::Scope* current = scope.get(); current != nullptr; current = current->parent.get())
	{
		const auto found = current->importedModules.find(alias);

		if (found != current->importedModules.end())
		{
			return found->second;
		}
	}

	return std::nullopt;
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
			// The `function` keyword itself is meaningful: hovering it shows the type
			// of the function it introduces.
			return token.type != Luau::Lexeme::ReservedFunction;
		}

		if (token.type == Luau::Lexeme::Name)
		{
			// `const` and `continue` are contextual Luau keywords and are lexed as names.
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
