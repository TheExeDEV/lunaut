#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

#include "Luau/Ast.h"

#include <cstring>

namespace lunaut
{
namespace
{
namespace priv
{

const std::map<std::string_view, std::string_view> relevant_contexts = {
	{"game/Players/LocalPlayer/PlayerScripts", "game/StarterPlayer/StarterPlayerScripts"},
	{"game/Players/LocalPlayer/PlayerGui", "game/StarterGui"},
	{"game/Players/LocalPlayer/StarterGear", "game/StarterPack"},
};

std::string map_context(const std::string& context);
std::string get_string(const Luau::AstExprConstantString* value);
Luau::ModuleInfo join(const std::string& base, const std::string& remainder, bool optional);
std::optional<std::string> get_ancestor_path(const sourcemap& map, const std::string& context, const std::string& ancestor);

}
}

std::optional<Luau::ModuleInfo> robloxresolver::resolve_expression(
	const sourcemap& map, const Luau::ModuleInfo* context, Luau::AstExpr* node, const virtualizer& to_virtual)
{
	if (Luau::AstExprGlobal* global = node->as<Luau::AstExprGlobal>())
	{
		if (global->name == "game")
		{
			return Luau::ModuleInfo{"game"};
		}

		if (global->name == "script" && context != nullptr)
		{
			if (sourcemap::is_virtual_path(context->name))
			{
				return Luau::ModuleInfo{context->name};
			}

			if (std::optional<std::string> virtual_path = to_virtual(context->name))
			{
				return Luau::ModuleInfo{*virtual_path};
			}
		}

		return std::nullopt;
	}

	if (context == nullptr)
	{
		return std::nullopt;
	}

	if (Luau::AstExprIndexName* index = node->as<Luau::AstExprIndexName>())
	{
		if (std::strcmp(index->index.value, "Parent") == 0)
		{
			if (std::optional<std::string> parent = sourcemap::get_parent_path(context->name))
			{
				return Luau::ModuleInfo{*parent, context->optional};
			}

			return std::nullopt;
		}

		return Luau::ModuleInfo{priv::map_context(context->name) + "/" + index->index.value, context->optional};
	}

	if (Luau::AstExprIndexExpr* index = node->as<Luau::AstExprIndexExpr>())
	{
		if (Luau::AstExprConstantString* value = index->index->as<Luau::AstExprConstantString>())
		{
			return Luau::ModuleInfo{priv::map_context(context->name) + "/" + priv::get_string(value), context->optional};
		}

		return std::nullopt;
	}

	Luau::AstExprCall* call = node->as<Luau::AstExprCall>();

	if (call == nullptr || !call->self || call->args.size < 1)
	{
		return std::nullopt;
	}

	Luau::AstExprConstantString* argument = call->args.data[0]->as<Luau::AstExprConstantString>();
	Luau::AstExprIndexName* method = call->func->as<Luau::AstExprIndexName>();

	if (argument == nullptr || method == nullptr)
	{
		return std::nullopt;
	}

	std::string name = priv::get_string(argument);

	if (method->index == "GetService" && context->name == "game")
	{
		return Luau::ModuleInfo{"game/" + name};
	}

	if (method->index == "WaitForChild" || (method->index == "FindFirstChild" && call->args.size == 1))
	{
		return Luau::ModuleInfo{priv::map_context(context->name) + "/" + name, context->optional};
	}

	if (method->index == "FindFirstAncestor")
	{
		if (std::optional<std::string> ancestor = priv::get_ancestor_path(map, context->name, name))
		{
			return Luau::ModuleInfo{*ancestor, context->optional};
		}
	}

	return std::nullopt;
}

std::optional<Luau::ModuleInfo> robloxresolver::resolve_string(
	const sourcemap& map, const Luau::ModuleInfo* context, const std::string& required)
{
	if (context == nullptr || !sourcemap::is_virtual_path(context->name))
	{
		return std::nullopt;
	}

	std::string remainder = required;

	if (stdx::starts_with(required, "@"))
	{
		size_t slash = required.find('/');
		std::string alias = stdx::to_lower(required.substr(1, slash == std::string::npos ? std::string::npos : slash - 1));
		remainder = slash == std::string::npos ? "" : required.substr(slash + 1);

		if (alias == "game" && map.get_root() != nullptr)
		{
			return priv::join(map.get_root()->virtual_path, remainder, false);
		}

		if (alias == "self")
		{
			return priv::join(context->name, remainder, false);
		}

		return std::nullopt;
	}

	std::optional<std::string> base = sourcemap::get_parent_path(context->name);

	if (!base)
	{
		return std::nullopt;
	}

	std::string path = *base;

	for (const std::string& segment : stdx::split(remainder, '/'))
	{
		if (segment.empty() || segment == ".")
		{
			continue;
		}

		if (segment == "..")
		{
			std::optional<std::string> parent = sourcemap::get_parent_path(path);

			if (!parent)
			{
				return std::nullopt;
			}

			path = *parent;
			continue;
		}

		path += "/" + segment;
	}

	return Luau::ModuleInfo{path};
}

namespace
{
namespace priv
{

std::string map_context(const std::string& context)
{
	const auto found = relevant_contexts.find(context);
	return found == relevant_contexts.end() ? context : std::string(found->second);
}

std::string get_string(const Luau::AstExprConstantString* value)
{
	return std::string(value->value.data, value->value.size);
}

Luau::ModuleInfo join(const std::string& base, const std::string& remainder, bool optional)
{
	if (remainder.empty())
	{
		return Luau::ModuleInfo{base, optional};
	}

	return Luau::ModuleInfo{base + "/" + remainder, optional};
}

std::optional<std::string> get_ancestor_path(const sourcemap& map, const std::string& context, const std::string& ancestor)
{
	if (const sourcenode* node = map.get_by_virtual_path(context))
	{
		if (const sourcenode* found = node->find_ancestor(ancestor))
		{
			return found->virtual_path;
		}

		return std::nullopt;
	}

	std::vector<std::string> parts = stdx::split(context, '/');

	while (parts.size() > 1)
	{
		parts.pop_back();

		if (parts.back() == ancestor)
		{
			std::string path = parts.front();

			for (size_t i = 1; i < parts.size(); ++i)
			{
				path += "/" + parts[i];
			}

			return path;
		}
	}

	return std::nullopt;
}

}
}

}
