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

struct generic_collector final : Luau::TypeOnceVisitor
{
	generic_collector();

	bool visit(Luau::TypeId type, const Luau::GenericType& generic) override;
	bool visit(Luau::TypePackId pack, const Luau::GenericTypePack& generic) override;
	bool visit(Luau::TypeId type, const Luau::ExternType& extern_type) override;

	std::set<const void*> found;
};

std::string make_generic_name(size_t index);
std::string replace_unknown_error(const std::string& text, bool enabled);

}
}

std::string workspace::render_type_display(
	Luau::TypeId type,
	const Luau::ModuleName& module_name,
	const Luau::ScopePtr& scope,
	size_t max_length,
	bool named_function,
	bool hide_self,
	const std::string* return_alias,
	std::vector<typelinkinfo>* links) const
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

				return priv::replace_unknown_error(text, settings.features.unknown_error);
			}
		}

		return priv::replace_unknown_error(Luau::toString(followed, plain), settings.features.unknown_error);
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

	auto record_link = [&](const std::string& name, const Luau::ModuleName& module, const std::optional<Luau::Location>& location)
	{
		if (links == nullptr || !location)
		{
			return;
		}

		for (const typelinkinfo& existing : *links)
		{
			if (existing.module == module && existing.location == *location)
			{
				return;
			}
		}

		links->push_back({name, module, *location});
	};

	auto fallback = [&](Luau::TypeId value)
	{
		return Luau::toString(value, options);
	};
	auto fallback_pack = [&](Luau::TypePackId value)
	{
		return Luau::toString(value, options);
	};

	std::map<const void*, std::string> generic_names;

	auto get_generic_name = [&](const void* key, const std::string& name) -> std::string
	{
		if (!name.empty())
		{
			return name;
		}

		const auto found = generic_names.find(key);

		if (found != generic_names.end())
		{
			return found->second;
		}

		std::string generated = priv::make_generic_name(generic_names.size());
		generic_names.emplace(key, generated);
		return generated;
	};

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

			if (std::optional<Luau::TypeFun> definition = find_type_alias(value, module, name, module_name))
			{
				result = format_alias_display(module, name);
				record_link(name, module, definition->definitionLocation);
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

					if (std::optional<Luau::TypeFun> definition = find_type_alias(only, module, only_name, module_name))
					{
						result = format_alias_display(module, only_name) + (optional ? "?" : "");
						record_link(only_name, module, definition->definitionLocation);
					}
				}
			}
		}

		alias_cache[value] = result;
		return result;
	};

	std::function<std::string(Luau::TypePackId)> render_pack;

	std::function<std::string(Luau::TypeId)> render = [&](Luau::TypeId value) -> std::string
	{
		value = Luau::follow(value);

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
			typecomparer::flatten_intersection(value, parts);

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
				record_link(
					*table->name,
					table->definitionModuleName.empty() ? module_name : table->definitionModuleName,
					table->definitionLocation);

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
			auto [arguments, tail] = Luau::flatten(function->argTypes);
			std::optional<Luau::TypePackId> arguments_tail = visible_tail(tail);
			priv::generic_collector used;

			for (size_t index = 0; index < arguments.size(); ++index)
			{
				if (!(hide_self && depth == 1 && index == 0 && function->hasSelf))
				{
					used.traverse(arguments[index]);
				}
			}

			if (arguments_tail)
			{
				used.traverse(*arguments_tail);
			}

			used.traverse(function->retTypes);
			std::vector<std::string> parameters;

			for (Luau::TypeId generic_id : function->generics)
			{
				Luau::TypeId followed_generic = Luau::follow(generic_id);
				const Luau::GenericType* generic = Luau::get<Luau::GenericType>(followed_generic);

				if (generic != nullptr && used.found.count(followed_generic) != 0)
				{
					parameters.push_back(get_generic_name(followed_generic, generic->name));
				}
			}

			for (Luau::TypePackId generic_id : function->genericPacks)
			{
				Luau::TypePackId followed_generic = Luau::follow(generic_id);
				const Luau::GenericTypePack* generic = Luau::get<Luau::GenericTypePack>(followed_generic);

				if (generic != nullptr && used.found.count(followed_generic) != 0)
				{
					parameters.push_back(get_generic_name(followed_generic, generic->name) + "...");
				}
			}

			if (!parameters.empty())
			{
				result = "<";

				for (size_t index = 0; index < parameters.size(); ++index)
				{
					result += (index == 0 ? "" : ", ") + parameters[index];
				}

				result += ">";
			}

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

				result += render_pack(*arguments_tail);
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

						result += render_pack(*returns_tail);
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
			result = get_generic_name(value, generic->name);
		}
		else
		{
			result = fallback(value);
		}

		active.erase(value);
		--depth;
		return result;
	};

	render_pack = [&](Luau::TypePackId pack) -> std::string
	{
		Luau::TypePackId followed_pack = Luau::follow(pack);

		if (const Luau::VariadicTypePack* variadic = Luau::get<Luau::VariadicTypePack>(followed_pack))
		{
			if (variadic->hidden)
			{
				return std::string();
			}

			std::string inner = render(variadic->ty);
			Luau::TypeId followed_type = Luau::follow(variadic->ty);
			bool wrap = !resolve_alias(followed_type) &&
				(Luau::get<Luau::UnionType>(followed_type) != nullptr || Luau::get<Luau::IntersectionType>(followed_type) != nullptr);

			return "..." + (wrap ? "(" + inner + ")" : inner);
		}

		if (const Luau::GenericTypePack* generic = Luau::get<Luau::GenericTypePack>(followed_pack))
		{
			return get_generic_name(followed_pack, generic->name) + "...";
		}

		if (const Luau::FreeTypePack* free = Luau::get<Luau::FreeTypePack>(followed_pack))
		{
			return "..." + std::to_string(free->index);
		}

		auto [head, tail] = Luau::flatten(pack);
		std::string text;

		for (size_t index = 0; index < head.size(); ++index)
		{
			if (index != 0)
			{
				text += ", ";
			}

			text += render(head[index]);
		}

		if (tail)
		{
			std::string rest = render_pack(*tail);

			if (!rest.empty())
			{
				if (!text.empty())
				{
					text += ", ";
				}

				text += std::move(rest);
			}
		}

		return text;
	};

	std::string result = render(followed);

	if (result.empty())
	{
		result = fallback(followed);
	}

	return priv::replace_unknown_error(result, settings.features.unknown_error);
}

typedisplayinfo workspace::get_type_display(
	Luau::TypeId type,
	const Luau::Module& module,
	const Luau::ScopePtr& scope,
	const Luau::AstType* annotation,
	const Luau::AstExpr* initializer,
	size_t max_length,
	std::vector<typelinkinfo>* links) const
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
					info.text = render_type_display(left, module.name, scope, max_length, false, false, nullptr, links);
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
							info.text = render_type_display(values.front(), module.name, scope, max_length, false, false, nullptr, links);
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

								joined += render_type_display(values[index], module.name, scope, max_length, false, false, nullptr, links);
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
		if (const std::optional<namedtypeinfo> reference = typelookup::get_reference_alias(scope, annotation))
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

		if (links != nullptr && definition.definitionLocation && info.module)
		{
			links->push_back({info.name, *info.module, *definition.definitionLocation});
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

	info.text = render_type_display(followed, module.name, scope, max_length, false, false, nullptr, links);

	if (max_length > 0 && info.text.size() > max_length)
	{
		info.truncated = true;
	}

	return info;
}

std::string workspace::render_type_pack_display(
	Luau::TypePackId pack, const Luau::ModuleName& module_name, const Luau::ScopePtr& scope, size_t max_length) const
{
	Luau::TypePackId followed = Luau::follow(pack);

	if (const Luau::VariadicTypePack* variadic = Luau::get<Luau::VariadicTypePack>(followed))
	{
		if (variadic->hidden)
		{
			return std::string();
		}

		std::string inner = render_type_display(variadic->ty, module_name, scope, max_length);
		Luau::TypeId followed_type = Luau::follow(variadic->ty);
		Luau::ModuleName alias_module;
		std::string alias_name;
		bool aliased = find_type_alias(followed_type, alias_module, alias_name, module_name).has_value();
		bool wrap = !aliased &&
			(Luau::get<Luau::UnionType>(followed_type) != nullptr || Luau::get<Luau::IntersectionType>(followed_type) != nullptr);
		return "..." + (wrap ? "(" + inner + ")" : inner);
	}

	if (const Luau::GenericTypePack* generic = Luau::get<Luau::GenericTypePack>(followed))
	{
		return generic->name.empty() ? Luau::toString(followed) : generic->name + "...";
	}

	if (const Luau::FreeTypePack* free = Luau::get<Luau::FreeTypePack>(followed))
	{
		return "..." + std::to_string(free->index);
	}

	auto [head, tail] = Luau::flatten(followed);
	std::string text;

	for (size_t index = 0; index < head.size(); ++index)
	{
		if (index != 0)
		{
			text += ", ";
		}

		text += render_type_display(head[index], module_name, scope, max_length);
	}

	if (tail)
	{
		std::string rest = render_type_pack_display(*tail, module_name, scope, max_length);

		if (!rest.empty())
		{
			if (!text.empty())
			{
				text += ", ";
			}

			text += std::move(rest);
		}
	}

	bool wrap = tail ? !head.empty() : head.size() > 1;
	return wrap ? "(" + text + ")" : text;
}

typedisplayinfo workspace::get_type_display(
	Luau::TypePackId pack, const Luau::Module& module, const Luau::ScopePtr& scope, size_t max_length) const
{
	typedisplayinfo info;
	info.text = render_type_pack_display(pack, module.name, scope, max_length);
	return info;
}

namespace
{
namespace priv
{

std::string make_generic_name(size_t index)
{
	std::string name(1, static_cast<char>('a' + index % 26));
	return index < 26 ? name : name + std::to_string(index / 26);
}

generic_collector::generic_collector()
	: Luau::TypeOnceVisitor("generic_collector", true)
{
}

bool generic_collector::visit(Luau::TypeId type, const Luau::GenericType& generic)
{
	found.insert(type);
	return true;
}

bool generic_collector::visit(Luau::TypePackId pack, const Luau::GenericTypePack& generic)
{
	found.insert(pack);
	return true;
}

bool generic_collector::visit(Luau::TypeId type, const Luau::ExternType& extern_type)
{
	return false;
}

std::string replace_unknown_error(const std::string& text, bool enabled)
{
	if (!enabled)
	{
		return text;
	}

	constexpr std::string_view marker = "*error-type*";
	std::string result;
	size_t offset = 0;

	while (true)
	{
		size_t found = text.find(marker, offset);

		if (found == std::string::npos)
		{
			result.append(text, offset, std::string::npos);
			break;
		}

		result.append(text, offset, found - offset);
		result += "unknown";
		offset = found + marker.size();
	}

	return result;
}

}
}

}
