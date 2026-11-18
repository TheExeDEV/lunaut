#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include <algorithm>
#include <vector>

namespace lunaut
{
namespace
{
namespace priv
{

bool is_refinement(Luau::TypeId type);
void collect_parts(Luau::TypeId type, std::vector<Luau::TypeId>& parts, bool& refined);

}
}

Luau::TypeId typenormalizer::strip_refinements(Luau::TypeId type)
{
	Luau::TypeId followed = Luau::follow(type);
	const Luau::UnionType* options = Luau::get<Luau::UnionType>(followed);

	if (options == nullptr)
	{
		std::vector<Luau::TypeId> parts;
		bool refined = false;
		priv::collect_parts(followed, parts, refined);
		return refined && parts.size() == 1 ? parts.front() : followed;
	}

	std::optional<std::vector<Luau::TypeId>> shared;
	std::optional<Luau::TypeId> plain;

	for (Luau::TypeId option : options->options)
	{
		std::vector<Luau::TypeId> parts;
		bool refined = false;
		priv::collect_parts(option, parts, refined);

		if (shared && *shared != parts)
		{
			return followed;
		}

		shared = parts;

		if (!refined && !plain)
		{
			plain = Luau::follow(option);
		}
	}

	if (plain)
	{
		return *plain;
	}

	return shared && shared->size() == 1 ? shared->front() : followed;
}

bool typenormalizer::is_refined(Luau::TypeId type)
{
	Luau::TypeId followed = Luau::follow(type);
	std::vector<Luau::TypeId> options = {followed};

	if (const Luau::UnionType* found = Luau::get<Luau::UnionType>(followed))
	{
		options = found->options;
	}

	for (Luau::TypeId option : options)
	{
		std::vector<Luau::TypeId> parts;
		bool refined = false;
		priv::collect_parts(option, parts, refined);

		if (refined)
		{
			return true;
		}
	}

	return false;
}

namespace
{
namespace priv
{

bool is_refinement(Luau::TypeId type)
{
	const Luau::TableType* table = Luau::get<Luau::TableType>(Luau::follow(type));

	if (table == nullptr || table->indexer || table->props.empty())
	{
		return false;
	}

	for (const auto& [name, property] : table->props)
	{
		if (property.writeTy || !property.readTy || !Luau::get<Luau::UnknownType>(Luau::follow(*property.readTy)))
		{
			return false;
		}
	}

	return true;
}

void collect_parts(Luau::TypeId type, std::vector<Luau::TypeId>& parts, bool& refined)
{
	Luau::TypeId followed = Luau::follow(type);
	const Luau::IntersectionType* intersection = Luau::get<Luau::IntersectionType>(followed);

	if (intersection == nullptr)
	{
		if (is_refinement(followed))
		{
			refined = true;
		}
		else if (std::find(parts.begin(), parts.end(), followed) == parts.end())
		{
			parts.push_back(followed);
		}

		return;
	}

	for (Luau::TypeId part : intersection->parts)
	{
		collect_parts(part, parts, refined);
	}

	std::sort(parts.begin(), parts.end());
}

}
}

}
