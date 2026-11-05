#include "project.hpp"
#include "build.hpp"
#include "fflag_core.hpp"
#include "fflag.hpp"

#include "core.d.hpp"

namespace lunaut
{
namespace
{
namespace priv
{

const std::map<std::string_view, Luau::SourceCode::Type> relevant_script_types = {
	{"ModuleScript", Luau::SourceCode::Type::Module},
	{"Script", Luau::SourceCode::Type::Script},
	{"LocalScript", Luau::SourceCode::Type::Script},
};

bool is_source_file(std::string_view path);

}
}

bool sourcenode::is_script() const
{
	return priv::relevant_script_types.find(class_name) != priv::relevant_script_types.end();
}

std::optional<std::string> sourcenode::get_script_path() const
{
	for (const std::string& path : file_paths)
	{
		if (priv::is_source_file(path))
		{
			return path;
		}
	}

	return std::nullopt;
}

Luau::SourceCode::Type sourcenode::get_source_type() const
{
	const auto found = priv::relevant_script_types.find(class_name);

	if (found == priv::relevant_script_types.end())
	{
		return Luau::SourceCode::Type::None;
	}

	return found->second;
}

const sourcenode* sourcenode::find_child(std::string_view child) const
{
	for (const std::unique_ptr<sourcenode>& node : children)
	{
		if (node->name == child)
		{
			return node.get();
		}
	}

	return nullptr;
}

const sourcenode* sourcenode::find_ancestor(std::string_view ancestor) const
{
	for (const sourcenode* node = parent; node != nullptr; node = node->parent)
	{
		if (node->name == ancestor)
		{
			return node;
		}
	}

	return nullptr;
}

void sourcenode::clear_types() const
{
	types.clear();

	for (const std::unique_ptr<sourcenode>& node : children)
	{
		node->clear_types();
	}
}

namespace
{
namespace priv
{

bool is_source_file(std::string_view path)
{
	return stdx::ends_with(path, ".luau") || stdx::ends_with(path, ".lua");
}

}
}

}
