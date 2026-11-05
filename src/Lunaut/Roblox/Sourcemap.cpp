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

constexpr std::string_view datamodel_root = "game";
constexpr std::string_view project_root = "ProjectRoot";

}
}

result<nothing> sourcemap::load_from_file(const std::filesystem::path& file, const std::filesystem::path& base)
{
	std::optional<std::string> contents = stdx::read_file(file);

	if (!contents)
	{
		return result<nothing>::fail("cannot read sourcemap: " + file.generic_string());
	}

	return load_from_string(*contents, base);
}

result<nothing> sourcemap::load_from_string(const std::string& contents, const std::filesystem::path& base)
{
	clear();

	try
	{
		json data = json::parse(contents);
		root = parse_node(data, nullptr, base);
	}
	catch (const std::exception& error)
	{
		clear();
		return result<nothing>::fail(std::string("malformed sourcemap: ") + error.what());
	}

	if (!root)
	{
		return result<nothing>::fail("sourcemap has no root node");
	}

	add_node(root.get());
	return result<nothing>::ok({});
}

void sourcemap::clear()
{
	root.reset();
	virtual_paths.clear();
	real_paths.clear();
}

bool sourcemap::is_loaded() const
{
	return root != nullptr;
}

size_t sourcemap::count() const
{
	return virtual_paths.size();
}

const sourcenode* sourcemap::get_root() const
{
	return root.get();
}

const sourcenode* sourcemap::get_by_virtual_path(const std::string& path) const
{
	const auto found = virtual_paths.find(path);
	return found == virtual_paths.end() ? nullptr : found->second;
}

const sourcenode* sourcemap::get_by_real_path(const std::string& path) const
{
	const auto found = real_paths.find(uri::normalize_path(path));
	return found == real_paths.end() ? nullptr : found->second;
}

std::optional<std::string> sourcemap::get_real_path(const std::string& virtual_path) const
{
	const sourcenode* node = get_by_virtual_path(virtual_path);

	if (node == nullptr)
	{
		return std::nullopt;
	}

	return node->get_script_path();
}

std::optional<std::string> sourcemap::get_virtual_path(const std::string& real_path) const
{
	const sourcenode* node = get_by_real_path(real_path);

	if (node == nullptr)
	{
		return std::nullopt;
	}

	return node->virtual_path;
}

bool sourcemap::is_virtual_path(const std::string& name)
{
	for (std::string_view prefix : {priv::datamodel_root, priv::project_root})
	{
		if (name == prefix || (stdx::starts_with(name, prefix) && name.size() > prefix.size() && name[prefix.size()] == '/'))
		{
			return true;
		}
	}

	return false;
}

std::optional<std::string> sourcemap::get_parent_path(const std::string& name)
{
	size_t slash = name.find_last_of('/');

	if (slash == std::string::npos)
	{
		return std::nullopt;
	}

	return name.substr(0, slash);
}

std::unique_ptr<sourcenode> sourcemap::parse_node(const json& data, sourcenode* parent, const std::filesystem::path& base)
{
	std::unique_ptr<sourcenode> node = std::make_unique<sourcenode>();
	node->parent = parent;
	node->name = data.value("name", "");
	node->class_name = data.value("className", "Instance");

	if (parent == nullptr)
	{
		node->virtual_path = node->class_name == "DataModel" ? std::string(priv::datamodel_root) : std::string(priv::project_root);
	}
	else
	{
		node->virtual_path = parent->virtual_path + "/" + node->name;
	}

	if (data.contains("filePaths") && data["filePaths"].is_array())
	{
		for (const json& path : data["filePaths"])
		{
			if (path.is_string())
			{
				node->file_paths.push_back(uri::normalize_path((base / path.get<std::string>()).generic_string()));
			}
		}
	}

	if (data.contains("children") && data["children"].is_array())
	{
		for (const json& child : data["children"])
		{
			node->children.push_back(parse_node(child, node.get(), base));
		}
	}

	return node;
}

void sourcemap::add_node(const sourcenode* node)
{
	virtual_paths.emplace(node->virtual_path, node);

	if (std::optional<std::string> path = node->get_script_path())
	{
		real_paths.emplace(*path, node);
	}

	for (const std::unique_ptr<sourcenode>& child : node->children)
	{
		add_node(child.get());
	}
}

}
