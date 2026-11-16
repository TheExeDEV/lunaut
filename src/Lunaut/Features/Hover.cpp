#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/AstQuery.h"
#include "Luau/PrettyPrinter.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"

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
};

std::string make_code_block(const std::string& code);
std::optional<std::string> get_type_name(Luau::TypeId type);
std::string to_string_type_fun(const std::string& name, const Luau::TypeFun& definition);
std::string to_string_named_function(
	const Luau::Module& module,
	const Luau::FunctionType& function,
	const std::string& name,
	Luau::AstExpr* expr,
	const Luau::ToStringOptions& base);
std::optional<Luau::Property> find_property(Luau::TypeId owner, const std::string& name, Luau::TypeId& base);
bool is_same_type(Luau::TypeId left, Luau::TypeId right);
void flatten_intersection(Luau::TypeId type, std::vector<Luau::TypeId>& parts);
std::optional<named_type> get_reference_alias(const Luau::ScopePtr& scope, const Luau::AstType* annotation);
std::optional<Luau::ModuleName> get_definition_module(Luau::TypeId type);
std::optional<Luau::ModuleName> find_imported_module(const Luau::ScopePtr& scope, const std::string& alias);
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
		std::string type_name;

		if (std::optional<Luau::TypeFun> definition = find_return_alias(*type, type_name))
		{
			value_alias = priv::named_type{type_name, *definition};
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

	Luau::ToStringOptions display_options = options;
	display_options.exhaustive = false;

	std::string text = Luau::toString(*type, options);
	std::string display_text = Luau::toString(*type, display_options);
	const std::string display = value_alias ? value_alias->name : display_text;

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
		text = priv::make_code_block(priv::to_string_named_function(*module, *function, function_name, target.getExpr(), options));
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

std::optional<Luau::ModuleName> workspace::find_definition_module(const Luau::Location& location) const
{
	for (const auto& [name, source_node] : frontend.sourceNodes)
	{
		Luau::ModulePtr module = frontend.moduleResolver.getModule(name);

		if (!module)
		{
			continue;
		}

		for (const auto& [alias, definition] : module->exportedTypeBindings)
		{
			if (definition.definitionLocation && *definition.definitionLocation == location)
			{
				return name;
			}
		}
	}

	for (const auto& [name, module] : definition_modules)
	{
		if (!module)
		{
			continue;
		}

		for (const auto& [alias, definition] : module->exportedTypeBindings)
		{
			if (definition.definitionLocation && *definition.definitionLocation == location)
			{
				return name;
			}
		}
	}

	return std::nullopt;
}

std::optional<Luau::TypeFun> workspace::find_type_alias(Luau::TypeId type, Luau::ModuleName& module, std::string& name) const
{
	Luau::TypeId followed = Luau::follow(type);

	for (const auto& [source_name, source_node] : frontend.sourceNodes)
	{
		Luau::ModulePtr source_module = frontend.moduleResolver.getModule(source_name);

		if (!source_module)
		{
			continue;
		}

		for (const auto& [alias, definition] : source_module->exportedTypeBindings)
		{
			if (priv::is_same_type(followed, Luau::follow(definition.type)))
			{
				module = source_name;
				name = alias;
				return definition;
			}
		}
	}

	for (const auto& [definition_name, definition_module] : definition_modules)
	{
		if (!definition_module)
		{
			continue;
		}

		for (const auto& [alias, definition] : definition_module->exportedTypeBindings)
		{
			if (priv::is_same_type(followed, Luau::follow(definition.type)))
			{
				module = definition_name;
				name = alias;
				return definition;
			}
		}
	}

	return std::nullopt;
}

std::optional<Luau::TypeFun> workspace::find_return_alias(Luau::TypeId type, std::string& name) const
{
	Luau::TypeId followed = Luau::follow(type);

	struct candidate
	{
		Luau::ModulePtr module;
		const Luau::AstType* annotation;
		Luau::TypeId type;
	};

	std::vector<candidate> candidates;

	for (const auto& [module_name, source_node] : frontend.sourceNodes)
	{
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

				if (std::optional<priv::named_type> alias = priv::get_reference_alias(scope, expression->local->annotation))
				{
					name = alias->name;
					return alias->definition;
				}
			}
			else
			{
				candidates.push_back({module, expression->local->annotation, annotation_type});
			}
		}
	}

	for (const candidate& entry : candidates)
	{
		if (!priv::is_same_type(followed, entry.type))
		{
			continue;
		}

		Luau::ScopePtr scope = Luau::findScopeAtPosition(*entry.module, entry.annotation->location.begin);

		if (std::optional<priv::named_type> alias = priv::get_reference_alias(scope, entry.annotation))
		{
			name = alias->name;
			return alias->definition;
		}
	}

	return std::nullopt;
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

std::optional<std::string>
workspace::get_type_alias_name(Luau::TypeId type, const Luau::ScopePtr& scope, const Luau::AstType* annotation) const
{
	if (std::optional<priv::named_type> reference = priv::get_reference_alias(scope, annotation))
	{
		return reference->name;
	}

	std::string name;

	if (find_return_alias(type, name))
	{
		return name;
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

bool is_same_type(Luau::TypeId left, Luau::TypeId right)
{
	if (left == right)
	{
		return true;
	}

	std::vector<Luau::TypeId> left_parts;
	std::vector<Luau::TypeId> right_parts;
	flatten_intersection(left, left_parts);
	flatten_intersection(right, right_parts);

	if (left_parts.empty() || left_parts.size() != right_parts.size())
	{
		return false;
	}

	Luau::ToStringOptions options;
	options.hideTableAliasExpansions = true;

	std::vector<bool> matched(right_parts.size(), false);

	for (Luau::TypeId left_part : left_parts)
	{
		bool found = false;

		for (size_t index = 0; index < right_parts.size(); ++index)
		{
			if (matched[index])
			{
				continue;
			}

			if (left_part == right_parts[index] || Luau::toString(left_part, options) == Luau::toString(right_parts[index], options))
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

std::optional<named_type> get_reference_alias(const Luau::ScopePtr& scope, const Luau::AstType* annotation)
{
	if (annotation == nullptr)
	{
		return std::nullopt;
	}

	const Luau::AstTypeReference* reference = annotation->as<Luau::AstTypeReference>();

	if (reference == nullptr || reference->hasParameterList)
	{
		return std::nullopt;
	}

	std::optional<Luau::TypeFun> definition = reference->prefix ? scope->lookupImportedType(reference->prefix->value, reference->name.value)
																: scope->lookupType(reference->name.value);

	if (!definition || !definition->typeParams.empty() || !definition->typePackParams.empty())
	{
		return std::nullopt;
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

std::string to_string_named_function(
	const Luau::Module& module,
	const Luau::FunctionType& function,
	const std::string& name,
	Luau::AstExpr* expr,
	const Luau::ToStringOptions& base)
{
	Luau::ToStringOptions options = base;
	options.useLineBreaks = false;

	auto render = [&function, &options]()
	{
		std::string text = Luau::toStringNamedFunction("", function, options);
		size_t found = 0;

		while ((found = text.find("_: ", found)) != std::string::npos)
		{
			text.erase(found, 3);
		}

		return text;
	};

	if (!name.empty())
	{
		return "function " + name + render();
	}

	if (expr == nullptr || expr->is<Luau::AstExprGroup>() || expr->is<Luau::AstExprFunction>())
	{
		return "function" + render();
	}

	if (Luau::AstExprLocal* local = expr->as<Luau::AstExprLocal>())
	{
		return "function " + std::string(local->local->name.value) + render();
	}

	if (Luau::AstExprGlobal* global = expr->as<Luau::AstExprGlobal>())
	{
		return "function " + std::string(global->name.value) + render();
	}

	const Luau::TypeId* parent = nullptr;
	std::string method;
	std::string base_name;

	if (Luau::AstExprIndexName* index = expr->as<Luau::AstExprIndexName>())
	{
		parent = module.astTypes.find(index->expr);
		method = std::string(1, index->op) + index->index.value;
		options.hideFunctionSelfArgument = index->op == ':';
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
		return "function" + method + render();
	}

	if (std::optional<std::string> type_name = get_type_name(*parent))
	{
		base_name = *type_name;
	}

	return "function " + base_name + method + render();
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
