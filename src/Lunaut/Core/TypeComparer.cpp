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

shallow_kind get_shallow_kind(Luau::TypeId type);
bool position_before(const Luau::Position& left, const Luau::Position& right);
bool location_contains(const Luau::Location& outer, const Luau::Location& inner);

}
}

void typecomparer::flatten_intersection(Luau::TypeId type, std::vector<Luau::TypeId>& parts)
{
	type = Luau::follow(type);

	if (const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(type))
	{
		for (Luau::TypeId part : intersection->parts)
		{
			typecomparer::flatten_intersection(part, parts);
		}

		return;
	}

	parts.push_back(type);
}

bool typecomparer::is_primitive_like(Luau::TypeId type)
{
	type = Luau::follow(type);

	if (const Luau::TableType* table = Luau::get<Luau::TableType>(type))
	{
		return table->props.empty() && !table->indexer && !table->name && !table->syntheticName;
	}

	return Luau::get<Luau::AnyType>(type) != nullptr || Luau::get<Luau::UnknownType>(type) != nullptr ||
		Luau::get<Luau::NeverType>(type) != nullptr || Luau::get<Luau::NoRefineType>(type) != nullptr ||
		Luau::get<Luau::ErrorType>(type) != nullptr || Luau::get<Luau::PrimitiveType>(type) != nullptr;
}

bool typecomparer::is_same_type(Luau::TypeId left, Luau::TypeId right)
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

	priv::type_equality equality;
	return equality.equals(left, right);
}

bool typecomparer::is_same_type(Luau::TypeId left, Luau::TypeId right, const Luau::TypeFun& definition)
{
	priv::type_equality equality;

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
		return typecomparer::is_same_type(left, right);
	}

	equality.any_unknown_equivalent = true;
	return equality.equals(left, right);
}

bool typecomparer::may_be_same_type(Luau::TypeId left, Luau::TypeId right)
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
			return true;
		}

		return Luau::get<Luau::TableType>(left_intersection != nullptr ? right : left) != nullptr;
	}

	priv::shallow_kind left_kind = priv::get_shallow_kind(left);
	priv::shallow_kind right_kind = priv::get_shallow_kind(right);

	if (left_kind != right_kind)
	{
		return false;
	}

	switch (left_kind)
	{
	case priv::shallow_kind::table:
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
	case priv::shallow_kind::union_type:
	{
		const Luau::UnionType* left_union = Luau::get<Luau::UnionType>(left);
		const Luau::UnionType* right_union = Luau::get<Luau::UnionType>(right);

		if (left_union->options.size() != right_union->options.size())
		{
			return false;
		}

		std::vector<priv::shallow_kind> left_options;
		std::vector<priv::shallow_kind> right_options;
		left_options.reserve(left_union->options.size());
		right_options.reserve(right_union->options.size());

		for (Luau::TypeId option : left_union->options)
		{
			left_options.push_back(priv::get_shallow_kind(Luau::follow(option)));
		}

		for (Luau::TypeId option : right_union->options)
		{
			right_options.push_back(priv::get_shallow_kind(Luau::follow(option)));
		}

		std::sort(left_options.begin(), left_options.end());
		std::sort(right_options.begin(), right_options.end());
		return left_options == right_options;
	}
	case priv::shallow_kind::function:
	{
		const Luau::FunctionType* left_function = Luau::get<Luau::FunctionType>(left);
		const Luau::FunctionType* right_function = Luau::get<Luau::FunctionType>(right);
		return left_function->generics.size() == right_function->generics.size() &&
			left_function->genericPacks.size() == right_function->genericPacks.size();
	}
	case priv::shallow_kind::type_function:
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

void typecomparer::collect_origin_modules(Luau::TypeId type, std::set<Luau::ModuleName>& modules)
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

void typecomparer::collect_definition_locations(Luau::TypeId type, std::vector<declarationinfo>& locations)
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

int typecomparer::definition_location_score(
	const Luau::ModuleName& module, const std::optional<Luau::Location>& location, const std::vector<declarationinfo>& locations)
{
	if (!location)
	{
		return 0;
	}

	int best = 0;

	for (size_t index = 0; index < locations.size(); ++index)
	{
		const declarationinfo& candidate = locations[index];

		if (candidate.module != module || !priv::location_contains(*location, candidate.location) ||
			candidate.location.end != location->end)
		{
			continue;
		}

		int begin_overhead = static_cast<int>(candidate.location.begin.line) - static_cast<int>(location->begin.line);
		int end_overhead = static_cast<int>(location->end.line) - static_cast<int>(candidate.location.end.line);
		int overhead = std::max(0, begin_overhead) + std::max(0, end_overhead);
		int value = 150 - std::min(90, overhead * 6);

		value += index == 0 ? 60 : std::max(0, 30 - static_cast<int>(index) * 4);

		best = std::max(best, value);
	}

	return best;
}

namespace
{
namespace priv
{

bool type_equality::equals(Luau::TypeId left, Luau::TypeId right)
{
	left = Luau::follow(left);
	right = Luau::follow(right);

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
		typecomparer::flatten_intersection(left, left_parts);
		typecomparer::flatten_intersection(right, right_parts);
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

bool position_before(const Luau::Position& left, const Luau::Position& right)
{
	return left.line < right.line || (left.line == right.line && left.column <= right.column);
}

bool location_contains(const Luau::Location& outer, const Luau::Location& inner)
{
	return position_before(outer.begin, inner.begin) && position_before(inner.end, outer.end);
}

}
}

}
