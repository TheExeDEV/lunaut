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

bool contains_generic_reference(const Luau::ScopePtr& scope, const Luau::AstTypePack* pack);
bool contains_generic_reference(const Luau::ScopePtr& scope, const Luau::AstType* annotation);

}
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

	if (std::optional<namedtypeinfo> reference = typelookup::get_reference_alias(scope, annotation))
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

	if (typecomparer::is_primitive_like(followed))
	{
		return std::nullopt;
	}

	std::set<Luau::ModuleName> origins;
	typecomparer::collect_origin_modules(followed, origins);
	std::vector<declarationinfo> locations;
	typecomparer::collect_definition_locations(followed, locations);

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

		value += typecomparer::definition_location_score(candidate_module, definition.definitionLocation, locations);

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

			if (!typecomparer::may_be_same_type(followed, candidate_type))
			{
				return false;
			}

			return typecomparer::is_same_type(followed, candidate_type, definition);
		};

		for (const auto& [alias, definition] : candidate_module_ptr->exportedTypeBindings)
		{
			bool same = matches(definition);
			bool located =
				match_locations && typecomparer::definition_location_score(candidate_module, definition.definitionLocation, locations) > 0;

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
				match_locations && typecomparer::definition_location_score(candidate_module, definition.definitionLocation, locations) > 0;

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

	if (typecomparer::is_primitive_like(followed))
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
			typecomparer::flatten_intersection(part, parts);

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

		if (function != nullptr && function->magic)
		{
			return std::nullopt;
		}

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

					if (std::optional<Luau::Property> property = typelookup::find_property(base, index->index.value, base))
					{
						if (property->readTy)
						{
							std::optional<Luau::ModuleName> owner_module = typelookup::get_definition_module(base);

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

	if (typecomparer::is_primitive_like(followed))
	{
		return std::nullopt;
	}

	std::optional<Luau::ModuleName> preferred = typelookup::get_definition_module(followed);
	std::set<Luau::ModuleName> origins;
	typecomparer::collect_origin_modules(followed, origins);
	std::optional<namedtypeinfo> exact;
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
						exact = namedtypeinfo{annotation_name, *definition, annotation_name};
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
		if (!typecomparer::is_same_type(followed, entry.type))
		{
			return std::nullopt;
		}

		if (!origins.empty())
		{
			std::set<Luau::ModuleName> candidate_origins;
			typecomparer::collect_origin_modules(entry.type, candidate_origins);

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

	for (auto node = ancestry.rbegin(); node != ancestry.rend(); ++node)
	{
		Luau::AstExprFunction* expression = (*node)->as<Luau::AstExprFunction>();

		if (expression == nullptr)
		{
			continue;
		}

		if (expression->returnAnnotation == nullptr)
		{
			break;
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

std::optional<Luau::ModuleName> workspace::find_prefix_module(const Luau::ScopePtr& scope, const std::string& prefix) const
{
	if (std::optional<Luau::ModuleName> required = typelookup::find_imported_module(scope, prefix))
	{
		return required;
	}

	const auto found = show_as_module_names.find(prefix);

	if (found == show_as_module_names.end())
	{
		return std::nullopt;
	}

	return found->second;
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

namespace
{
namespace priv
{

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

}
}

}
